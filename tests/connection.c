#include "connection_job.h"
#include "connection_guest_bytes.h"
#include "business_guest_bytes.h"
#include "broker_guest_bytes.h"
#include "wasm_pool.h"
#include "index_arena.h"
#include "http.h"
#include "slice.h"
#include "app-routes.h"
#include <wasmtime.h>
#include <assert.h>
static business_runtime *business;
static business_runtime *broker;

static const_byte_slice Bytes(string_slice text) { return (const_byte_slice){(const u8 *)text.ptr, text.len}; }
static void Equal(const_byte_slice left, const_byte_slice right)
{
    assert(left.len == right.len);
    SliceFor(left, index) { assert(left.ptr[index] == right.ptr[index]); }
}
static void Target(const_byte_slice message, string_slice target)
{
    app_route_payload payload;
    route_request_read_result routed = RouteRequestRead(message, &payload);
    if (routed.tag == ROUTE_REQUEST_READ_OK) message = routed.http;
    assert(ConnectionMessageValidate(message).tag == CONNECTION_MESSAGE_VALIDATE_OK);
    http_request root;
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&root, sizeof(root)}, 0, message, 0, sizeof(root)).tag == BYTE_SLICE_COPY_RANGE_OK);
    const_byte_slice field;
    assert(ConstByteSliceSubslice(message, root.TargetOffset, root.TargetLength, &field).tag == CONST_BYTE_SLICE_SUBSLICE_OK);
    Equal(field, Bytes(target));
}

static void TestIndex(void)
{
    index_arena arena;
    assert(IndexArenaCreate(&arena, 0, 256, 64).tag == INDEX_ARENA_CREATE_INVALID_ARGUMENT);
    assert(IndexArenaCreate(&arena, 2, SIZE_MAX, 64).tag == INDEX_ARENA_CREATE_OVERFLOW);
    assert(IndexArenaCreate(&arena, 2, 256, 64).tag == INDEX_ARENA_CREATE_OK);
    indexed_id first, second, replacement;
    byte_slice a, b;
    assert(IndexArenaAcquire(&arena, &first, &a).tag == INDEX_ARENA_ACQUIRE_OK);
    assert((uintptr_t)a.ptr % 64 == 0);
    a.ptr[0] = 99;
    assert(IndexArenaAcquire(&arena, &second, &b).tag == INDEX_ARENA_ACQUIRE_OK);
    assert(IndexArenaAcquire(&arena, &replacement, &b).tag == INDEX_ARENA_ACQUIRE_NO_SPACE);
    assert(IndexArenaDestroy(&arena).tag == INDEX_ARENA_DESTROY_BUSY);
    assert(IndexArenaRelease(&arena, first).tag == INDEX_ARENA_RELEASE_OK);
    assert(IndexArenaGet(&arena, first, &b).tag == INDEX_ARENA_GET_STALE_ID);
    assert(IndexArenaAcquire(&arena, &replacement, &b).tag == INDEX_ARENA_ACQUIRE_OK);
    assert(first.index == replacement.index && first.generation != replacement.generation);
    assert(a.ptr == b.ptr && b.ptr[0] == 0);
    assert(IndexArenaRelease(&arena, first).tag == INDEX_ARENA_RELEASE_STALE_ID);
    assert(IndexArenaRelease(&arena, replacement).tag == INDEX_ARENA_RELEASE_OK);
    assert(IndexArenaRelease(&arena, second).tag == INDEX_ARENA_RELEASE_OK);
    assert(IndexArenaDestroy(&arena).tag == INDEX_ARENA_DESTROY_OK);
}

