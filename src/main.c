#include "os.h"
#include "connection_job.h"
#include "index_arena.h"
#include "slice.h"
#include "connection_guest_bytes.h"
#include "business_guest_bytes.h"
#include "broker_guest_bytes.h"
#include "wasm_pool.h"
#include <stdio.h>
#include <string.h>

enum { CONNECTION_LIMIT = WASM_POOL_CAPACITY };
enum { OP_ACCEPT, OP_RECEIVE, OP_SEND, OP_COUNT };

typedef struct connection {
    indexed_id id;
    u32 worker_index;
    usize business_worker;
    i32 socket;
    i32 job_pending;
    u32 job_type;
    usize input_length;
    byte_slice buffer;
    const_byte_slice pending;
    _Alignas(64) connection_job_context job;
    _Alignas(64) u8 receive_storage[CONNECTION_INPUT_SIZE];
} connection;
typedef struct server {
    index_arena connections;
    connection_runtime *runtime;
    worker_pool *pool;
    worker_pool *business_pool;
    business_runtime *business;
    business_runtime *broker;
    worker_job inline_jobs[CONNECTION_LIMIT];
    usize inline_head, inline_count;
    bool dispatch_via_main;
    usize next_business;
    io_queue *queue;
    i32 listener;
    i32 accept_pending;
    u32 next_worker;
} server;
typedef struct worker_pool_slice { worker_pool **ptr; usize len; } worker_pool_slice;

static i32 ReportError(string_slice operation)
{
    TerminalWrite(operation);
    TerminalWrite(StringSlice("\n"));
    TerminalFlush();
    return 1;
}

static i32 ReportNativeError(string_slice operation, i32 native_error)
{
    TerminalWrite(operation);
    TerminalWrite(StringSlice(": "));
    TerminalWrite(OsErrorText(native_error));
    TerminalWrite(StringSlice("\n"));
    TerminalFlush();
    return 1;
}

static connection *ConnectionGet(server *host, indexed_id id)
{
    byte_slice item;
    return IndexArenaGet(&host->connections, id, &item).tag == INDEX_ARENA_GET_OK ? (connection *)item.ptr : NULL;
}

static u64 IoId(indexed_id id, u32 operation)
{
    return ((u64)id.generation << 32) | ((u64)id.index * OP_COUNT + operation);
}

typedef enum queue_accept_tag { QUEUE_ACCEPT_OK, QUEUE_ACCEPT_INVALID_STATE, QUEUE_ACCEPT_QUEUE_FAILED } queue_accept_tag;
typedef struct queue_accept_result { queue_accept_tag tag; } queue_accept_result;
typedef enum queue_message_tag { QUEUE_MESSAGE_OK, QUEUE_MESSAGE_BUSY, QUEUE_MESSAGE_SUBMIT_FAILED, QUEUE_MESSAGE_DISPATCH_FAILED } queue_message_tag;
typedef struct queue_message_result { queue_message_tag tag; } queue_message_result;
typedef enum close_connection_tag { CLOSE_CONNECTION_OK, CLOSE_CONNECTION_QUEUE_FAILED } close_connection_tag;
typedef struct close_connection_result { close_connection_tag tag; } close_connection_result;
typedef enum handle_request_tag { HANDLE_REQUEST_OK, HANDLE_REQUEST_INVALID_MESSAGE, HANDLE_REQUEST_CLOSE_FAILED, HANDLE_REQUEST_RELEASE_FAILED, HANDLE_REQUEST_IO_FAILED } handle_request_tag;
typedef struct handle_request_result { handle_request_tag tag; } handle_request_result;
typedef enum handle_completion_tag { HANDLE_COMPLETION_OK, HANDLE_COMPLETION_ACCEPT_FAILED, HANDLE_COMPLETION_INVALID_COUNT, HANDLE_COMPLETION_IO_FAILED, HANDLE_COMPLETION_JOB_FAILED, HANDLE_COMPLETION_CLOSE_FAILED } handle_completion_tag;
typedef struct handle_completion_result { handle_completion_tag tag; } handle_completion_result;

