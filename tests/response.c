#include "http_response_message.h"
#include "http_response.h"
#include "slice.h"
#include <assert.h>
#include <stdio.h>

static bool Contains(const_byte_slice bytes, string_slice text)
{
    SliceFor(bytes, i) {
        if (text.len > bytes.len - i) break;
        bool same = true;
        SliceFor(text, j) { if (bytes.ptr[i + j] != (u8)text.ptr[j]) same = false; }
        if (same) return true;
    }
    return false;
}
static http_response Root(const_byte_slice message)
{
    http_response root;
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&root, sizeof(root)}, 0, message, 0, sizeof(root)).tag == BYTE_SLICE_COPY_RANGE_OK);
    return root;
}
static void TestCopyAndSerialize(void)
{
    u8 storage[4096], relocated[4097], wire_storage[4096];
    bump_allocator arena;
    assert(BumpInit(&arena, (byte_slice){storage, sizeof(storage)}).tag == BUMP_INIT_OK);
    char first[] = "first", second[] = "second";
    u8 body[] = {0, 1, 255, 'x'};
    const http_response_header headers[] = {
        {StringSlice("Content-Type"), StringSlice("application/octet-stream")},
        {StringSlice("X-Test-One"), StringSlice(first)},
        {StringSlice("X-Test-Two"), StringSlice(second)},
        {StringSlice("Set-Cookie"), StringSlice("a=1")},
        {StringSlice("Set-Cookie"), StringSlice("b=2")},
        {StringSlice("X-Empty"), StringSlice("")},
    };
    http_response_build_result built = HttpResponseBuild(&arena, 201,
        (http_response_header_slice){headers, sizeof(headers) / sizeof(headers[0])}, BytesOf(body));
    assert(built.tag == HTTP_RESPONSE_BUILD_OK);
    first[0] = 'X'; second[0] = 'X'; body[0] = 99;
    assert(HttpResponseValidate(built.message).tag == HTTP_RESPONSE_VALIDATE_OK);
    /* The descriptor can be moved to an unaligned address with no fixups. */
    byte_slice copy;
    assert(ByteSliceSubslice((byte_slice){relocated, sizeof(relocated)}, 1, built.message.len, &copy).tag == BYTE_SLICE_SUBSLICE_OK);
    assert(ByteSliceCopy(copy, 0, built.message).tag == BYTE_SLICE_COPY_OK);
    const_byte_slice message = {copy.ptr, copy.len};
    BumpReset(&arena);
    assert(ByteSliceZero((byte_slice){storage, sizeof(storage)}).tag == BYTE_SLICE_ZERO_OK);
    assert(HttpResponseValidate(message).tag == HTTP_RESPONSE_VALIDATE_OK);
    http_response root = Root(message);
    assert(root.StatusCode == 201 && root.HeaderCount == 6 && root.Body.len == 4);
    const_byte_range_resolve_result data = ConstByteRangeResolve(message, root.Body);
    assert(data.tag == CONST_BYTE_RANGE_RESOLVE_OK && data.bytes.ptr[0] == 0 && data.bytes.ptr[2] == 255);
    byte_slice wire = {wire_storage, sizeof(wire_storage)};
    http_response_serialize_result serialized = HttpResponseSerialize(message, 0, 1, wire);
    assert(serialized.tag == HTTP_RESPONSE_SERIALIZE_OK);
    assert(Contains(serialized.bytes, StringSlice("X-Test-One: first\r\nX-Test-Two: second\r\n")));
    assert(Contains(serialized.bytes, StringSlice("Set-Cookie: a=1\r\nSet-Cookie: b=2\r\nX-Empty: \r\n")));
    assert(Contains(serialized.bytes, StringSlice("Content-Length: 4\r\nConnection: keep-alive\r\n")));
    usize full_length = serialized.bytes.len;
    serialized = HttpResponseSerialize(message, 1, 0, wire);
    assert(serialized.tag == HTTP_RESPONSE_SERIALIZE_OK);
    assert(Contains(serialized.bytes, StringSlice("Content-Length: 4\r\nConnection: close\r\n\r\n")));
    /* Connection: close is five bytes shorter, and HEAD omits four body bytes. */
    assert(serialized.bytes.len + 9 == full_length);
    assert(HttpResponseSerialize(message, 0, 1, (byte_slice){wire.ptr, 1}).tag == HTTP_RESPONSE_SERIALIZE_INVALID);
}
static void TestValidationAndLimits(void)
{
    u8 storage[4096], body_storage[HTTP_RESPONSE_LIMIT + 1] = {0};
    bump_allocator arena;
    assert(BumpInit(&arena, (byte_slice){storage, sizeof(storage)}).tag == BUMP_INIT_OK);
    http_response_header_slice none = {0};
    const_byte_slice empty = {0};
    const http_response_header invalid_headers[] = {
        {StringSlice("Content-Length"), StringSlice("0")},
        {StringSlice("tRaNsFeR-EnCoDiNg"), StringSlice("chunked")},
        {StringSlice("Connection"), StringSlice("close")},
        {StringSlice("Trailer"), StringSlice("X-Foo")},
        {StringSlice("Upgrade"), StringSlice("websocket")},
        {StringSlice("Keep-Alive"), StringSlice("timeout=1")},
        {StringSlice("Proxy-Connection"), StringSlice("close")},
        {StringSlice("Bad Name"), StringSlice("value")},
        {StringSlice(""), StringSlice("value")},
        {StringSlice("X-Test"), StringSlice("value\r\nInjected: true")},
        {StringSlice("X-Test"), StringSlice("a\0b")},
        {{NULL, 1}, StringSlice("")},
        {StringSlice("X-Test"), {NULL, 1}},
    };
    http_response_header_slice invalid = {invalid_headers, sizeof(invalid_headers) / sizeof(invalid_headers[0])};
    SliceFor(invalid, i) {
        http_response_build_result result = HttpResponseBuild(&arena, 200, (http_response_header_slice){&invalid.ptr[i], 1}, empty);
        assert(result.tag == HTTP_RESPONSE_BUILD_INVALID_ARGUMENT && arena.used == 0);
    }
    assert(HttpResponseBuild(NULL, 200, none, empty).tag == HTTP_RESPONSE_BUILD_INVALID_ARGUMENT);
    assert(HttpResponseBuild(&arena, 200, (http_response_header_slice){NULL, 1}, empty).tag == HTTP_RESPONSE_BUILD_INVALID_ARGUMENT);
    assert(HttpResponseBuild(&arena, 200, none, (const_byte_slice){NULL, 1}).tag == HTTP_RESPONSE_BUILD_INVALID_ARGUMENT);
    assert(HttpResponseBuild(&arena, 199, none, empty).tag == HTTP_RESPONSE_BUILD_INVALID_ARGUMENT);
    assert(HttpResponseBuild(&arena, 600, none, empty).tag == HTTP_RESPONSE_BUILD_INVALID_ARGUMENT);
    const u32 statuses[] = {200, 204, 205, 304, 599};
    typedef struct status_slice { const u32 *ptr; usize len; } status_slice;
    status_slice codes = {statuses, sizeof(statuses) / sizeof(statuses[0])};
    SliceFor(codes, i) {
        BumpReset(&arena);
        u32 status = codes.ptr[i];
        http_response_build_result result = HttpResponseBuild(&arena, status, none, empty);
        assert(result.tag == HTTP_RESPONSE_BUILD_OK && result.message.len == sizeof(http_response));
        assert(HttpResponseValidate(result.message).tag == HTTP_RESPONSE_VALIDATE_OK);
        http_response root = Root(result.message);
        assert(root.Body.index == 0 && root.Body.len == 0 && root.HeaderCount == 0 && root.FirstHeaderOffset == 0);
        if (status == 204 || status == 205 || status == 304) {
            usize used = arena.used;
            assert(HttpResponseBuild(&arena, status, none, (const_byte_slice){body_storage, 1}).tag == HTTP_RESPONSE_BUILD_INVALID_ARGUMENT);
            assert(arena.used == used);
        }
    }
    BumpReset(&arena);
    http_response_build_result full = HttpResponseBuild(&arena, 200, none,
        (const_byte_slice){body_storage, HTTP_RESPONSE_LIMIT - sizeof(http_response)});
    assert(full.tag == HTTP_RESPONSE_BUILD_OK && full.message.len == HTTP_RESPONSE_LIMIT);
    assert(HttpResponseValidate(full.message).tag == HTTP_RESPONSE_VALIDATE_OK);
    BumpReset(&arena);
    assert(HttpResponseBuild(&arena, 200, none, (const_byte_slice){body_storage, HTTP_RESPONSE_LIMIT - sizeof(http_response) + 1}).tag == HTTP_RESPONSE_BUILD_LIMIT);
    assert(arena.used == 0);
    http_response_header many[HTTP_RESPONSE_HEADER_LIMIT + 1];
    http_response_header_slice all = {many, sizeof(many) / sizeof(many[0])};
    SliceFor(all, i) { many[i] = (http_response_header){StringSlice("X"), StringSlice("")}; }
    assert(HttpResponseBuild(&arena, 200, all, empty).tag == HTTP_RESPONSE_BUILD_LIMIT);
    assert(arena.used == 0);
    --all.len;
    full = HttpResponseBuild(&arena, 200, all, empty);
    assert(full.tag == HTTP_RESPONSE_BUILD_OK && HttpResponseValidate(full.message).tag == HTTP_RESPONSE_VALIDATE_OK);
    u8 tiny_storage[sizeof(http_response) - 1];
    byte_slice tiny = {tiny_storage, sizeof(tiny_storage)};
    SliceFor(tiny, i) { tiny.ptr[i] = 0xa5; }
    bump_allocator limited;
    assert(BumpInit(&limited, tiny).tag == BUMP_INIT_OK);
    assert(HttpResponseBuild(&limited, 200, none, empty).tag == HTTP_RESPONSE_BUILD_NO_SPACE && limited.used == 0);
    SliceFor(tiny, i) { assert(tiny.ptr[i] == 0xa5); }
}
int main(void)
{
    TestCopyAndSerialize();
    TestValidationAndLimits();
    puts("PASS: response builder ownership, relocation, headers, binary body, HEAD, validation and limits");
}