static connection_delivery Step(connection_vm *vm, u32 type, const_byte_slice bytes)
{
    connection_delivery result;
    assert(ConnectionVmStep(vm, type, bytes, &result).tag == CONNECTION_VM_STEP_OK);
    assert(result.type == CONNECTION_REQUEST && result.consumed == bytes.len);
    if (result.flags == CONNECTION_HAS_REQUEST) {
        connection_delivery parsed = result;
        u8 storage[BUSINESS_RESPONSE_LIMIT];
        workflow_ticket ticket = {.connection = 1, .request = 1};
        workflow_open_result opened = WorkflowOpen(business, broker, parsed.request, ticket);
        assert(opened.tag == WORKFLOW_OPEN_OK);
        workflow_step_result run;
        do {
            run = WorkflowStep(opened.instance, ticket, (byte_slice){storage, sizeof(storage)});
            if (run.tag == WORKFLOW_STEP_PENDING) ticket = run.next;
        } while (run.tag == WORKFLOW_STEP_PENDING);
        assert(run.tag == WORKFLOW_STEP_COMPLETE);
        assert(WorkflowClose(opened.instance).tag == WORKFLOW_CLOSE_OK);
        assert(ConnectionVmStep(vm, CONNECTION_RESPONSE, run.response, &result).tag == CONNECTION_VM_STEP_OK);
        result.request = parsed.request;
        result.consumed = parsed.consumed;
        result.flags |= CONNECTION_HAS_REQUEST;
    }
    return result;
}

static void TestMemoryAndPipeline(connection_runtime *runtime)
{
    connection_vm *vm;
    connection_delivery result;
    assert(ConnectionVmOpen(runtime, &vm, &result).tag == CONNECTION_VM_OPEN_OK && result.flags == CONNECTION_WANT_INPUT);
    result = Step(vm, CONNECTION_RECEIVE, Bytes(StringSlice("GET /profile/1 HTTP/1.1\r\nHost: test\r\n\r\nGET /profile/2 HTTP/1.1\r\nHost: test\r\n\r\n")));
    assert(result.flags == (CONNECTION_WANT_SEND | CONNECTION_HAS_REQUEST));
    Target(result.request, StringSlice("/profile/1"));
    app_route_payload captures;
    assert(RouteRequestRead(result.request, &captures).tag == ROUTE_REQUEST_READ_OK);
    assert(captures.tag == APP_ROUTE_profile);
    const_byte_range_resolve_result id = ConstByteRangeResolve(result.request, captures.captures.route_profile.capture_id);
    assert(id.tag == CONST_BYTE_RANGE_RESOLVE_OK);
    Equal(id.bytes, Bytes(StringSlice("1")));
    u8 saved_storage[4096];
    byte_slice saved = {saved_storage, result.request.len};
    assert(ByteSliceCopy(saved, 0, result.request).tag == BYTE_SLICE_COPY_OK);
    /* No new network input: second request is already buffered in the guest,
     * outside the arena that will reset when SEND_COMPLETE starts it. */
    result = Step(vm, CONNECTION_SEND_COMPLETE, (const_byte_slice){0});
    assert(result.flags == (CONNECTION_WANT_SEND | CONNECTION_HAS_REQUEST));
    Target(result.request, StringSlice("/profile/2"));
    assert(RouteRequestRead(result.request, &captures).tag == ROUTE_REQUEST_READ_OK);
    assert(captures.tag == APP_ROUTE_profile);
    id = ConstByteRangeResolve(result.request, captures.captures.route_profile.capture_id);
    assert(id.tag == CONST_BYTE_RANGE_RESOLVE_OK);
    Equal(id.bytes, Bytes(StringSlice("2")));
    Target((const_byte_slice){saved.ptr, saved.len}, StringSlice("/profile/1"));
    assert(Step(vm, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}).flags == CONNECTION_WANT_INPUT);

    u8 large_storage[65536];
    byte_slice large = {large_storage, sizeof(large_storage)};
    string_slice prefix = StringSlice("GET / HTTP/1.1\r\nHost: test\r\n");
    assert(ByteSliceCopy(large, 0, Bytes(prefix)).tag == BYTE_SLICE_COPY_OK);
    usize used = prefix.len;
    u8 value_storage[500];
    byte_slice value = {value_storage, sizeof(value_storage)};
    SliceFor(value, index) { value.ptr[index] = 'a'; }
    for (usize index = 0; index < 127; ++index) {
        assert(ByteSliceCopy(large, used, Bytes(StringSlice("X: "))).tag == BYTE_SLICE_COPY_OK); used += 3;
        assert(ByteSliceCopy(large, used, (const_byte_slice){value.ptr, value.len}).tag == BYTE_SLICE_COPY_OK); used += value.len;
        assert(ByteSliceCopy(large, used, Bytes(StringSlice("\r\n"))).tag == BYTE_SLICE_COPY_OK); used += 2;
    }
    assert(ByteSliceCopy(large, used, Bytes(StringSlice("\r\n"))).tag == BYTE_SLICE_COPY_OK); used += 2;
    const_byte_slice input = {large.ptr, used};
    while (input.len != 0) {
        usize count = input.len < CONNECTION_INPUT_SIZE ? input.len : CONNECTION_INPUT_SIZE;
        const_byte_slice part;
        assert(ConstByteSliceSubslice(input, 0, count, &part).tag == CONST_BYTE_SLICE_SUBSLICE_OK);
        result = Step(vm, CONNECTION_RECEIVE, part);
        assert(ConstByteSliceSubslice(input, count, input.len - count, &input).tag == CONST_BYTE_SLICE_SUBSLICE_OK);
        assert(result.flags == (input.len ? CONNECTION_WANT_INPUT : CONNECTION_WANT_SEND | CONNECTION_HAS_REQUEST));
    }
    assert(result.request.len > 65536);
    connection_vm_memory_size_result memory = ConnectionVmMemorySize(vm);
    assert(memory.tag == CONNECTION_VM_MEMORY_SIZE_OK);
    usize plateau = memory.value;
    assert(Step(vm, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}).flags == CONNECTION_WANT_INPUT);
    for (usize request = 0; request < 2000; ++request) {
        result = Step(vm, CONNECTION_RECEIVE, Bytes(StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\n")));
        Target(result.request, StringSlice("/"));
        assert(Step(vm, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}).flags == CONNECTION_WANT_INPUT);
        memory = ConnectionVmMemorySize(vm);
        assert(memory.tag == CONNECTION_VM_MEMORY_SIZE_OK && memory.value == plateau);
    }
    assert(Step(vm, CONNECTION_EOF, (const_byte_slice){0}).flags == CONNECTION_WANT_CLOSE);
    assert(ConnectionVmClose(vm).tag == CONNECTION_VM_CLOSE_OK);
}