static queue_accept_result QueueAccept(server *host)
{
    if (host->accept_pending) return (queue_accept_result){.tag = QUEUE_ACCEPT_OK};
    indexed_id id;
    byte_slice item;
    index_arena_acquire_result acquired = IndexArenaAcquire(&host->connections, &id, &item);
    switch (acquired.tag) {
    case INDEX_ARENA_ACQUIRE_NO_SPACE: return (queue_accept_result){.tag = QUEUE_ACCEPT_OK};
    case INDEX_ARENA_ACQUIRE_INVALID_ARGUMENT: return (queue_accept_result){.tag = QUEUE_ACCEPT_INVALID_STATE};
    case INDEX_ARENA_ACQUIRE_OK: break;
    }
    connection *client = (connection *)item.ptr;
    client->id = id;
    client->socket = -1;
    client->buffer = (byte_slice){client->receive_storage, sizeof(client->receive_storage)};
    if (TcpAccept(host->queue, host->listener, IoId(id, OP_ACCEPT)).tag != TCP_ACCEPT_OK) {
        IndexArenaRelease(&host->connections, id);
        return (queue_accept_result){.tag = QUEUE_ACCEPT_QUEUE_FAILED};
    }
    host->accept_pending = 1;
    return (queue_accept_result){.tag = QUEUE_ACCEPT_OK};
}

static handle_request_result HandleRequest(server *host, worker_result result);

static queue_message_result QueueMessage(server *host, connection *client, u32 type, const_byte_slice input)
{
    if (client->job_pending) return (queue_message_result){.tag = QUEUE_MESSAGE_BUSY};
    worker_job message = {.id = IndexedIdPack(client->id), .function = ConnectionWorkerJob,
        .context = &client->job, .input = input, .type = type};
    client->job_pending = 1;
    client->job_type = type;
    client->input_length = input.len;
    if (host->pool == NULL) {
        handle_request_result handled = HandleRequest(host, ConnectionWorkerJob(message));
        return (queue_message_result){.tag = handled.tag == HANDLE_REQUEST_OK ? QUEUE_MESSAGE_OK : QUEUE_MESSAGE_DISPATCH_FAILED};
    }
    worker_submit_result submitted = WorkerSubmit(host->pool, client->worker_index, message);
    if (submitted.tag != WORKER_SUBMIT_OK) {
        client->job_pending = 0;
        return (queue_message_result){.tag = QUEUE_MESSAGE_SUBMIT_FAILED};
    }
    return (queue_message_result){.tag = QUEUE_MESSAGE_OK};
}

static queue_message_result QueueBusiness(server *host, connection *client, const_byte_slice input)
{
    if (client->job_pending) return (queue_message_result){.tag = QUEUE_MESSAGE_BUSY};
    worker_pool *pool = host->business_pool ? host->business_pool : host->pool;
    worker_job job = {.id = IndexedIdPack(client->id), .function = BusinessWorkerJob,
        .context = &client->job.business, .input = input, .type = BUSINESS_REQUEST};
    client->job_pending = 1;
    client->job_type = BUSINESS_REQUEST;
    if (!pool) {
        if (host->inline_count == CONNECTION_LIMIT) {
            client->job_pending = 0;
            return (queue_message_result){.tag = QUEUE_MESSAGE_SUBMIT_FAILED};
        }
        host->inline_jobs[(host->inline_head + host->inline_count++) % CONNECTION_LIMIT] = job;
        return (queue_message_result){.tag = QUEUE_MESSAGE_OK};
    }
    if (!client->job.business.active) {
        client->business_worker = client->worker_index;
        if (host->dispatch_via_main) {
            worker_select_result selected = WorkerSelect(pool, host->next_business);
            if (selected.tag != WORKER_SELECT_OK) {
                client->job_pending = 0;
                return (queue_message_result){.tag = QUEUE_MESSAGE_SUBMIT_FAILED};
            }
            client->business_worker = selected.index;
            host->next_business = (selected.index + 1) % WorkerPoolCount(pool);
        }
    }
    if (WorkerSubmit(pool, client->business_worker, job).tag != WORKER_SUBMIT_OK) {
        client->job_pending = 0;
        return (queue_message_result){.tag = QUEUE_MESSAGE_SUBMIT_FAILED};
    }
    return (queue_message_result){.tag = QUEUE_MESSAGE_OK};
}

