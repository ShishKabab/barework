#define _POSIX_C_SOURCE 200809L
#include "os.h"
#include "slice.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const_byte_slice Bytes(string_slice text)
{
    return (const_byte_slice){(const u8 *)text.ptr, text.len};
}

static void Pause(void)
{
    struct timespec remaining = {.tv_nsec = 1000000};
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

static void SendAll(i32 client, const_byte_slice bytes, usize fragment)
{
    while (bytes.len != 0) {
        usize length = bytes.len < fragment ? bytes.len : fragment;
        isize sent = send(client, bytes.ptr, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        assert(sent > 0);
        bytes.ptr += sent;
        bytes.len -= (usize)sent;
        if (fragment == 1) {
            Pause();
        }
    }
}

static usize Find(const_byte_slice bytes, string_slice text)
{
    if (text.len <= bytes.len) {
        for (usize index = 0; index <= bytes.len - text.len; ++index) {
            if (memcmp(bytes.ptr + index, text.ptr, text.len) == 0) {
                return index;
            }
        }
    }
    return SIZE_MAX;
}

typedef struct response_reader {
    byte_slice storage;
    usize used;
} response_reader;

static void ExpectEof(i32 client, response_reader *reader)
{
    assert(reader->used == 0);
    u8 byte;
    isize count;
    do { count = recv(client, &byte, 1, 0); } while (count < 0 && errno == EINTR);
    assert(count == 0);
}

/* Frame by headers + Content-Length, preserving any subsequent response bytes. */
static i32 CheckNextResponse(i32 client, response_reader *reader, u32 code,
                             string_slice body, i32 head, i32 expected_keep_alive)
{
    usize end, size;
    for (;;) {
        const_byte_slice available = {reader->storage.ptr, reader->used};
        end = Find(available, StringSlice("\r\n\r\n"));
        if (end != SIZE_MAX) {
            size = end + 4 + (head ? 0 : body.len);
            assert(size <= reader->storage.len);
            if (reader->used >= size) break;
        }
        byte_slice free_bytes;
        assert(ByteSliceSubslice(reader->storage, reader->used,
                                 reader->storage.len - reader->used, &free_bytes).tag == BYTE_SLICE_SUBSLICE_OK);
        assert(free_bytes.len != 0);
        isize count = recv(client, free_bytes.ptr, free_bytes.len, 0);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        reader->used += (usize)count;
    }
    const_byte_slice response = {reader->storage.ptr, size};
    assert(size > 12 && memcmp(response.ptr, "HTTP/1.1 ", 9) == 0);
    u32 actual = (response.ptr[9] - '0') * 100 + (response.ptr[10] - '0') * 10 + response.ptr[11] - '0';
    if (actual != code) fwrite(response.ptr, 1, response.len, stderr);
    assert(actual == code);
    i32 keep_alive = Find(response, StringSlice("\r\nConnection: keep-alive\r\n")) != SIZE_MAX;
    assert(keep_alive || Find(response, StringSlice("\r\nConnection: close\r\n")) != SIZE_MAX);
    if (expected_keep_alive >= 0) assert(keep_alive == expected_keep_alive);
    string_slice key = StringSlice("\r\nContent-Length: ");
    usize start = Find(response, key);
    if (code == 204 || code == 304) {
        assert(start == SIZE_MAX && body.len == 0);
    } else {
    assert(start != SIZE_MAX);
    start += key.len;
    usize length = 0;
    while (start < end && response.ptr[start] >= '0' && response.ptr[start] <= '9') {
        assert(length < 8192);
        length = length * 10 + response.ptr[start++] - '0';
    }
    assert(response.ptr[start] == '\r' && length == body.len);
    }
    if (code == 200 || code == 204) {
        assert(Find(response, StringSlice("\r\nContent-Type: text/plain; charset=utf-8\r\n")) != SIZE_MAX ||
            Find(response, StringSlice("\r\nContent-Type: application/json\r\n")) != SIZE_MAX);
        assert(Find(response, StringSlice("\r\nX-Test-")) == SIZE_MAX);
    }
    const_byte_slice received_body;
    assert(ConstByteSliceSubslice(response, end + 4, response.len - end - 4, &received_body).tag == CONST_BYTE_SLICE_SUBSLICE_OK);
    assert(received_body.len == (head ? 0 : body.len));
    if (!head) {
        SliceFor(received_body, index) { assert(received_body.ptr[index] == (u8)body.ptr[index]); }
    }
    usize remaining = reader->used - size;
    assert(ByteSliceCopyRange(reader->storage, 0, (const_byte_slice){reader->storage.ptr, reader->used},
                              size, remaining).tag == BYTE_SLICE_COPY_RANGE_OK);
    reader->used = remaining;
    return keep_alive;
}

static void CheckResponse(i32 client, u32 code, string_slice body, i32 head)
{
    u8 storage[8192];
    response_reader reader = {.storage = {storage, sizeof(storage)}};
    if (!CheckNextResponse(client, &reader, code, body, head, -1)) ExpectEof(client, &reader);
}
static void Request(const_byte_slice request, u32 code, string_slice body,
                    i32 head, usize fragment, i32 eof)
{
    i32 client = Connect();
    assert(client >= 0);
    SendAll(client, request, fragment);
    if (eof) {
        assert(shutdown(client, SHUT_WR) == 0);
    }
    CheckResponse(client, code, body, head);
    assert(close(client) == 0);
}

typedef struct request_case {
    string_slice request;
    u32 code;
    string_slice body;
    i32 head;
} request_case;

typedef struct case_slice {
    request_case *ptr;
    usize len;
} case_slice;

static void TestCases(void)
{
    request_case storage[] = {
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\n"), 200, StringSlice("Hello, world!\n"), 0},
        {StringSlice("HEAD / HTTP/1.1\r\nHost: test\r\n\r\n"), 200, StringSlice("Hello, world!\n"), 1},
        {StringSlice("GET /empty HTTP/1.1\r\nHost: test\r\n\r\n"), 204, StringSlice(""), 0},
        {StringSlice("GET /profile/0?view=full HTTP/1.1\r\nHost: test\r\n\r\n"), 200, StringSlice("[]"), 0},
        {StringSlice("HEAD /timeline/0 HTTP/1.1\r\nHost: test\r\n\r\n"), 200, StringSlice("[]"), 1},
        {StringSlice("GET /profile/a%2Fb HTTP/1.1\r\nHost: test\r\n\r\n"), 400, StringSlice("{\"error\":\"invalid route parameter\"}"), 0},
        {StringSlice("GET /profile/13 HTTP/1.1\r\nHost: test\r\n\r\n"), 503, StringSlice("{\"error\":\"query unavailable\"}"), 0},
        {StringSlice("GET /profile-summary/0 HTTP/1.1\r\nHost: test\r\n\r\n"), 200, StringSlice("[]"), 0},
        {StringSlice("GET /profile/ HTTP/1.1\r\nHost: test\r\n\r\n"), 404, StringSlice("Not found\n"), 0},
        {StringSlice("GET /profile/alice/ HTTP/1.1\r\nHost: test\r\n\r\n"), 404, StringSlice("Not found\n"), 0},
        {StringSlice("HEAD /empty HTTP/1.1\r\nHost: test\r\n\r\n"), 204, StringSlice(""), 1},
        {StringSlice("GET /missing HTTP/1.1\r\nHost: test\r\n\r\n"), 404, StringSlice("Not found\n"), 0},
        {StringSlice("HEAD /missing HTTP/1.1\r\nHost: test\r\n\r\n"), 404, StringSlice("Not found\n"), 1},
        {StringSlice("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n"), 405, StringSlice("Method not allowed\n"), 0},
        {StringSlice("GET / HTTP/1.0\r\n\r\n"), 200, StringSlice("Hello, world!\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nhOsT: test\r\ncontent-length: 0\r\n\r\n"), 200, StringSlice("Hello, world!\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nHost: two\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: \r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\nHost: test\n\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nX : value\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\n folded\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nContent-Length: -1\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nContent-Length: 18446744073709551616\r\n\r\n"), 400, StringSlice("Bad request\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n"), 501, StringSlice("Not implemented\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\n\r\nX"), 501, StringSlice("Not implemented\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\n\r\n"), 501, StringSlice("Not implemented\n"), 0},
        {StringSlice("POST / HTTP/1.1\r\nHost: test\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n"), 417, StringSlice("Expectation failed\n"), 0},
        {StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\nGET /missing HTTP/1.1\r\nHost: test\r\n\r\n"), 200, StringSlice("Hello, world!\n"), 0},
    };
    case_slice cases = {storage, sizeof(storage) / sizeof(storage[0])};
    SliceFor(cases, index) {
        request_case test = cases.ptr[index];
        Request(Bytes(test.request), test.code, test.body, test.head, 4096, 0);
    }
    Request(Bytes(cases.ptr[0].request), 200, cases.ptr[0].body, 0, 1, 0);
    Request(Bytes(StringSlice("GET / HTTP/1.1\r\nHost: te")), 400, StringSlice("Bad request\n"), 0, 7, 1);
}

static void TestLimits(void)
{
    static u8 storage[65537];
    byte_slice bytes = {storage, sizeof(storage)};
    string_slice prefix = StringSlice("GET / HTTP/1.1\r\nHost: test\r\nX: ");
    memset(bytes.ptr, 'a', bytes.len);
    memcpy(bytes.ptr, prefix.ptr, prefix.len);
    string_slice end = StringSlice("\r\n\r\n");
    memcpy(bytes.ptr + 16384, end.ptr, end.len);
    Request((const_byte_slice){bytes.ptr, 16384 + end.len}, 200, StringSlice("Hello, world!\n"), 0, 1024, 0);
    memset(bytes.ptr + prefix.len, 'a', bytes.len - prefix.len);
    Request((const_byte_slice){bytes.ptr, bytes.len}, 431, StringSlice("Headers too large\n"), 0, 4096, 0);
    usize used = 0;
    string_slice request_line = StringSlice("GET / HTTP/1.1\r\n");
    memcpy(bytes.ptr, request_line.ptr, request_line.len);
    used += request_line.len;
    for (usize index = 0; index < 129; ++index) {
        string_slice header = StringSlice("X: a\r\n");
        memcpy(bytes.ptr + used, header.ptr, header.len);
        used += header.len;
    }
    Request((const_byte_slice){bytes.ptr, used}, 431, StringSlice("Headers too large\n"), 0, 4096, 0);
}

static void ConcurrentRequest(void *context)
{
    const request_case *test = context;
    Request(Bytes(test->request), test->code, test->body, 0, 7, 0);
}

typedef struct thread_slice {
    os_thread **ptr;
    usize len;
} thread_slice;

static void TestConcurrentAndResets(void)
{
    request_case valid = {StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\n"), 200, StringSlice("Hello, world!\n"), 0};
    request_case missing = {StringSlice("GET /missing HTTP/1.1\r\nHost: test\r\n\r\n"), 404, StringSlice("Not found\n"), 0};
    os_thread *storage[32];
    thread_slice threads = {storage, 32};
    SliceFor(threads, index) {
        assert(ThreadStart(ConcurrentRequest, index % 2 ? &valid : &missing, (os_cpu_id_slice){0}, &threads.ptr[index]).tag == THREAD_START_OK);
    }
    SliceFor(threads, index) {
        assert(ThreadJoin(threads.ptr[index]).tag == THREAD_JOIN_OK);
    }
    for (usize index = 0; index < 32; ++index) {
        i32 client = Connect();
        assert(client >= 0);
        SendAll(client, Bytes(StringSlice("GET / HTTP/1.1\r\nHost: ")), 4096);
        struct linger reset = {.l_onoff = 1, .l_linger = 0};
        assert(setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
        assert(close(client) == 0);
    }
    for (usize index = 0; index < 100; ++index) {
        Request(Bytes(valid.request), valid.code, valid.body, 0, 4096, 0);
    }
}

typedef struct benchmark_client {
    usize requests;
    usize requests_per_connection;
    i32 start_event;
    os_thread *thread;
} benchmark_client;
typedef struct benchmark_slice { benchmark_client *ptr; usize len; } benchmark_slice;

static void RequestConnection(usize count)
{
    i32 client = Connect();
    assert(client >= 0);
    u8 storage[8192];
    response_reader reader = {.storage = {storage, sizeof(storage)}};
    for (usize index = 0; index < count; ++index) {
        i32 keep_alive = index + 1 < count;
        string_slice request = keep_alive ?
            StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\n") :
            StringSlice("GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        SendAll(client, Bytes(request), 4096);
        CheckNextResponse(client, &reader, 200, StringSlice("Hello, world!\n"), 0, keep_alive);
    }
    ExpectEof(client, &reader);
    assert(close(client) == 0);
}

static void BenchmarkClient(void *context)
{
    benchmark_client *client = context;
    assert(OsEventWait(client->start_event).tag == OS_EVENT_WAIT_OK);
    for (usize index = 0; index < client->requests; index += client->requests_per_connection) {
        RequestConnection(client->requests_per_connection);
    }
}

static void TestPersistence(void)
{
    RequestConnection(10);
    RequestConnection(25); /* No server-side ten-request cap. */
    u8 storage[8192];
    response_reader reader = {.storage = {storage, sizeof(storage)}};
    i32 client = Connect();
    assert(client >= 0);
    SendAll(client, Bytes(StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\nGET /miss")), 4096);
    CheckNextResponse(client, &reader, 200, StringSlice("Hello, world!\n"), 0, 1);
    SendAll(client, Bytes(StringSlice("ing HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n")), 1);
    CheckNextResponse(client, &reader, 404, StringSlice("Not found\n"), 0, 0);
    ExpectEof(client, &reader);
    assert(close(client) == 0);

    client = Connect();
    assert(client >= 0);
    SendAll(client, Bytes(StringSlice("GET / HTTP/1.0\r\nConnection: KeEp-AlIvE\r\n\r\n")), 4096);
    CheckNextResponse(client, &reader, 200, StringSlice("Hello, world!\n"), 0, 1);
    SendAll(client, Bytes(StringSlice("HEAD / HTTP/1.1\r\nHost: test\r\nConnection: x-close\r\n\r\n"
        "GET /empty HTTP/1.1\r\nHost: test\r\n\r\n"
        "POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n"
        "GET / HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\nConnection: foo, cLoSe\r\n\r\n"
        "GET / HTTP/1.1\r\nHost: test\r\n\r\n")), 4096);
    CheckNextResponse(client, &reader, 200, StringSlice("Hello, world!\n"), 1, 1);
    CheckNextResponse(client, &reader, 204, StringSlice(""), 0, 1);
    CheckNextResponse(client, &reader, 405, StringSlice("Method not allowed\n"), 0, 1);
    CheckNextResponse(client, &reader, 200, StringSlice("Hello, world!\n"), 0, 0);
    ExpectEof(client, &reader); /* Close ignores the extra pipelined request. */
    assert(close(client) == 0);

    client = Connect();
    assert(client >= 0);
    SendAll(client, Bytes(StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\nGET / HTTP/1.1\r\nHost: te")), 4096);
    assert(shutdown(client, SHUT_WR) == 0);
    CheckNextResponse(client, &reader, 200, StringSlice("Hello, world!\n"), 0, 1);
    CheckNextResponse(client, &reader, 400, StringSlice("Bad request\n"), 0, 0);
    ExpectEof(client, &reader);
    assert(close(client) == 0);

    Request(Bytes(StringSlice("GET / HTTP/1.1\r\nHost: test\r\nConnection: cl ose\r\n\r\n")),
            400, StringSlice("Bad request\n"), 0, 4096, 0);
    Request(Bytes(StringSlice("GET / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\n\r\nX"
                             "GET / HTTP/1.1\r\nHost: test\r\n\r\n")),
            501, StringSlice("Not implemented\n"), 0, 4096, 0);

    /* Pipeline crosses the server's receive-buffer boundary. */
    u8 request_storage[16384];
    byte_slice requests = {request_storage, sizeof(request_storage)};
    usize used = 0;
    for (usize index = 0; index < 150; ++index) {
        string_slice request = index == 149 ?
            StringSlice("GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n") :
            StringSlice("GET / HTTP/1.1\r\nHost: test\r\n\r\n");
        assert(ByteSliceCopy(requests, used, Bytes(request)).tag == BYTE_SLICE_COPY_OK);
        used += request.len;
    }
    assert(used > 4096);
    client = Connect();
    assert(client >= 0);
    SendAll(client, (const_byte_slice){requests.ptr, used}, 4096);
    for (usize index = 0; index < 150; ++index) {
        CheckNextResponse(client, &reader, 200, StringSlice("Hello, world!\n"), 0, index != 149);
    }
    ExpectEof(client, &reader);
    assert(close(client) == 0);
}

static usize BenchmarkNumber(string_slice number, usize maximum)
{
    usize result = 0;
    assert(number.len != 0);
    SliceFor(number, index) {
        assert(number.ptr[index] >= '0' && number.ptr[index] <= '9');
        usize digit = (usize)(number.ptr[index] - '0');
        assert(result <= (maximum - digit) / 10);
        result = result * 10 + digit;
    }
    assert(result > 0 && result <= maximum);
    return result;
}

static void Benchmark(usize requests, usize concurrency, usize requests_per_connection)
{
    assert(requests % requests_per_connection == 0);
    usize connections = requests / requests_per_connection;
    benchmark_client storage[1024];
    benchmark_slice clients = {storage, concurrency};
    for (usize index = 0; index < 64; ++index) {
        RequestConnection(requests_per_connection);
    }
    SliceFor(clients, index) {
        clients.ptr[index] = (benchmark_client){
            .requests = (connections / concurrency + (index < connections % concurrency)) * requests_per_connection,
            .requests_per_connection = requests_per_connection};
        assert(OsEventCreate(&clients.ptr[index].start_event).tag == OS_EVENT_CREATE_OK);
        assert(ThreadStart(BenchmarkClient, &clients.ptr[index], (os_cpu_id_slice){0}, &clients.ptr[index].thread).tag == THREAD_START_OK);
    }
    u64 begin, end;
    assert(OsMonotonicNanoseconds(&begin).tag == OS_MONOTONIC_NANOSECONDS_OK);
    SliceFor(clients, index) { assert(OsEventSignal(clients.ptr[index].start_event).tag == OS_EVENT_SIGNAL_OK); }
    SliceFor(clients, index) {
        assert(ThreadJoin(clients.ptr[index].thread).tag == THREAD_JOIN_OK);
        OsEventClose(clients.ptr[index].start_event);
    }
    assert(OsMonotonicNanoseconds(&end).tag == OS_MONOTONIC_NANOSECONDS_OK && end > begin);
    f64 seconds = (f64)(end - begin) / 1e9;
    /* printf is the report-formatting API boundary. Each response was checked. */
    printf("%zu,%zu,%.6f,%.2f\n", requests, concurrency, seconds, requests / seconds);
}

int main(int argc, char **argv)
{
    if (argc != 1 && ((argc != 4 && argc != 5) || strcmp(argv[1], "--benchmark") != 0)) {
        TerminalWrite(StringSlice("Usage: http-server-test [--benchmark requests concurrency [requests-per-connection]]\n"));
        return 1;
    }
    tcp_connect_result ready = {.tag = TCP_CONNECT_REFUSED};
    for (usize attempt = 0; attempt < 5000; ++attempt) {
        ready = TcpConnect(0x7f000001, 8080);
        if (ready.tag == TCP_CONNECT_OK) {
            break;
        }
        assert(ready.tag == TCP_CONNECT_REFUSED);
        Pause();
    }
    assert(ready.tag == TCP_CONNECT_OK);
    assert(close(ready.socket) == 0);
    if (argc >= 4) {
        /* argv's terminators belong to the C runtime interface. */
        usize requests = BenchmarkNumber((string_slice){argv[2], strlen(argv[2])}, 100000000);
        usize concurrency = BenchmarkNumber((string_slice){argv[3], strlen(argv[3])}, 1024);
        usize per_connection = argc == 5 ? BenchmarkNumber((string_slice){argv[4], strlen(argv[4])}, 1000000) : 1;
        Benchmark(requests, concurrency, per_connection);
        return 0;
    }
    TestCases();
    TestLimits();
    TestConcurrentAndResets();
    TestPersistence();
    string_slice message = StringSlice("PASS: TCP -> connection Wasm -> response, fragments, HEAD, errors, limits, concurrency, resets\n");
    assert(fwrite(message.ptr, 1, message.len, stdout) == message.len);
    return 0;
}
