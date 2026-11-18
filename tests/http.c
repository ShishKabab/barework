#include "http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static byte_slice Allocate(bump_allocator *arena, usize size, usize alignment)
{
    bump_allocate_result result = BumpAllocate(arena, size, alignment);
    assert(result.tag == BUMP_ALLOCATE_OK);
    return result.allocation;
}
static const_byte_slice Bytes(string_slice text)
{
    return (const_byte_slice){(const u8 *)text.ptr, text.len};
}

static void ExpectText(const_byte_slice message, u32 offset, u32 length, string_slice expected)
{
    assert(offset <= message.len && length <= message.len - offset);
    assert(length == expected.len);
    if (length != 0) {
        assert(offset >= sizeof(http_request));
        assert(memcmp(message.ptr + offset, expected.ptr, length) == 0);
    }
}

typedef struct expected_header {
    string_slice key;
    string_slice value;
} expected_header;

typedef struct expected_header_slice {
    expected_header *ptr;
    usize len;
} expected_header_slice;

static void CheckMessage(const_byte_slice message)
{
    assert(message.len >= sizeof(http_request));
    http_request request;
    memcpy(&request, message.ptr, sizeof(request));
    assert(request.MessageLength == message.len);
    assert(request.VersionMajor == 1 && request.VersionMinor == 1);
    ExpectText(message, request.MethodOffset, request.MethodLength, StringSlice("POST"));
    ExpectText(message, request.TargetOffset, request.TargetLength, StringSlice("/submit?q=x"));
    expected_header header_storage[] = {
        {StringSlice("Host"), StringSlice("example.test")},
        {StringSlice("X-Key"), StringSlice("alpha\t beta")},
        {StringSlice("X-Key"), StringSlice("second")},
        {StringSlice("Empty"), StringSlice("")},
        {StringSlice("Content-Length"), StringSlice("4")},
    };
    expected_header_slice headers = {header_storage, sizeof(header_storage) / sizeof(header_storage[0])};
    assert(request.HeaderCount == headers.len);
    u32 offset = request.FirstHeaderOffset;
    SliceFor(headers, index) {
        assert(offset >= sizeof(request) && offset <= message.len);
        assert(sizeof(http_header) <= message.len - offset);
        assert(offset % _Alignof(http_header) == 0);
        http_header header;
        memcpy(&header, message.ptr + offset, sizeof(header));
        ExpectText(message, header.KeyOffset, header.KeyLength, headers.ptr[index].key);
        ExpectText(message, header.ValueOffset, header.ValueLength, headers.ptr[index].value);
        assert(header.NextHeaderOffset == 0 || header.NextHeaderOffset > offset);
        offset = header.NextHeaderOffset;
    }
    assert(offset == 0);
}

static void TestBump(void)
{
    _Alignas(16) u8 storage[64];
    memset(storage, 0xa5, sizeof(storage));
    bump_allocator allocator;
    assert(BumpInit(&allocator, (byte_slice){storage + 1, sizeof(storage) - 1}).tag == BUMP_INIT_OK);
    byte_slice allocation = Allocate(&allocator, 7, 16);
    assert(allocation.len == 7 && (uintptr_t)allocation.ptr % 16 == 0);
    byte_slice initialized = {allocator.storage.ptr, allocator.used};
    SliceFor(initialized, index) {
        assert(initialized.ptr[index] == 0);
    }
    usize before = allocator.used;
    assert(BumpAllocate(&allocator, SIZE_MAX, 1).tag == BUMP_ALLOCATE_NO_SPACE);
    assert(BumpAllocate(&allocator, 1, 3).tag == BUMP_ALLOCATE_INVALID_ARGUMENT);
    assert(BumpAllocate(&allocator, 1, 0).tag == BUMP_ALLOCATE_INVALID_ARGUMENT);
    assert(allocator.used == before);
    usize remaining = allocator.storage.len - before;
    assert(Allocate(&allocator, remaining, 1).len == remaining);
    assert(BumpAllocate(&allocator, 1, 1).tag == BUMP_ALLOCATE_NO_SPACE);
    assert(storage[0] == 0xa5);
    assert(BumpInit(NULL, (byte_slice){0}).tag == BUMP_INIT_INVALID_ARGUMENT);
}