static close_connection_result CloseConnection(server *host, connection *client)
{
    /* Call only after the single pending socket operation or job completes. */
    if (client->socket >= 0) {
        TcpClose(client->socket); /* Linux releases the descriptor even on error. */
        client->socket = -1;
    }
    client->pending = (const_byte_slice){0};
    queue_message_result queued = QueueMessage(host, client, CONNECTION_CLOSE, (const_byte_slice){0});
    return (close_connection_result){.tag = queued.tag == QUEUE_MESSAGE_OK ? CLOSE_CONNECTION_OK : CLOSE_CONNECTION_QUEUE_FAILED};
}

static handle_request_result HandleRequest(server *host, worker_result result)
{
    indexed_id id = IndexedIdUnpack(result.id);
    connection *client = ConnectionGet(host, id);
    if (client == NULL) return (handle_request_result){.tag = HANDLE_REQUEST_OK};
    if (!client->job_pending) return (handle_request_result){.tag = HANDLE_REQUEST_INVALID_MESSAGE};
    client->job_pending = 0;
    if (client->job_type == BUSINESS_REQUEST) {
        workflow_step_result business;
        if (result.type != BUSINESS_RESULT || result.outcome.len != sizeof(business) ||
            ByteSliceCopy((byte_slice){(u8 *)&business, sizeof(business)}, 0, result.outcome).tag != BYTE_SLICE_COPY_OK)
            return (handle_request_result){.tag = HANDLE_REQUEST_INVALID_MESSAGE};
        queue_message_result queued = business.tag == WORKFLOW_STEP_PENDING ?
            QueueBusiness(host, client, (const_byte_slice){0}) : QueueMessage(host, client,
            business.tag == WORKFLOW_STEP_COMPLETE ? CONNECTION_RESPONSE : CONNECTION_BUSINESS_FAILED,
            business.tag == WORKFLOW_STEP_COMPLETE ? business.response : (const_byte_slice){0});
        return (handle_request_result){.tag = queued.tag == QUEUE_MESSAGE_OK ? HANDLE_REQUEST_OK : HANDLE_REQUEST_IO_FAILED};
    }
    connection_job_result outcome;
    if (result.outcome.len != sizeof(outcome) ||
        ByteSliceCopy((byte_slice){(u8 *)&outcome, sizeof(outcome)}, 0, result.outcome).tag != BYTE_SLICE_COPY_OK)
        return (handle_request_result){.tag = HANDLE_REQUEST_INVALID_MESSAGE};
    if (outcome.tag != CONNECTION_JOB_OK) {
        if (client->job_type == CONNECTION_CLOSE) return (handle_request_result){.tag = HANDLE_REQUEST_CLOSE_FAILED};
        close_connection_result closed = CloseConnection(host, client);
        return (handle_request_result){.tag = closed.tag == CLOSE_CONNECTION_OK ? HANDLE_REQUEST_OK : HANDLE_REQUEST_CLOSE_FAILED};
    }
    if (result.type == CONNECTION_CLOSED) {
        if (client->job_type != CONNECTION_CLOSE || client->socket >= 0)
            return (handle_request_result){.tag = HANDLE_REQUEST_INVALID_MESSAGE};
        index_arena_release_result released = IndexArenaRelease(&host->connections, id);
        return (handle_request_result){.tag = released.tag == INDEX_ARENA_RELEASE_OK ? HANDLE_REQUEST_OK : HANDLE_REQUEST_RELEASE_FAILED};
    }
    if (result.type != CONNECTION_REQUEST || result.consumed != client->input_length)
        return (handle_request_result){.tag = HANDLE_REQUEST_INVALID_MESSAGE};
    if (result.flags == CONNECTION_HAS_REQUEST) {
        queue_message_result queued = QueueBusiness(host, client, outcome.delivery.request);
        return (handle_request_result){.tag = queued.tag == QUEUE_MESSAGE_OK ? HANDLE_REQUEST_OK : HANDLE_REQUEST_IO_FAILED};
    }
    if (result.flags == CONNECTION_WANT_CLOSE) {
        close_connection_result closed = CloseConnection(host, client);
        return (handle_request_result){.tag = closed.tag == CLOSE_CONNECTION_OK ? HANDLE_REQUEST_OK : HANDLE_REQUEST_CLOSE_FAILED};
    }
    if (result.flags == CONNECTION_WANT_INPUT) {
        tcp_receive_result queued = TcpReceive(host->queue, client->socket, client->buffer, IoId(id, OP_RECEIVE));
        return (handle_request_result){.tag = queued.tag == TCP_RECEIVE_OK ? HANDLE_REQUEST_OK : HANDLE_REQUEST_IO_FAILED};
    }
    if (result.flags == CONNECTION_WANT_SEND || result.flags == (CONNECTION_WANT_SEND | CONNECTION_HAS_REQUEST)) {
        client->pending = result.data;
        if (client->pending.len == 0) return (handle_request_result){.tag = HANDLE_REQUEST_INVALID_MESSAGE};
        tcp_send_result queued = TcpSend(host->queue, client->socket, client->pending, IoId(id, OP_SEND));
        return (handle_request_result){.tag = queued.tag == TCP_SEND_OK ? HANDLE_REQUEST_OK : HANDLE_REQUEST_IO_FAILED};
    }
    return (handle_request_result){.tag = HANDLE_REQUEST_INVALID_MESSAGE};
}