typedef struct wrong_thread {
    connection_vm *vm;
    connection_vm_step_result status;
    connection_vm_close_result close;
    connection_vm_memory_size_result memory;
} wrong_thread;
static void WrongThread(void *context)
{
    wrong_thread *test = context;
    connection_delivery result;
    test->status = ConnectionVmStep(test->vm, CONNECTION_EOF, (const_byte_slice){0}, &result);
    test->close = ConnectionVmClose(test->vm);
    test->memory = ConnectionVmMemorySize(test->vm);
}

static void TestOwnershipAndEof(connection_runtime *runtime)
{
    connection_vm *vm;
    connection_delivery result;
    assert(ConnectionVmOpen(runtime, &vm, &result).tag == CONNECTION_VM_OPEN_OK);
    wrong_thread test = {.vm = vm};
    os_thread *thread;
    assert(ThreadStart(WrongThread, &test, (os_cpu_id_slice){0}, &thread).tag == THREAD_START_OK);
    assert(ThreadJoin(thread).tag == THREAD_JOIN_OK && test.status.tag == CONNECTION_VM_STEP_WRONG_THREAD);
    assert(test.close.tag == CONNECTION_VM_CLOSE_WRONG_THREAD);
    assert(test.memory.tag == CONNECTION_VM_MEMORY_SIZE_WRONG_THREAD);
    assert(ConnectionVmMemorySize(NULL).tag == CONNECTION_VM_MEMORY_SIZE_INVALID_ARGUMENT);
    assert(ConnectionVmStep(vm, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}, &result).tag == CONNECTION_VM_STEP_INVALID_ARGUMENT);
    assert(Step(vm, CONNECTION_RECEIVE, Bytes(StringSlice("GET / HTTP/1.1\r\nHost: par"))).flags == CONNECTION_WANT_INPUT);
    result = Step(vm, CONNECTION_EOF, (const_byte_slice){0});
    assert(result.flags == CONNECTION_WANT_SEND);
    assert(Step(vm, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}).flags == CONNECTION_WANT_CLOSE);
    assert(ConnectionVmClose(vm).tag == CONNECTION_VM_CLOSE_OK);
}