static void TestFragments(void)
{
    const_byte_slice input = Bytes(StringSlice(
        "POST /submit?q=x HTTP/1.1\r\nHost: example.test\r\n"
        "X-Key: \talpha\t beta \t\r\nX-Key:second\r\nEmpty: \t \r\n"
        "Content-Length: 4\r\n\r\nDATA"));
    usize head_length = input.len - 4;
    _Alignas(http_request) u8 reference_storage[1024];
    http_parser reference;
    assert(HttpParserInit(&reference, (byte_slice){reference_storage, sizeof(reference_storage)},
                          HttpDefaultLimits()).tag == HTTP_PARSER_INIT_READY);
    http_parse_result result = HttpParserParse(&reference, input);
    assert(result.status == HTTP_PARSE_COMPLETE && result.consumed == head_length);
    assert(result.remainder.ptr == input.ptr + head_length && result.remainder.len == 4);
    const_byte_slice expected = HttpParserMessage(&reference);
    CheckMessage(expected);

    for (usize split = 0; split <= head_length; ++split) {
        _Alignas(http_request) u8 storage[1024];
        http_parser parser;
        assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)},
                              HttpDefaultLimits()).tag == HTTP_PARSER_INIT_READY);
        result = HttpParserParse(&parser, (const_byte_slice){input.ptr, split});
        assert(result.consumed == split);
        assert(result.status == (split == head_length ? HTTP_PARSE_COMPLETE : HTTP_PARSE_NEED_MORE));
        result = HttpParserParse(&parser, (const_byte_slice){input.ptr + split, input.len - split});
        assert(result.status == HTTP_PARSE_COMPLETE && result.consumed == head_length - split);
        assert(result.remainder.len == 4 && memcmp(result.remainder.ptr, "DATA", 4) == 0);
        const_byte_slice message = HttpParserMessage(&parser);
        assert(message.len == expected.len && memcmp(message.ptr, expected.ptr, message.len) == 0);
        assert(HttpParserFinish(&parser).tag == HTTP_PARSER_FINISH_COMPLETE);
    }

    /* Every input chunk is overwritten after parsing; output must own its bytes.
     * Both the arena and the relocated message deliberately start unaligned. */
    _Alignas(http_request) u8 storage[1024];
    http_parser parser;
    assert(HttpParserInit(&parser, (byte_slice){storage + 1, sizeof(storage) - 1},
                          HttpDefaultLimits()).tag == HTTP_PARSER_INIT_READY);
    for (usize index = 0; index < head_length; ++index) {
        u8 byte = input.ptr[index];
        result = HttpParserParse(&parser, (const_byte_slice){&byte, 1});
        assert(result.consumed == 1);
        byte = 0;
    }
    assert(result.status == HTTP_PARSE_COMPLETE);
    const_byte_slice message = HttpParserMessage(&parser);
    assert(message.len == expected.len && memcmp(message.ptr, expected.ptr, message.len) == 0);
    u8 copied_storage[1025];
    const_byte_slice copied = {copied_storage + 1, message.len};
    memcpy(copied_storage + 1, message.ptr, message.len);
    memset(storage, 0xa5, sizeof(storage));
    memset(&parser, 0, sizeof(parser));
    CheckMessage(copied);

    /* Exhaust every possible storage prefix; never publish a partial message. */
    for (usize capacity = 0; capacity <= expected.len; ++capacity) {
        assert(HttpParserInit(&parser, (byte_slice){storage, capacity}, HttpDefaultLimits()).tag != HTTP_PARSER_INIT_INVALID_ARGUMENT);
        result = HttpParserParse(&parser, input);
        assert(result.status == (capacity == expected.len ? HTTP_PARSE_COMPLETE : HTTP_PARSE_NO_SPACE));
        assert(HttpParserMessage(&parser).len == (capacity == expected.len ? expected.len : 0));
    }
    for (usize length = 0; length < head_length; ++length) {
        assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)},
                              HttpDefaultLimits()).tag == HTTP_PARSER_INIT_READY);
        result = HttpParserParse(&parser, (const_byte_slice){input.ptr, length});
        assert(result.status == HTTP_PARSE_NEED_MORE);
        assert(HttpParserFinish(&parser).tag == HTTP_PARSER_FINISH_TRUNCATED);
        assert(HttpParserMessage(&parser).len == 0);
        result = HttpParserParse(&parser, input);
        assert(result.status == HTTP_PARSE_TRUNCATED && result.consumed == 0);
    }
}

typedef struct text_slice {
    string_slice *ptr;
    usize len;
} text_slice;