static handle_completion_result HandleCompletion(server *host, io_completion completion)
{
    u32 low = (u32)completion.id;
    u32 operation = low % OP_COUNT;
    indexed_id id = {low / OP_COUNT, (u32)(completion.id >> 32)};
    connection *client = ConnectionGet(host, id);
    if (client == NULL) {
        if (operation == OP_ACCEPT && completion.tag == IO_COMPLETION_OK) TcpClose((i32)completion.value);
        return (handle_completion_result){.tag = HANDLE_COMPLETION_OK};
    }
    if (operation == OP_ACCEPT) {
        host->accept_pending = 0;
        if (completion.tag != IO_COMPLETION_OK) {
            IndexArenaRelease(&host->connections, id);
            switch (completion.tag) {
            case IO_COMPLETION_INTERRUPTED:
            case IO_COMPLETION_WOULD_BLOCK:
            case IO_COMPLETION_ABORTED:
            case IO_COMPLETION_PROTOCOL_ERROR: return (handle_completion_result){.tag = HANDLE_COMPLETION_OK};
            case IO_COMPLETION_BAD_HANDLE:
            case IO_COMPLETION_SYSTEM_ERROR:
            case IO_COMPLETION_OK: return (handle_completion_result){.tag = HANDLE_COMPLETION_ACCEPT_FAILED};
            }
        }
        client->socket = (i32)completion.value;
        client->worker_index = host->next_worker;
        usize count = WorkerPoolCount(host->pool);
        if (count != 0) host->next_worker = (host->next_worker + 1) % (u32)count;
        client->job.runtime = host->runtime;
        client->job.business.runtime = host->business;
        client->job.business.broker = host->broker;
        client->job.dispatch_via_main = host->dispatch_via_main;
        client->job.id = IndexedIdPack(id);
        queue_message_result queued = QueueMessage(host, client, CONNECTION_OPEN, (const_byte_slice){0});
        return (handle_completion_result){.tag = queued.tag == QUEUE_MESSAGE_OK ? HANDLE_COMPLETION_OK : HANDLE_COMPLETION_JOB_FAILED};
    }
    if (completion.tag == IO_COMPLETION_INTERRUPTED || completion.tag == IO_COMPLETION_WOULD_BLOCK) {
        if (operation == OP_RECEIVE) {
            tcp_receive_result queued = TcpReceive(host->queue, client->socket, client->buffer, completion.id);
            return (handle_completion_result){.tag = queued.tag == TCP_RECEIVE_OK ? HANDLE_COMPLETION_OK : HANDLE_COMPLETION_IO_FAILED};
        }
        tcp_send_result queued = TcpSend(host->queue, client->socket, client->pending, completion.id);
        return (handle_completion_result){.tag = queued.tag == TCP_SEND_OK ? HANDLE_COMPLETION_OK : HANDLE_COMPLETION_IO_FAILED};
    }
    if (completion.tag != IO_COMPLETION_OK || (operation == OP_SEND && completion.value == 0)) {
        close_connection_result closed = CloseConnection(host, client);
        return (handle_completion_result){.tag = closed.tag == CLOSE_CONNECTION_OK ? HANDLE_COMPLETION_OK : HANDLE_COMPLETION_CLOSE_FAILED};
    }
    if (operation == OP_RECEIVE) {
        if (completion.value > client->buffer.len) return (handle_completion_result){.tag = HANDLE_COMPLETION_INVALID_COUNT};
        queue_message_result queued = QueueMessage(host, client, completion.value == 0 ? CONNECTION_EOF : CONNECTION_RECEIVE,
                            (const_byte_slice){client->buffer.ptr, completion.value});
        return (handle_completion_result){.tag = queued.tag == QUEUE_MESSAGE_OK ? HANDLE_COMPLETION_OK : HANDLE_COMPLETION_JOB_FAILED};
    }
    if (completion.value > client->pending.len ||
        ConstByteSliceSubslice(client->pending, completion.value,
            client->pending.len - completion.value, &client->pending).tag != CONST_BYTE_SLICE_SUBSLICE_OK)
        return (handle_completion_result){.tag = HANDLE_COMPLETION_INVALID_COUNT};
    if (client->pending.len != 0) {
        tcp_send_result queued = TcpSend(host->queue, client->socket, client->pending, completion.id);
        return (handle_completion_result){.tag = queued.tag == TCP_SEND_OK ? HANDLE_COMPLETION_OK : HANDLE_COMPLETION_IO_FAILED};
    }
    queue_message_result queued = QueueMessage(host, client, CONNECTION_SEND_COMPLETE, (const_byte_slice){0});
    return (handle_completion_result){.tag = queued.tag == QUEUE_MESSAGE_OK ? HANDLE_COMPLETION_OK : HANDLE_COMPLETION_JOB_FAILED};
}
typedef enum worker_option_tag { WORKER_OPTION_OK, WORKER_OPTION_INVALID_ARGUMENT } worker_option_tag;
typedef struct worker_option_result { worker_option_tag tag; } worker_option_result;
static bool OptionEqual(string_slice left, string_slice right)
{
    if (left.len != right.len) return false;
    SliceFor(left, index) { if (left.ptr[index] != right.ptr[index]) return false; }
    return true;
}
static worker_option_result WorkerOption(int argc, char **argv, usize *count, i32 *automatic,
    bool *via_main, usize *business_count)
{
    *automatic = 1;
    *via_main = false;
    *business_count = 0;
    bool seen_mode = false, seen_business = false;
    for (int argument = 1; argument < argc; argument += 2) {
        if (argument + 1 >= argc) goto invalid;
        string_slice option = {argv[argument], strlen(argv[argument])};
        string_slice value = {argv[argument + 1], strlen(argv[argument + 1])};
        if (OptionEqual(option, StringSlice("--business-dispatch"))) {
            if (seen_mode) goto invalid;
            seen_mode = true;
            if (OptionEqual(value, StringSlice("main"))) *via_main = true;
            else if (!OptionEqual(value, StringSlice("local"))) goto invalid;
            continue;
        }
        usize number = 0;
        if (!value.len) goto invalid;
        SliceFor(value, index) {
            u8 digit = (u8)value.ptr[index];
            if (digit < '0' || digit > '9' || number > CONNECTION_LIMIT / 10) goto invalid;
            number = number * 10 + digit - '0';
        }
        if (number > CONNECTION_LIMIT) goto invalid;
        if (OptionEqual(option, StringSlice("--workers"))) {
            if (!*automatic) goto invalid;
            *automatic = 0;
            *count = number;
        } else if (OptionEqual(option, StringSlice("--business-workers"))) {
            if (seen_business || number == 0) goto invalid;
            seen_business = true;
            *business_count = number;
        } else goto invalid;
    }
    if (seen_business && seen_mode && !*via_main) goto invalid;
    if (seen_business) *via_main = true;
    return (worker_option_result){.tag = WORKER_OPTION_OK};
invalid:
    return (worker_option_result){.tag = WORKER_OPTION_INVALID_ARGUMENT};
}

