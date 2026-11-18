#define _GNU_SOURCE
#include "os.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <liburing.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

terminal_write_result TerminalWrite(string_slice text)
{
    if (text.len == 0) {
        return (terminal_write_result){.tag = TERMINAL_WRITE_OK};
    }

    usize written = fwrite(text.ptr, 1, text.len, stdout);
    if (written != text.len)
        return (terminal_write_result){.tag = TERMINAL_WRITE_SYSTEM_ERROR, .written = written, .native_error = errno};
    return (terminal_write_result){.tag = TERMINAL_WRITE_OK, .written = written};
}

tcp_connect_result TcpConnect(u32 address, u16 port)
{
    i32 descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) return (tcp_connect_result){.tag = TCP_CONNECT_SYSTEM_ERROR, .native_error = errno};
    struct sockaddr_in endpoint = {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {.s_addr = htonl(address)},
    };
    if (connect(descriptor, (struct sockaddr *)&endpoint, sizeof(endpoint)) < 0) {
        i32 error = errno;
        close(descriptor);
        return (tcp_connect_result){.tag = error == ECONNREFUSED ? TCP_CONNECT_REFUSED : TCP_CONNECT_SYSTEM_ERROR,
            .native_error = error};
    }
    return (tcp_connect_result){.tag = TCP_CONNECT_OK, .socket = descriptor};
}

terminal_flush_result TerminalFlush(void)
{
    if (fflush(stdout) != 0) return (terminal_flush_result){.tag = TERMINAL_FLUSH_SYSTEM_ERROR, .native_error = errno};
    return (terminal_flush_result){.tag = TERMINAL_FLUSH_OK};
}

string_slice OsErrorText(i32 error)
{
    /* strerror and strlen are the C-string API boundary. */
    const char *text = strerror(error);
    return (string_slice){text, strlen(text)};
}

os_monotonic_nanoseconds_result OsMonotonicNanoseconds(u64 *nanoseconds)
{
    if (nanoseconds == NULL) return (os_monotonic_nanoseconds_result){.tag = OS_MONOTONIC_NANOSECONDS_INVALID_ARGUMENT};
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return (os_monotonic_nanoseconds_result){.tag = OS_MONOTONIC_NANOSECONDS_SYSTEM_ERROR, .native_error = errno};
    *nanoseconds = (u64)now.tv_sec * UINT64_C(1000000000) + (u64)now.tv_nsec;
    return (os_monotonic_nanoseconds_result){.tag = OS_MONOTONIC_NANOSECONDS_OK};
}

struct os_thread {
    pthread_t native;
    thread_function function;
    void *context;
};

static void *ThreadEntry(void *argument)
{
    os_thread *thread = argument;
    thread->function(thread->context);
    return NULL;
}

typedef enum thread_affinity_tag {
    THREAD_AFFINITY_OK, THREAD_AFFINITY_INVALID_ARGUMENT,
    THREAD_AFFINITY_OUT_OF_MEMORY, THREAD_AFFINITY_SYSTEM_ERROR,
} thread_affinity_tag;
typedef struct thread_affinity_result { thread_affinity_tag tag; i32 native_error; } thread_affinity_result;

static thread_affinity_result ThreadAffinity(pthread_attr_t *attributes, os_cpu_id_slice affinity)
{
    usize capacity = 128, size;
    cpu_set_t *allowed;
    for (;;) {
        size = CPU_ALLOC_SIZE(capacity);
        allowed = CPU_ALLOC(capacity);
        if (allowed == NULL) return (thread_affinity_result){.tag = THREAD_AFFINITY_OUT_OF_MEMORY};
        if (sched_getaffinity(0, size, allowed) == 0) break;
        i32 error = errno;
        CPU_FREE(allowed);
        if (error != EINVAL || capacity > INT_MAX / 2)
            return (thread_affinity_result){.tag = THREAD_AFFINITY_SYSTEM_ERROR, .native_error = error};
        capacity *= 2;
    }
    cpu_set_t *selected = CPU_ALLOC(capacity);
    if (selected == NULL) {
        CPU_FREE(allowed);
        return (thread_affinity_result){.tag = THREAD_AFFINITY_OUT_OF_MEMORY};
    }
    CPU_ZERO_S(size, selected);
    SliceFor(affinity, index) {
        u32 cpu = affinity.ptr[index];
        if (cpu >= capacity || !CPU_ISSET_S(cpu, size, allowed)) {
            CPU_FREE(allowed);
            CPU_FREE(selected);
            return (thread_affinity_result){.tag = THREAD_AFFINITY_INVALID_ARGUMENT};
        }
        CPU_SET_S(cpu, size, selected);
    }
    /* pthread attributes copy this mask; the new thread starts with it. */
    i32 error = pthread_attr_setaffinity_np(attributes, size, selected);
    CPU_FREE(allowed);
    CPU_FREE(selected);
    if (error != 0) return (thread_affinity_result){.tag = THREAD_AFFINITY_SYSTEM_ERROR, .native_error = error};
    return (thread_affinity_result){.tag = THREAD_AFFINITY_OK};
}