static worker_result Job(worker_pool *pool, usize owner, connection_job_context *context, u32 type, const_byte_slice input)
{
    worker_job job = {.id = context->id, .function = ConnectionWorkerJob, .context = context, .input = input, .type = type};
    worker_result result;
    if (pool == NULL) {
        result = ConnectionWorkerJob(job);
    } else {
        assert(WorkerSubmit(pool, owner, job).tag == WORKER_SUBMIT_OK);
        worker_receive_result ready;
        do { ready = WorkerReceive(pool, owner, &result); assert(ready.tag == WORKER_RECEIVE_OK || ready.tag == WORKER_RECEIVE_EMPTY); } while (ready.tag == WORKER_RECEIVE_EMPTY);
    }
    assert(result.id == context->id);
    connection_job_result outcome;
    assert(result.outcome.len == sizeof(outcome));
    assert(ByteSliceCopy((byte_slice){(u8 *)&outcome, sizeof(outcome)}, 0, result.outcome).tag == BYTE_SLICE_COPY_OK);
    assert(outcome.tag == CONNECTION_JOB_OK);
    return result;
}

static connection_job_result JobOutcome(worker_result response)
{
    connection_job_result outcome;
    assert(response.outcome.len == sizeof(outcome));
    assert(ByteSliceCopy((byte_slice){(u8 *)&outcome, sizeof(outcome)}, 0, response.outcome).tag == BYTE_SLICE_COPY_OK);
    return outcome;
}
static void TestJobs(connection_runtime *runtime)
{
    worker_pool *pool;
    assert(WorkerPoolCreate(2, 4, (os_cpu_id_slice){0}, &pool).tag == WORKER_POOL_CREATE_OK);
    connection_job_context first = {.runtime = runtime, .id = IndexedIdPack((indexed_id){3, 1})};
    connection_job_context second = {.runtime = runtime, .id = IndexedIdPack((indexed_id){4, 1})};
    first.business.runtime = second.business.runtime = business;
    assert(Job(pool, 0, &first, CONNECTION_OPEN, (const_byte_slice){0}).flags == CONNECTION_WANT_INPUT);
    assert(Job(pool, 1, &second, CONNECTION_OPEN, (const_byte_slice){0}).flags == CONNECTION_WANT_INPUT);
    assert(Job(pool, 0, &first, CONNECTION_RECEIVE, Bytes(StringSlice("GET / HTTP/1.1\r\nHost: par"))).flags == CONNECTION_WANT_INPUT);
    assert(Job(pool, 0, &first, CONNECTION_CLOSE, (const_byte_slice){0}).type == CONNECTION_CLOSED);
    string_slice request = StringSlice("GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    worker_result parsed = Job(pool, 1, &second, CONNECTION_RECEIVE, Bytes(request));
    assert(JobOutcome(parsed).tag == CONNECTION_JOB_OK && parsed.type == CONNECTION_REQUEST);
    Target(parsed.message, StringSlice("/"));
    connection_job_context direct = {.runtime = runtime, .id = 55};
    direct.business.runtime = business;
    assert(JobOutcome(Job(NULL, 0, &direct, CONNECTION_OPEN, (const_byte_slice){0})).tag == CONNECTION_JOB_OK);
    worker_result inline_result = Job(NULL, 0, &direct, CONNECTION_RECEIVE, Bytes(request));
    Equal(parsed.message, inline_result.message);
    Equal(parsed.data, inline_result.data);
    worker_job stale = {.id = 0, .type = CONNECTION_EOF, .context = &direct};
    assert(ConnectionJob(stale).tag == CONNECTION_JOB_STALE_ID);
    assert(Job(pool, 1, &second, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}).flags == CONNECTION_WANT_CLOSE);
    assert(Job(pool, 1, &second, CONNECTION_CLOSE, (const_byte_slice){0}).type == CONNECTION_CLOSED);
    assert(Job(NULL, 0, &direct, CONNECTION_CLOSE, (const_byte_slice){0}).type == CONNECTION_CLOSED);
    /* Parse on worker 0, run a fresh business instance on worker 1, resume the
     * original connection on worker 0. The copied input remains live meanwhile. */
    connection_job_context dispatched = {.runtime = runtime, .id = 77, .dispatch_via_main = true,
        .business = {.runtime = business}};
    assert(Job(pool, 0, &dispatched, CONNECTION_OPEN, (const_byte_slice){0}).flags == CONNECTION_WANT_INPUT);
    worker_result awaiting = Job(pool, 0, &dispatched, CONNECTION_RECEIVE, Bytes(request));
    assert(awaiting.flags == CONNECTION_HAS_REQUEST);
    worker_job business_job = {.id = dispatched.id, .type = BUSINESS_REQUEST, .function = BusinessWorkerJob,
        .context = &dispatched.business, .input = awaiting.message};
    assert(WorkerSubmit(pool, 1, business_job).tag == WORKER_SUBMIT_OK);
    worker_select_result selected = WorkerSelect(pool, 1);
    assert(selected.tag == WORKER_SELECT_OK && selected.index == 0);
    worker_result completed;
    worker_receive_result received;
    do { received = WorkerReceive(pool, 1, &completed); } while (received.tag == WORKER_RECEIVE_EMPTY);
    assert(received.tag == WORKER_RECEIVE_OK && completed.type == BUSINESS_RESULT);
    business_run_result response;
    assert(ByteSliceCopy((byte_slice){(u8 *)&response, sizeof(response)}, 0, completed.outcome).tag == BYTE_SLICE_COPY_OK);
    assert(response.tag == BUSINESS_RUN_OK);
    assert(Job(pool, 0, &dispatched, CONNECTION_RESPONSE, response.response).flags == CONNECTION_WANT_SEND);
    assert(Job(pool, 0, &dispatched, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}).flags == CONNECTION_WANT_CLOSE);
    assert(Job(pool, 0, &dispatched, CONNECTION_CLOSE, (const_byte_slice){0}).type == CONNECTION_CLOSED);
    /* Failure is a connection-generated HTTP 500 followed by close. */
    dispatched.id = 78;
    assert(Job(pool, 0, &dispatched, CONNECTION_OPEN, (const_byte_slice){0}).flags == CONNECTION_WANT_INPUT);
    assert(Job(pool, 0, &dispatched, CONNECTION_RECEIVE, Bytes(request)).flags == CONNECTION_HAS_REQUEST);
    worker_result failed = Job(pool, 0, &dispatched, CONNECTION_BUSINESS_FAILED, (const_byte_slice){0});
    assert(failed.flags == CONNECTION_WANT_SEND && failed.data.len > 12 && failed.data.ptr[9] == '5');
    assert(Job(pool, 0, &dispatched, CONNECTION_SEND_COMPLETE, (const_byte_slice){0}).flags == CONNECTION_WANT_CLOSE);
    assert(Job(pool, 0, &dispatched, CONNECTION_CLOSE, (const_byte_slice){0}).type == CONNECTION_CLOSED);
    assert(WorkerPoolDestroy(pool).tag == WORKER_POOL_DESTROY_OK);
}

