#define _POSIX_C_SOURCE 200809L
#include "os.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* POSIX adapters are local to this test client. Keeping the client independent
 * of our io_uring implementation lets it check the server over real TCP. */
static void SleepMilliseconds(u32 milliseconds)
{
    struct timespec remaining = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) < 0) {
        assert(errno == EINTR);
    }
}

static i32 Connect(void)
{
    tcp_connect_result result = TcpConnect(0x7f000001, 8080);
    assert(result.tag == TCP_CONNECT_OK);
    return result.socket;
}

static void SendAll(i32 client, const_byte_slice bytes)
{
    while (bytes.len != 0) {
        usize count = bytes.len < 7919 ? bytes.len : 7919;
        isize sent = send(client, bytes.ptr, count, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        assert(sent > 0);
        bytes.ptr += sent;
        bytes.len -= (usize)sent;
    }
}

static void ReceiveExact(i32 client, byte_slice bytes)
{
    while (bytes.len != 0) {
        isize received = recv(client, bytes.ptr, bytes.len, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        assert(received > 0);
        bytes.ptr += received;
        bytes.len -= (usize)received;
    }
}

typedef struct writer_context {
    i32 client;
    const_byte_slice payload;
} writer_context;

static void WritePayload(void *argument)
{
    writer_context *context = argument;
    SendAll(context->client, context->payload);
    assert(shutdown(context->client, SHUT_WR) == 0);
}

static void Echo(const_byte_slice payload, u32 delay_ms)
{
    i32 client = Connect();
    assert(client >= 0);
    writer_context context = {client, payload};
    os_thread *writer;
    assert(ThreadStart(WritePayload, &context, (os_cpu_id_slice){0}, &writer).tag == THREAD_START_OK);
    SleepMilliseconds(delay_ms);
    u8 storage[16384];
    byte_slice buffer = {storage, sizeof(storage)};
    usize offset = 0;
    for (;;) {
        isize received = recv(client, buffer.ptr, buffer.len, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        assert(received >= 0);
        if (received == 0) {
            break;
        }
        assert((usize)received <= payload.len - offset);
        assert(memcmp(buffer.ptr, payload.ptr + offset, (usize)received) == 0);
        offset += (usize)received;
    }
    assert(offset == payload.len);
    assert(ThreadJoin(writer).tag == THREAD_JOIN_OK);
    assert(close(client) == 0);
}

static void ConcurrentEcho(void *argument)
{
    const_byte_slice *payload = argument;
    Echo(*payload, 0);
}

typedef struct thread_slice {
    os_thread **ptr;
    usize len;
} thread_slice;

typedef struct payload_slice {
    const_byte_slice *ptr;
    usize len;
} payload_slice;

static void TestConcurrent(void)
{
    enum { CLIENTS = 32, PAYLOAD_SIZE = 65537 };
    static u8 storage[CLIENTS * PAYLOAD_SIZE];
    byte_slice bytes = {storage, sizeof(storage)};
    os_thread *thread_storage[CLIENTS];
    thread_slice threads = {thread_storage, CLIENTS};
    const_byte_slice payload_storage[CLIENTS];
    payload_slice payloads = {payload_storage, CLIENTS};
    SliceFor(threads, index) {
        byte_slice part = {bytes.ptr + index * PAYLOAD_SIZE, PAYLOAD_SIZE};
        memset(part.ptr, (int)index, part.len);
        payloads.ptr[index] = (const_byte_slice){part.ptr, part.len};
        assert(ThreadStart(ConcurrentEcho, &payloads.ptr[index], (os_cpu_id_slice){0},
                           &threads.ptr[index]).tag == THREAD_START_OK);
    }
    SliceFor(threads, index) {
        assert(ThreadJoin(threads.ptr[index]).tag == THREAD_JOIN_OK);
    }
}

typedef struct socket_slice {
    i32 *ptr;
    usize len;
} socket_slice;

static void TestCapacity(void)
{
    i32 storage[128];
    socket_slice clients = {storage, 128};
    const u8 message = 'x';
    SliceFor(clients, index) {
        clients.ptr[index] = Connect();
        assert(clients.ptr[index] >= 0);
        SendAll(clients.ptr[index], (const_byte_slice){&message, 1});
        u8 reply;
        ReceiveExact(clients.ptr[index], (byte_slice){&reply, 1});
        assert(reply == message);
    }
    i32 waiting = Connect();
    assert(waiting >= 0);
    SendAll(waiting, (const_byte_slice){&message, 1});
    struct pollfd event = {.fd = waiting, .events = POLLIN};
    assert(poll(&event, 1, 100) == 0);
    assert(close(clients.ptr[--clients.len]) == 0);
    u8 reply;
    ReceiveExact(waiting, (byte_slice){&reply, 1});
    assert(reply == message);
    assert(close(waiting) == 0);
    SliceFor(clients, index) {
        assert(close(clients.ptr[index]) == 0);
    }
}

static void TestResets(void)
{
    static const u8 storage[40960] = {0};
    const_byte_slice bytes = {storage, sizeof(storage)};
    for (usize index = 0; index < 32; ++index) {
        i32 client = Connect();
        assert(client >= 0);
        struct linger reset = {.l_onoff = 1, .l_linger = 0};
        assert(setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
        SendAll(client, bytes);
        assert(close(client) == 0);
    }
    Echo(bytes, 0);
}

int main(void)
{
    /* Bash starts the server. Wait at most five seconds for it to listen. */
    tcp_connect_result ready = {.tag = TCP_CONNECT_REFUSED};
    for (usize attempt = 0; attempt < 250; ++attempt) {
        ready = TcpConnect(0x7f000001, 8080);
        if (ready.tag == TCP_CONNECT_OK) {
            break;
        }
        assert(ready.tag == TCP_CONNECT_REFUSED);
        SleepMilliseconds(20);
    }
    assert(ready.tag == TCP_CONNECT_OK);
    assert(close(ready.socket) == 0);

    Echo((const_byte_slice){0}, 0);
    static const u8 binary[] = {'h', 'i', 0, 0xff, '\n'};
    Echo((const_byte_slice){binary, sizeof(binary)}, 0);
    static u8 large_storage[4 * 1024 * 1024];
    byte_slice large = {large_storage, sizeof(large_storage)};
    SliceFor(large, index) {
        large.ptr[index] = (u8)index;
    }
    Echo((const_byte_slice){large.ptr, large.len}, 100);
    TestConcurrent();
    TestCapacity();
    TestResets();
    string_slice message = StringSlice(
        "PASS: binary/large echo, EOF, 32 concurrent clients, capacity, resets\n");
    assert(fwrite(message.ptr, 1, message.len, stdout) == message.len);
    return 0;
}