thread_start_result ThreadStart(thread_function function, void *context, os_cpu_id_slice affinity, os_thread **thread)
{
    if (thread == NULL) {
        return (thread_start_result){.tag = THREAD_START_INVALID_ARGUMENT};
    }
    *thread = NULL;
    if (function == NULL || (affinity.ptr == NULL && affinity.len != 0)) {
        return (thread_start_result){.tag = THREAD_START_INVALID_ARGUMENT};
    }
    os_thread *created = malloc(sizeof(*created));
    if (created == NULL) {
        return (thread_start_result){.tag = THREAD_START_OUT_OF_MEMORY};
    }
    created->function = function;
    created->context = context;
    pthread_attr_t attributes;
    i32 error = pthread_attr_init(&attributes);
    if (error != 0) {
        free(created);
        return (thread_start_result){.tag = THREAD_START_SYSTEM_ERROR, .native_error = error};
    }
    if (affinity.len != 0) {
        thread_affinity_result pinned = ThreadAffinity(&attributes, affinity);
        if (pinned.tag != THREAD_AFFINITY_OK) {
            thread_start_result failure = {.tag = THREAD_START_SYSTEM_ERROR};
            switch (pinned.tag) {
            case THREAD_AFFINITY_INVALID_ARGUMENT: failure.tag = THREAD_START_INVALID_ARGUMENT; break;
            case THREAD_AFFINITY_OUT_OF_MEMORY: failure.tag = THREAD_START_OUT_OF_MEMORY; break;
            case THREAD_AFFINITY_SYSTEM_ERROR: failure.native_error = pinned.native_error; break;
            case THREAD_AFFINITY_OK: break;
            }
            pthread_attr_destroy(&attributes);
            free(created);
            return failure;
        }
    }
    error = pthread_create(&created->native, &attributes, ThreadEntry, created);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        free(created);
        return (thread_start_result){.tag = THREAD_START_SYSTEM_ERROR, .native_error = error};
    }
    *thread = created;
    return (thread_start_result){.tag = THREAD_START_OK};
}

thread_join_result ThreadJoin(os_thread *thread)
{
    if (thread == NULL) {
        return (thread_join_result){.tag = THREAD_JOIN_INVALID_ARGUMENT};
    }
    i32 error = pthread_join(thread->native, NULL);
    if (error != 0) {
        return (thread_join_result){.tag = THREAD_JOIN_SYSTEM_ERROR, .native_error = error};
    }
    free(thread);
    return (thread_join_result){.tag = THREAD_JOIN_OK};
}

void ThreadYield(void)
{
    sched_yield();
}

u64 ThreadCurrentId(void)
{
    return (u64)gettid();
}

os_event_create_result OsEventCreate(i32 *event)
{
    if (event == NULL) return (os_event_create_result){.tag = OS_EVENT_CREATE_INVALID_ARGUMENT};
    *event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (*event < 0) return (os_event_create_result){.tag = OS_EVENT_CREATE_SYSTEM_ERROR, .native_error = errno};
    return (os_event_create_result){.tag = OS_EVENT_CREATE_OK};
}