static void BadModule(string_slice wat, i32 creation_fails)
{
    wasm_byte_vec_t bytes;
    wasmtime_error_t *error = wasmtime_wat2wasm(wat.ptr, wat.len, &bytes);
    assert(error == NULL);
    connection_runtime *runtime;
    connection_runtime_create_result result = ConnectionRuntimeCreate((const_byte_slice){(const u8 *)bytes.data, bytes.size}, &runtime);
    wasm_byte_vec_delete(&bytes);
    if (creation_fails) { assert(result.tag == CONNECTION_RUNTIME_CREATE_INVALID_MODULE); return; }
    assert(result.tag == CONNECTION_RUNTIME_CREATE_OK);
    connection_vm *vm;
    connection_delivery delivery;
    assert(ConnectionVmOpen(runtime, &vm, &delivery).tag == CONNECTION_VM_OPEN_INVALID_GUEST && vm == NULL);
    ConnectionRuntimeDestroy(runtime);
}

static void TestInvalidActions(void)
{
    /* Valid initialization followed by an action array crossing memory's end. */
    string_slice wat = StringSlice(
        "(module (memory (export \"memory\") 1)"
        " (func (export \"ConnectionInit\") (param i32) (result i32)"
        " i32.const 64 i32.const 4 i32.store"
        " i32.const 68 i32.const 52 i32.store"
        " i32.const 72 i32.const 1024 i32.store"
        " i32.const 76 i32.const 4096 i32.store"
        " i32.const 88 i32.const 0x1006 i32.store"
        " i32.const 100 i32.const 128 i32.store"
        " i32.const 104 i32.const 1 i32.store"
        " i32.const 108 i32.const 8192 i32.store"
        " i32.const 112 i32.const 3072 i32.store"
        " i32.const 128 i32.const 0x1010 i32.store"
        " i32.const 52 i32.const 64 i32.store i32.const 48)"
        " (func (export \"ConnectionStep\")"
        " i32.const 100 i32.const 65535 i32.store))");
    wasm_byte_vec_t bytes;
    assert(wasmtime_wat2wasm(wat.ptr, wat.len, &bytes) == NULL);
    connection_runtime *runtime;
    assert(ConnectionRuntimeCreate((const_byte_slice){(const u8 *)bytes.data, bytes.size}, &runtime).tag == CONNECTION_RUNTIME_CREATE_OK);
    wasm_byte_vec_delete(&bytes);
    connection_vm *vm;
    connection_delivery delivery;
    assert(ConnectionVmOpen(runtime, &vm, &delivery).tag == CONNECTION_VM_OPEN_OK);
    assert(ConnectionVmStep(vm, CONNECTION_EOF, (const_byte_slice){0}, &delivery).tag == CONNECTION_VM_STEP_INVALID_GUEST);
    assert(ConnectionVmClose(vm).tag == CONNECTION_VM_CLOSE_OK);
    ConnectionRuntimeDestroy(runtime);
}