static void TestSyntax(void)
{
    string_slice invalid_storage[] = {
        StringSlice(" GET / HTTP/1.1\r\n\r\n"),
        StringSlice("G(ET / HTTP/1.1\r\n\r\n"),
        StringSlice("GET  / HTTP/1.1\r\n\r\n"),
        StringSlice("GET /\t HTTP/1.1\r\n\r\n"),
        StringSlice("GET / HTTP/2.0\r\n\r\n"),
        StringSlice("GET / HTTP/1.10\r\n\r\n"),
        StringSlice("GET / HTTP/1.1\n\n"),
        StringSlice("GET / HTTP/1.1\rX"),
        StringSlice("GET / HTTP/1.1\r\nX : y\r\n\r\n"),
        StringSlice("GET / HTTP/1.1\r\n: y\r\n\r\n"),
        StringSlice("GET / HTTP/1.1\r\nX: a\r\n b\r\n\r\n"),
        StringSlice("GET / HTTP/1.1\r\nX: a\0b\r\n\r\n"),
        StringSlice("GET / HTTP/1.1\r\nX: a\x7f\r\n\r\n"),
        StringSlice("GET / HTTP/1.1\r\nX: a\n\r\n"),
        StringSlice("GET / HTTP/1.1\r\n\rX"),
    };
    text_slice invalid = {invalid_storage, sizeof(invalid_storage) / sizeof(invalid_storage[0])};
    u8 storage[1024];
    http_parser parser;
    SliceFor(invalid, index) {
        const_byte_slice input = Bytes(invalid.ptr[index]);
        for (usize split = 0; split <= input.len; ++split) {
            assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)},
                                  HttpDefaultLimits()).tag == HTTP_PARSER_INIT_READY);
            HttpParserParse(&parser, (const_byte_slice){input.ptr, split});
            http_parse_result result = HttpParserParse(&parser,
                (const_byte_slice){input.ptr + split, input.len - split});
            assert(result.status == HTTP_PARSE_INVALID);
            assert(HttpParserMessage(&parser).len == 0);
            assert(HttpParserParse(&parser, input).consumed == 0);
        }
    }
    const_byte_slice input = Bytes(StringSlice("OPTIONS * HTTP/1.0\r\nRaw:\x80\xff\r\n\r\n"));
    assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)},
                          HttpDefaultLimits()).tag == HTTP_PARSER_INIT_READY);
    assert(HttpParserParse(&parser, input).status == HTTP_PARSE_COMPLETE);
    http_request request;
    const_byte_slice message = HttpParserMessage(&parser);
    memcpy(&request, message.ptr, sizeof(request));
    assert(request.VersionMinor == 0);
    http_header header;
    memcpy(&header, message.ptr + request.FirstHeaderOffset, sizeof(header));
    ExpectText(message, header.ValueOffset, header.ValueLength, StringSlice("\x80\xff"));

    input = Bytes(StringSlice("GET / HTTP/1.1\r\n\r\n"));
    http_parse_limits limits = {.max_bytes = (u32)input.len, .max_headers = 0};
    assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)}, limits).tag == HTTP_PARSER_INIT_READY);
    assert(HttpParserParse(&parser, input).status == HTTP_PARSE_COMPLETE);
    message = HttpParserMessage(&parser);
    memcpy(&request, message.ptr, sizeof(request));
    assert(request.HeaderCount == 0 && request.FirstHeaderOffset == 0);
    --limits.max_bytes;
    assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)}, limits).tag == HTTP_PARSER_INIT_READY);
    assert(HttpParserParse(&parser, input).status == HTTP_PARSE_LIMIT);
    limits.max_bytes = 64;
    input = Bytes(StringSlice("GET / HTTP/1.1\r\nX: \r\n\r\n"));
    assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)}, limits).tag == HTTP_PARSER_INIT_READY);
    assert(HttpParserParse(&parser, input).status == HTTP_PARSE_LIMIT);
    limits.max_headers = 1;
    assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)}, limits).tag == HTTP_PARSER_INIT_READY);
    assert(HttpParserParse(&parser, input).status == HTTP_PARSE_COMPLETE);
    assert(HttpParserInit(&parser, (byte_slice){storage, sizeof(storage)}, limits).tag == HTTP_PARSER_INIT_READY);
    assert(HttpParserParse(&parser, (const_byte_slice){NULL, 1}).status == HTTP_PARSE_INVALID);
}

static void TestOwnedArena(void)
{
    http_parser parser;
    assert(HttpParserCreate(&parser, 1024 * 1024, HttpDefaultLimits()).tag == HTTP_PARSER_CREATE_READY);
    u8 *base = parser.arena.storage.ptr;
    assert(HttpParserParse(&parser, Bytes(StringSlice("GET / HTTP/1.1\r\nX: "))).status == HTTP_PARSE_NEED_MORE);
    u8 storage[8192];
    byte_slice value = {storage, sizeof(storage)};
    memset(value.ptr, 'a', value.len);
    assert(HttpParserParse(&parser, (const_byte_slice){value.ptr, value.len}).status == HTTP_PARSE_NEED_MORE);
    assert(HttpParserParse(&parser, Bytes(StringSlice("\r\n\r\n"))).status == HTTP_PARSE_COMPLETE);
    const_byte_slice message = HttpParserMessage(&parser);
    assert(message.ptr == base);
    http_request request;
    http_header header;
    memcpy(&request, message.ptr, sizeof(request));
    memcpy(&header, message.ptr + request.FirstHeaderOffset, sizeof(header));
    assert(header.ValueLength == value.len);
    assert(memcmp(message.ptr + header.ValueOffset, value.ptr, value.len) == 0);
    HttpParserDestroy(&parser);
    assert(HttpParserMessage(&parser).len == 0);
}

int main(void)
{
    TestBump();
    TestFragments();
    TestSyntax();
    TestOwnedArena();
    string_slice message = StringSlice("PASS: HTTP fragmentation, relocation, syntax, limits, exhaustion, EOF\n");
    assert(fwrite(message.ptr, 1, message.len, stdout) == message.len);
    return 0;
}