os_event_signal_result OsEventSignal(i32 event)
{
    u64 value = 1;
    isize result;
    do { result = write(event, &value, sizeof(value)); } while (result < 0 && errno == EINTR);
    if (result == sizeof(value) || (result < 0 && errno == EAGAIN)) return (os_event_signal_result){.tag = OS_EVENT_SIGNAL_OK};
    if (result < 0) return (os_event_signal_result){.tag = OS_EVENT_SIGNAL_SYSTEM_ERROR, .native_error = errno};
    return (os_event_signal_result){.tag = OS_EVENT_SIGNAL_IO_ERROR};
}

os_event_consume_result OsEventConsume(i32 event)
{
    u64 value;
    isize result;
    do { result = read(event, &value, sizeof(value)); } while (result < 0 && errno == EINTR);
    if (result == sizeof(value) || (result < 0 && errno == EAGAIN)) return (os_event_consume_result){.tag = OS_EVENT_CONSUME_OK};
    if (result < 0) return (os_event_consume_result){.tag = OS_EVENT_CONSUME_SYSTEM_ERROR, .native_error = errno};
    return (os_event_consume_result){.tag = OS_EVENT_CONSUME_IO_ERROR};
}

os_event_wait_result OsEventWait(i32 event)
{
    if (event < 0) return (os_event_wait_result){.tag = OS_EVENT_WAIT_INVALID_ARGUMENT};
    struct pollfd descriptor = {.fd = event, .events = POLLIN};
    i32 result;
    do { result = poll(&descriptor, 1, -1); } while (result < 0 && errno == EINTR);
    if (result < 0) return (os_event_wait_result){.tag = OS_EVENT_WAIT_SYSTEM_ERROR, .native_error = errno};
    if (!(descriptor.revents & POLLIN)) return (os_event_wait_result){.tag = OS_EVENT_WAIT_IO_ERROR};
    os_event_consume_result consumed = OsEventConsume(event);
    switch (consumed.tag) {
    case OS_EVENT_CONSUME_OK: return (os_event_wait_result){.tag = OS_EVENT_WAIT_OK};
    case OS_EVENT_CONSUME_IO_ERROR: return (os_event_wait_result){.tag = OS_EVENT_WAIT_IO_ERROR};
    case OS_EVENT_CONSUME_SYSTEM_ERROR: return (os_event_wait_result){.tag = OS_EVENT_WAIT_SYSTEM_ERROR, .native_error = consumed.native_error};
    }
    __builtin_unreachable();
}

void OsEventClose(i32 event)
{
    close(event);
}

struct io_queue {
    struct io_uring ring;
    u32 capacity;
    u32 outstanding;
};

io_create_result IoCreate(u32 capacity, io_queue **queue)
{
    if (queue == NULL || capacity == 0) {
        return (io_create_result){.tag = IO_CREATE_INVALID_ARGUMENT};
    }
    *queue = NULL;
    io_queue *created = calloc(1, sizeof(*created));
    if (created == NULL) {
        return (io_create_result){.tag = IO_CREATE_OUT_OF_MEMORY};
    }
    i32 result = io_uring_queue_init(capacity, &created->ring, 0);
    if (result < 0) {
        free(created);
        return (io_create_result){.tag = IO_CREATE_SYSTEM_ERROR, .native_error = -result};
    }
    created->capacity = capacity;
    *queue = created;
    return (io_create_result){.tag = IO_CREATE_OK};
}

io_destroy_result IoDestroy(io_queue *queue)
{
    if (queue == NULL) {
        return (io_destroy_result){.tag = IO_DESTROY_OK};
    }
    if (queue->outstanding != 0) {
        return (io_destroy_result){.tag = IO_DESTROY_BUSY};
    }
    io_uring_queue_exit(&queue->ring);
    free(queue);
    return (io_destroy_result){.tag = IO_DESTROY_OK};
}

static struct io_uring_sqe *IoNext(io_queue *queue, u64 id)
{
    if (queue->outstanding >= queue->capacity) {
        return NULL;
    }
    struct io_uring_sqe *entry = io_uring_get_sqe(&queue->ring);
    if (entry != NULL) {
        io_uring_sqe_set_data64(entry, id);
        ++queue->outstanding;
    }
    return entry;
}