static void TestInitFailure(void)
{
    /* Guest ABI fixture: tag 3 is CONNECTION_INIT_NO_SPACE. It must translate
     * into Open's own OUT_OF_MEMORY tag, not a pointer or generic guest error. */
    _Static_assert(CONNECTION_INIT_NO_SPACE == 3, "update WAT fixture with ABI");
    string_slice wat = StringSlice(
        "(module (memory (export \"memory\") 1)"
        " (data (i32.const 64) \"\\03\\00\\00\\00\")"
        " (func (export \"ConnectionStep\"))"
        " (func (export \"ConnectionInit\") (param i32) (result i32) i32.const 64))");
    wasm_byte_vec_t bytes;
    assert(wasmtime_wat2wasm(wat.ptr, wat.len, &bytes) == NULL);
    connection_runtime *runtime;
    assert(ConnectionRuntimeCreate((const_byte_slice){(const u8 *)bytes.data, bytes.size}, &runtime).tag == CONNECTION_RUNTIME_CREATE_OK);
    wasm_byte_vec_delete(&bytes);
    connection_vm *vm;
    connection_delivery delivery;
    assert(ConnectionVmOpen(runtime, &vm, &delivery).tag == CONNECTION_VM_OPEN_OUT_OF_MEMORY);
    assert(vm == NULL);
    ConnectionRuntimeDestroy(runtime);
}