int main(int argc, char **argv)
{
    usize worker_count = 0;
    usize business_count;
    bool via_main;
    i32 automatic;
    if (WorkerOption(argc, argv, &worker_count, &automatic, &via_main, &business_count).tag != WORKER_OPTION_OK) {
        TerminalWrite(StringSlice("Usage: news-platform [--workers N] [--business-dispatch local|main] [--business-workers N]\n"));
        return 1;
    }
    os_cpu_info cpus = {0};
    os_cpu_id_slice placement = {0};
    if (automatic || worker_count != 0 || business_count != 0) {
        if (OsCpuInfo(&cpus).tag != OS_CPU_INFO_OK) return ReportError(StringSlice("CPU detection failed"));
        placement = cpus.performance_cpus;
        if (placement.len == 0) {
            if (cpus.efficiency_count != 0) {
                OsCpuInfoDestroy(&cpus);
                return ReportError(StringSlice("No P-cores are available in the current CPU affinity; use --workers 0 for inline mode"));
            }
            placement = cpus.core_cpus;
        }
        if (automatic) worker_count = placement.len;
    }
    /* Process-lifetime state remains live until process exit on fatal I/O errors. */
    static server host;
    host.dispatch_via_main = via_main;
    tcp_listen_result listener = TcpListen(0x7f000001, 8080, CONNECTION_LIMIT);
    if (listener.tag != TCP_LISTEN_OK) {
        if (listener.tag == TCP_LISTEN_SYSTEM_ERROR) {
            return ReportNativeError(StringSlice("TCP listen failed"), listener.native_error);
        }
        return ReportError(StringSlice("TCP listen failed"));
    }
    host.listener = (i32)listener.value;
    if (ConnectionRuntimeCreate((const_byte_slice){connection_guest_bytes, connection_guest_bytes_len}, &host.runtime).tag != CONNECTION_RUNTIME_CREATE_OK)
        return ReportError(StringSlice("Wasm compilation failed"));
    if (BusinessRuntimeCreate((const_byte_slice){business_guest_bytes, business_guest_bytes_len}, &host.business).tag != BUSINESS_RUNTIME_CREATE_OK)
        return ReportError(StringSlice("Business Wasm compilation failed"));
    if (BrokerRuntimeCreate((const_byte_slice){broker_guest_bytes, broker_guest_bytes_len}, &host.broker).tag != BROKER_RUNTIME_CREATE_OK)
        return ReportError(StringSlice("Broker Wasm compilation failed"));
    if (IndexArenaCreate(&host.connections, CONNECTION_LIMIT, sizeof(connection), _Alignof(connection)).tag != INDEX_ARENA_CREATE_OK)
        return ReportError(StringSlice("Connection arena failed"));
    if (IoCreate(CONNECTION_LIMIT, &host.queue).tag != IO_CREATE_OK)
        return ReportError(StringSlice("io_uring initialization failed"));
    if (worker_count != 0 && WorkerPoolCreate(worker_count, CONNECTION_LIMIT, placement, &host.pool).tag != WORKER_POOL_CREATE_OK)
        return ReportError(StringSlice("Worker initialization failed"));
    /* Rotate the business pool's placement after the connection workers. With
     * X+Y <= P-core count the two pools use disjoint physical cores. */
    if (business_count != 0) {
        u32 cpu_storage[CONNECTION_LIMIT];
        os_cpu_id_slice business_placement = {cpu_storage, business_count};
        SliceFor(business_placement, index) { business_placement.ptr[index] = placement.ptr[(worker_count + index) % placement.len]; }
        if (WorkerPoolCreate(business_count, CONNECTION_LIMIT, business_placement, &host.business_pool).tag != WORKER_POOL_CREATE_OK)
            return ReportError(StringSlice("Business worker initialization failed"));
    }
    char description[256];
    i32 length = snprintf(description, sizeof(description),
        "HTTP server listening on http://127.0.0.1:8080; workers=%zu%s; Wasm per connection\n",
        worker_count, worker_count == 0 ? " (inline)" : "");
    if (length > 0 && (usize)length < sizeof(description)) TerminalWrite((string_slice){description, (usize)length});
    length = snprintf(description, sizeof(description), "Business dispatch: %s; business workers=%zu%s; %s\n",
        via_main ? "main" : "local", business_count, business_count ? " (separate pool)" : " (shared)",
        BusinessRuntimeCanReset(host.business) ? "thread-local reset instances" : "fresh instances (reset unsupported)");
    if (length > 0 && (usize)length < sizeof(description)) TerminalWrite((string_slice){description, (usize)length});
    if (worker_count != 0 || business_count != 0) {
        length = snprintf(description, sizeof(description),
            "Available CPUs: logical=%u cores=%u P=%u E=%u unknown=%u unresolved=%u\n",
            cpus.logical_count, cpus.core_count, cpus.performance_count,
            cpus.efficiency_count, cpus.unknown_count, cpus.unresolved_count);
        if (length > 0 && (usize)length < sizeof(description)) TerminalWrite((string_slice){description, (usize)length});
        TerminalWrite(cpus.performance_count != 0 ? StringSlice("Worker affinity: P-core logical CPUs ") :
            StringSlice("Worker affinity: unclassified physical-core fallback, logical CPUs "));
        SliceFor(placement, index) {
            length = snprintf(description, sizeof(description), "%s%u", index == 0 ? "" : ",", placement.ptr[index]);
            if (length > 0 && (usize)length < sizeof(description)) TerminalWrite((string_slice){description, (usize)length});
        }
        TerminalWrite(StringSlice("; round-robin, one CPU per worker\n"));
        if (business_count) {
            TerminalWrite(StringSlice("Business affinity: logical CPUs "));
            for (usize index = 0; index < business_count; ++index) {
                length = snprintf(description, sizeof(description), "%s%u", index == 0 ? "" : ",",
                    placement.ptr[(worker_count + index) % placement.len]);
                if (length > 0 && (usize)length < sizeof(description)) TerminalWrite((string_slice){description, (usize)length});
            }
            TerminalWrite(StringSlice("\n"));
        }
    }
    OsCpuInfoDestroy(&cpus);
    TerminalFlush();
    for (;;) {
        if (QueueAccept(&host).tag != QUEUE_ACCEPT_OK) return ReportError(StringSlice("Queue accept failed"));
        io_completion completion;
        io_poll_result polled = IoPoll(host.queue, &completion);
        if (polled.tag != IO_POLL_OK && polled.tag != IO_POLL_EMPTY) return ReportError(StringSlice("I/O poll failed"));
        if (polled.tag == IO_POLL_OK && HandleCompletion(&host, completion).tag != HANDLE_COMPLETION_OK)
            return ReportError(StringSlice("Connection I/O failed"));
        usize inline_batch = host.inline_count;
        for (usize index = 0; index < inline_batch; ++index) {
            worker_job job = host.inline_jobs[host.inline_head];
            host.inline_head = (host.inline_head + 1) % CONNECTION_LIMIT;
            --host.inline_count;
            if (HandleRequest(&host, BusinessWorkerJob(job)).tag != HANDLE_REQUEST_OK)
                return ReportError(StringSlice("Inline business request failed"));
        }
        worker_pool *pool_storage[] = {host.pool, host.business_pool};
        worker_pool_slice pools = {pool_storage, 2};
        SliceFor(pools, pool_index) {
            worker_pool *pool = pools.ptr[pool_index];
            for (usize worker = 0; worker < WorkerPoolCount(pool); ++worker) {
                worker_result response;
                /* Bound each visit even when SEND_COMPLETE yields another buffered request. */
                for (usize batch = 0; batch < CONNECTION_LIMIT; ++batch) {
                    worker_receive_result received = WorkerReceive(pool, worker, &response);
                    if (received.tag == WORKER_RECEIVE_EMPTY) break;
                    if (received.tag != WORKER_RECEIVE_OK) return ReportError(StringSlice("Worker receive failed"));
                    connection *client = ConnectionGet(&host, IndexedIdUnpack(response.id));
                    if (client == NULL) continue;
                    if (client->job_type == BUSINESS_REQUEST) {
                        if (worker != client->business_worker || pool != (host.business_pool ? host.business_pool : host.pool))
                            return ReportError(StringSlice("Business worker mismatch"));
                    } else if (pool != host.pool || client->worker_index != worker) return ReportError(StringSlice("Connection owner mismatch"));
                    if (HandleRequest(&host, response).tag != HANDLE_REQUEST_OK) return ReportError(StringSlice("Connection request failed"));
                }
            }
        }
    }
}