io_event_wait_result IoEventWait(io_queue *queue, i32 event, u64 id)
{
    if (queue == NULL || event < 0) return (io_event_wait_result){.tag = IO_EVENT_WAIT_INVALID_ARGUMENT};
    struct io_uring_sqe *entry = IoNext(queue, id);
    if (entry == NULL) return (io_event_wait_result){.tag = IO_EVENT_WAIT_NO_SPACE};
    io_uring_prep_poll_add(entry, event, POLLIN);
    return (io_event_wait_result){.tag = IO_EVENT_WAIT_OK};
}

typedef enum io_submit_tag { IO_SUBMIT_OK, IO_SUBMIT_IO_ERROR, IO_SUBMIT_SYSTEM_ERROR } io_submit_tag;
typedef struct io_submit_result { io_submit_tag tag; i32 native_error; } io_submit_result;
static io_submit_result IoSubmit(io_queue *queue)
{
    while (io_uring_sq_ready(&queue->ring) != 0) {
        i32 result = io_uring_submit(&queue->ring);
        if (result == -EINTR) continue;
        if (result < 0) return (io_submit_result){.tag = IO_SUBMIT_SYSTEM_ERROR, .native_error = -result};
        if (result == 0) return (io_submit_result){.tag = IO_SUBMIT_IO_ERROR};
    }
    return (io_submit_result){.tag = IO_SUBMIT_OK};
}

static io_completion IoCompletion(struct io_uring_cqe *entry)
{
    io_completion completion = {.id = io_uring_cqe_get_data64(entry), .tag = IO_COMPLETION_OK};
    if (entry->res >= 0) { completion.value = (u32)entry->res; return completion; }
    completion.native_error = -entry->res;
    switch (-entry->res) {
    case EINTR: completion.tag = IO_COMPLETION_INTERRUPTED; break;
    case EAGAIN: completion.tag = IO_COMPLETION_WOULD_BLOCK; break;
    case ECONNABORTED: completion.tag = IO_COMPLETION_ABORTED; break;
    case EPROTO: completion.tag = IO_COMPLETION_PROTOCOL_ERROR; break;
    case EBADF: completion.tag = IO_COMPLETION_BAD_HANDLE; break;
    default: completion.tag = IO_COMPLETION_SYSTEM_ERROR; break;
    }
    return completion;
}

io_poll_result IoPoll(io_queue *queue, io_completion *completion)
{
    if (queue == NULL || completion == NULL) return (io_poll_result){.tag = IO_POLL_INVALID_ARGUMENT};
    if (queue->outstanding == 0) return (io_poll_result){.tag = IO_POLL_EMPTY};
    io_submit_result submitted = IoSubmit(queue);
    switch (submitted.tag) {
    case IO_SUBMIT_OK: break;
    case IO_SUBMIT_IO_ERROR: return (io_poll_result){.tag = IO_POLL_IO_ERROR};
    case IO_SUBMIT_SYSTEM_ERROR: return (io_poll_result){.tag = IO_POLL_SYSTEM_ERROR, .native_error = submitted.native_error};
    }
    struct io_uring_cqe *entry;
    i32 result = io_uring_peek_cqe(&queue->ring, &entry);
    if (result == -EAGAIN || result == -EINTR) return (io_poll_result){.tag = IO_POLL_EMPTY};
    if (result < 0) return (io_poll_result){.tag = IO_POLL_SYSTEM_ERROR, .native_error = -result};
    *completion = IoCompletion(entry);
    io_uring_cqe_seen(&queue->ring, entry);
    --queue->outstanding;
    return (io_poll_result){.tag = IO_POLL_OK};
}