static void TestPoolCapacity(connection_runtime *runtime)
{
    connection_vm *storage[WASM_POOL_CAPACITY];
    struct connection_vm_slice { connection_vm **ptr; usize len; } instances = {storage, WASM_POOL_CAPACITY};
    connection_delivery delivery;
    SliceFor(instances, index) {
        assert(ConnectionVmOpen(runtime, &instances.ptr[index], &delivery).tag == CONNECTION_VM_OPEN_OK);
        assert(delivery.flags == CONNECTION_WANT_INPUT);
    }
    connection_vm *extra;
    assert(ConnectionVmOpen(runtime, &extra, &delivery).tag == CONNECTION_VM_OPEN_INVALID_GUEST && extra == NULL);
    usize recycled = instances.len / 2;
    assert(ConnectionVmClose(instances.ptr[recycled]).tag == CONNECTION_VM_CLOSE_OK);
    assert(ConnectionVmOpen(runtime, &instances.ptr[recycled], &delivery).tag == CONNECTION_VM_OPEN_OK);
    assert(delivery.flags == CONNECTION_WANT_INPUT);
    SliceFor(instances, index) { assert(ConnectionVmClose(instances.ptr[index]).tag == CONNECTION_VM_CLOSE_OK); }
}

int main(void)
{
    TestIndex();
    assert(BusinessRuntimeCreate((const_byte_slice){business_guest_bytes, business_guest_bytes_len}, &business).tag == BUSINESS_RUNTIME_CREATE_OK);
    assert(BrokerRuntimeCreate((const_byte_slice){broker_guest_bytes, broker_guest_bytes_len}, &broker).tag == BROKER_RUNTIME_CREATE_OK);
    connection_runtime *runtime;
    assert(ConnectionRuntimeCreate((const_byte_slice){connection_guest_bytes, connection_guest_bytes_len}, &runtime).tag == CONNECTION_RUNTIME_CREATE_OK);
    TestMemoryAndPipeline(runtime);
    TestOwnershipAndEof(runtime);
    TestJobs(runtime);
    TestPoolCapacity(runtime);
    BusinessRuntimeDestroy(business);
    BusinessRuntimeDestroy(broker);
    ConnectionRuntimeDestroy(runtime);
    TestInvalidActions();
    TestInitFailure();
    /* Pool limits reject oversized modules at compilation, before instantiation. */
    BadModule(StringSlice("(module (memory (export \"memory\") 33) (func (export \"ConnectionStep\")) (func (export \"ConnectionInit\") (param i32) (result i32) i32.const 64))"), 1);
    BadModule(StringSlice("(module (import \"wasi_snapshot_preview1\" \"fd_write\" (func)))"), 1);
    BadModule(StringSlice("(module (memory (export \"memory\") 1) (func (export \"ConnectionStep\")) (func (export \"ConnectionInit\") (param i32) (result i32) i32.const 65535))"), 0);
    BadModule(StringSlice("(module (memory (export \"memory\") 1) (func (export \"ConnectionStep\")) (func (export \"ConnectionInit\") (param i32) (result i32) (loop br 0) i32.const 0))"), 0);
    TerminalWrite(StringSlice("PASS: connection Wasm lifecycle, worker pinning, stale IDs, pipeline copies, arena reset/growth plateau, EOF, cancellation, ABI bounds/imports/fuel\n"));
    return 0;
}
