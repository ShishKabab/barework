#include "http.h"

#include <assert.h>
#include <string.h>

static void CheckRange(const_byte_slice message, u32 offset, u32 length)
{
    assert(offset <= message.len && length <= message.len - offset);
    assert(length == 0 || offset >= sizeof(http_request));
}

static void CheckMessage(const_byte_slice message)
{
    assert(message.len >= sizeof(http_request));
    http_request request;
    memcpy(&request, message.ptr, sizeof(request));
    assert(request.MessageLength == message.len);
    assert(request.MethodLength != 0 && request.TargetLength != 0);
    assert(request.VersionMajor == 1 && request.VersionMinor <= 1);
    CheckRange(message, request.MethodOffset, request.MethodLength);
    CheckRange(message, request.TargetOffset, request.TargetLength);
    assert(request.HeaderCount <= message.len / sizeof(http_header));
    u32 offset = request.FirstHeaderOffset;
    for (u32 index = 0; index < request.HeaderCount; ++index) {
        CheckRange(message, offset, sizeof(http_header));
        assert(offset % _Alignof(http_header) == 0);
        http_header header;
        memcpy(&header, message.ptr + offset, sizeof(header));
        assert(header.KeyLength != 0);
        CheckRange(message, header.KeyOffset, header.KeyLength);
        CheckRange(message, header.ValueOffset, header.ValueLength);
        assert(header.NextHeaderOffset == 0 || header.NextHeaderOffset > offset);
        offset = header.NextHeaderOffset;
    }
    assert(offset == 0);
}

/* libFuzzer's required entry point; raw input becomes a slice at this boundary. */
int LLVMFuzzerTestOneInput(const u8 *data, usize size)
{
    const_byte_slice input = {data, size};
    _Alignas(http_request) u8 first_storage[4098];
    _Alignas(http_request) u8 second_storage[4098];
    http_parser first;
    http_parser second;
    http_parse_limits limits = {.max_bytes = 2048, .max_headers = 32};
    assert(HttpParserInit(&first, (byte_slice){first_storage + 1, 4096}, limits).tag == HTTP_PARSER_INIT_READY);
    assert(HttpParserInit(&second, (byte_slice){second_storage + 2, 4096}, limits).tag == HTTP_PARSER_INIT_READY);
    http_parse_result whole = HttpParserParse(&first, input);
    assert(whole.consumed <= input.len);
    usize consumed = 0;
    http_parse_status status = HTTP_PARSE_NEED_MORE;
    usize chunk_size = input.len != 0 ? input.ptr[0] % 31 + 1 : 1;
    while (consumed < input.len && status == HTTP_PARSE_NEED_MORE) {
        usize remaining = input.len - consumed;
        usize length = remaining < chunk_size ? remaining : chunk_size;
        http_parse_result result = HttpParserParse(&second,
            (const_byte_slice){input.ptr + consumed, length});
        assert(result.consumed <= length);
        assert(result.remainder.len == length - result.consumed);
        assert(result.status != HTTP_PARSE_NEED_MORE || result.consumed == length);
        consumed += result.consumed;
        status = result.status;
    }
    assert(status == whole.status && consumed == whole.consumed);
    if (status == HTTP_PARSE_COMPLETE) {
        const_byte_slice first_message = HttpParserMessage(&first);
        const_byte_slice second_message = HttpParserMessage(&second);
        CheckMessage(first_message);
        CheckMessage(second_message);
        assert(first_message.len == second_message.len);
        assert(memcmp(first_message.ptr, second_message.ptr, first_message.len) == 0);
    } else {
        assert(HttpParserMessage(&first).len == 0);
        assert(HttpParserMessage(&second).len == 0);
    }
    assert(HttpParserFinish(&first).tag == HttpParserFinish(&second).tag);
    http_parse_result after = HttpParserParse(&first, input);
    assert(after.consumed == 0 && after.status == first.status);
    return 0;
}