io_wait_result IoWait(io_queue *queue, io_completion *completion)
{
    if (queue == NULL || completion == NULL || queue->outstanding == 0)
        return (io_wait_result){.tag = IO_WAIT_INVALID_ARGUMENT};
    io_submit_result submitted = IoSubmit(queue);
    switch (submitted.tag) {
    case IO_SUBMIT_OK: break;
    case IO_SUBMIT_IO_ERROR: return (io_wait_result){.tag = IO_WAIT_IO_ERROR};
    case IO_SUBMIT_SYSTEM_ERROR: return (io_wait_result){.tag = IO_WAIT_SYSTEM_ERROR, .native_error = submitted.native_error};
    }
    struct io_uring_cqe *entry;
    i32 result;
    do { result = io_uring_wait_cqe(&queue->ring, &entry); } while (result == -EINTR);
    if (result < 0) return (io_wait_result){.tag = IO_WAIT_SYSTEM_ERROR, .native_error = -result};
    *completion = IoCompletion(entry);
    io_uring_cqe_seen(&queue->ring, entry);
    --queue->outstanding;
    return (io_wait_result){.tag = IO_WAIT_OK};
}

tcp_listen_result TcpListen(u32 address, u16 port, i32 backlog)
{
    if (backlog <= 0) {
        return (tcp_listen_result){.tag = TCP_LISTEN_INVALID_ARGUMENT};
    }
    i32 listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        return (tcp_listen_result){.tag = TCP_LISTEN_SYSTEM_ERROR, .native_error = errno};
    }
    int reuse = 1;
    struct sockaddr_in endpoint = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(address)},
    };
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(listener, (struct sockaddr *)&endpoint, sizeof(endpoint)) < 0 ||
        listen(listener, backlog) < 0) {
        i32 error = errno;
        close(listener);
        return (tcp_listen_result){.tag = TCP_LISTEN_SYSTEM_ERROR, .native_error = error};
    }
    return (tcp_listen_result){.tag = TCP_LISTEN_OK, .value = (usize)listener};
}

tcp_accept_result TcpAccept(io_queue *queue, i32 listener, u64 id)
{
    if (queue == NULL || listener < 0) {
        return (tcp_accept_result){.tag = TCP_ACCEPT_INVALID_ARGUMENT};
    }
    struct io_uring_sqe *entry = IoNext(queue, id);
    if (entry == NULL) {
        return (tcp_accept_result){.tag = TCP_ACCEPT_NO_SPACE};
    }
    io_uring_prep_accept(entry, listener, NULL, NULL, SOCK_CLOEXEC);
    return (tcp_accept_result){.tag = TCP_ACCEPT_OK};
}

tcp_receive_result TcpReceive(io_queue *queue, i32 socket, byte_slice buffer, u64 id)
{
    if (queue == NULL || socket < 0 || (buffer.ptr == NULL && buffer.len != 0)) {
        return (tcp_receive_result){.tag = TCP_RECEIVE_INVALID_ARGUMENT};
    }
    if (buffer.len > INT_MAX) {
        return (tcp_receive_result){.tag = TCP_RECEIVE_MESSAGE_TOO_LARGE};
    }
    struct io_uring_sqe *entry = IoNext(queue, id);
    if (entry == NULL) {
        return (tcp_receive_result){.tag = TCP_RECEIVE_NO_SPACE};
    }
    io_uring_prep_recv(entry, socket, buffer.ptr, buffer.len, 0);
    return (tcp_receive_result){.tag = TCP_RECEIVE_OK};
}

tcp_send_result TcpSend(io_queue *queue, i32 socket, const_byte_slice buffer, u64 id)
{
    if (queue == NULL || socket < 0 || (buffer.ptr == NULL && buffer.len != 0)) {
        return (tcp_send_result){.tag = TCP_SEND_INVALID_ARGUMENT};
    }
    if (buffer.len > INT_MAX) {
        return (tcp_send_result){.tag = TCP_SEND_MESSAGE_TOO_LARGE};
    }
    struct io_uring_sqe *entry = IoNext(queue, id);
    if (entry == NULL) {
        return (tcp_send_result){.tag = TCP_SEND_NO_SPACE};
    }
    io_uring_prep_send(entry, socket, buffer.ptr, buffer.len, MSG_NOSIGNAL);
    return (tcp_send_result){.tag = TCP_SEND_OK};
}

tcp_close_result TcpClose(i32 socket)
{
    /* Linux releases the descriptor even on EINTR; never retry close. */
    if (close(socket) < 0) return (tcp_close_result){.tag = TCP_CLOSE_SYSTEM_ERROR, .native_error = errno};
    return (tcp_close_result){.tag = TCP_CLOSE_OK};
}
