#include "http.h"
#include "slice.h"

enum {
    HTTP_METHOD, HTTP_TARGET, HTTP_VERSION, HTTP_REQUEST_CR, HTTP_REQUEST_LF,
    HTTP_HEADER_START, HTTP_HEADER_KEY, HTTP_HEADER_VALUE, HTTP_HEADER_LF,
    HTTP_END_LF,
};

http_parse_limits HttpDefaultLimits(void)
{
    return (http_parse_limits){.max_bytes = 65536, .max_headers = 128};
}

static void HttpParserBegin(http_parser *parser)
{
    bump_allocate_result allocated = BumpAllocate(&parser->arena, sizeof(http_request), _Alignof(http_request));
    if (allocated.tag != BUMP_ALLOCATE_OK) {
        parser->status = HTTP_PARSE_NO_SPACE;
        return;
    }
    parser->root_start = (usize)(allocated.allocation.ptr - parser->arena.storage.ptr);
    parser->status = HTTP_PARSE_NEED_MORE;
    return;
}

http_parser_init_result HttpParserInit(http_parser *parser, byte_slice storage,
                                http_parse_limits limits)
{
    if (parser == NULL) {
        return (http_parser_init_result){.tag = HTTP_PARSER_INIT_INVALID_ARGUMENT};
    }
    *parser = (http_parser){.limits = limits, .status = HTTP_PARSE_INVALID};
    if (storage.len > UINT32_MAX || limits.max_bytes == 0 ||
        BumpInit(&parser->arena, storage).tag != BUMP_INIT_OK) {
        return (http_parser_init_result){.tag = HTTP_PARSER_INIT_INVALID_ARGUMENT};
    }
    HttpParserBegin(parser);
    return (http_parser_init_result){.tag = parser->status == HTTP_PARSE_NEED_MORE ? HTTP_PARSER_INIT_READY : HTTP_PARSER_INIT_NO_SPACE};
}

http_parser_create_result HttpParserCreate(http_parser *parser, usize arena_limit,
                                  http_parse_limits limits)
{
    if (parser == NULL) {
        return (http_parser_create_result){.tag = HTTP_PARSER_CREATE_INVALID_ARGUMENT};
    }
    *parser = (http_parser){.limits = limits, .status = HTTP_PARSE_INVALID};
    if (arena_limit == 0 || arena_limit > UINT32_MAX || limits.max_bytes == 0) {
        return (http_parser_create_result){.tag = parser->status == HTTP_PARSE_INVALID ? HTTP_PARSER_CREATE_INVALID_ARGUMENT : HTTP_PARSER_CREATE_NO_SPACE};
    }
    if (arena_limit < sizeof(http_request) || BumpCreate(&parser->arena, arena_limit).tag != BUMP_CREATE_OK) {
        parser->status = HTTP_PARSE_NO_SPACE;
        return (http_parser_create_result){.tag = parser->status == HTTP_PARSE_INVALID ? HTTP_PARSER_CREATE_INVALID_ARGUMENT : HTTP_PARSER_CREATE_NO_SPACE};
    }
    HttpParserBegin(parser);
    return (http_parser_create_result){.tag = parser->status == HTTP_PARSE_NEED_MORE ? HTTP_PARSER_CREATE_READY : HTTP_PARSER_CREATE_NO_SPACE};
}

void HttpParserDestroy(http_parser *parser)
{
    if (parser == NULL) {
        return;
    }
    BumpDestroy(&parser->arena);
    *parser = (http_parser){.status = HTTP_PARSE_INVALID};
}

http_parser_reset_result HttpParserReset(http_parser *parser)
{
    if (parser == NULL || parser->arena.storage.ptr == NULL) return (http_parser_reset_result){.tag = HTTP_PARSER_RESET_INVALID_ARGUMENT};
    bump_allocator arena = parser->arena;
    http_parse_limits limits = parser->limits;
    BumpReset(&arena);
    *parser = (http_parser){.arena = arena, .limits = limits};
    HttpParserBegin(parser);
    return (http_parser_reset_result){.tag = parser->status == HTTP_PARSE_NEED_MORE ? HTTP_PARSER_RESET_READY : HTTP_PARSER_RESET_NO_SPACE};
}

static bool HttpIsToken(u8 value)
{
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }
    switch (value) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

typedef enum http_append_tag { HTTP_APPEND_OK, HTTP_APPEND_NO_SPACE } http_append_tag;
typedef struct http_append_result { http_append_tag tag; } http_append_result;
typedef enum http_step_tag { HTTP_STEP_NEED_MORE, HTTP_STEP_COMPLETE, HTTP_STEP_INVALID, HTTP_STEP_NO_SPACE, HTTP_STEP_LIMIT } http_step_tag;
typedef struct http_step_result { http_step_tag tag; } http_step_result;
static http_append_result HttpAppend(http_parser *parser, u8 value, u32 *offset)
{
    bump_allocate_result allocated = BumpAllocate(&parser->arena, 1, 1);
    if (allocated.tag != BUMP_ALLOCATE_OK) {
        return (http_append_result){.tag = HTTP_APPEND_NO_SPACE};
    }
    allocated.allocation.ptr[0] = value;
    if (*offset == 0) {
        *offset = (u32)(parser->arena.used - parser->root_start - 1);
    }
    return (http_append_result){.tag = HTTP_APPEND_OK};
}

static http_step_result HttpStep(http_parser *parser, u8 value)
{
    http_append_result status;
    switch (parser->state) {
    case HTTP_METHOD:
        if (value == ' ' && parser->request.MethodLength != 0) {
            parser->state = HTTP_TARGET;
            break;
        }
        if (!HttpIsToken(value)) {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        status = HttpAppend(parser, value, &parser->request.MethodOffset);
        if (status.tag != HTTP_APPEND_OK) {
            return (http_step_result){.tag = HTTP_STEP_NO_SPACE};
        }
        ++parser->request.MethodLength;
        break;
    case HTTP_TARGET:
        if (value == ' ' && parser->request.TargetLength != 0) {
            parser->state = HTTP_VERSION;
            break;
        }
        if (value <= 0x20 || value >= 0x7f) {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        status = HttpAppend(parser, value, &parser->request.TargetOffset);
        if (status.tag != HTTP_APPEND_OK) {
            return (http_step_result){.tag = HTTP_STEP_NO_SPACE};
        }
        ++parser->request.TargetLength;
        break;
    case HTTP_VERSION: {
        string_slice prefix = StringSlice("HTTP/1.");
        if (parser->version_index < prefix.len) {
            if (value != (u8)prefix.ptr[parser->version_index]) {
                return (http_step_result){.tag = HTTP_STEP_INVALID};
            }
            ++parser->version_index;
        } else {
            if (value != '0' && value != '1') {
                return (http_step_result){.tag = HTTP_STEP_INVALID};
            }
            parser->request.VersionMajor = 1;
            parser->request.VersionMinor = value - '0';
            parser->state = HTTP_REQUEST_CR;
        }
        break;
    }
    case HTTP_REQUEST_CR:
        if (value != '\r') {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        parser->state = HTTP_REQUEST_LF;
        break;
    case HTTP_REQUEST_LF:
        if (value != '\n') {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        parser->state = HTTP_HEADER_START;
        break;
    case HTTP_HEADER_START: {
        if (value == '\r') {
            parser->state = HTTP_END_LF;
            break;
        }
        if (!HttpIsToken(value)) {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        if (parser->request.HeaderCount == parser->limits.max_headers) {
            return (http_step_result){.tag = HTTP_STEP_LIMIT};
        }
        bump_allocate_result allocated = BumpAllocate(&parser->arena, sizeof(http_header), _Alignof(http_header));
        if (allocated.tag != BUMP_ALLOCATE_OK) {
            return (http_step_result){.tag = HTTP_STEP_NO_SPACE};
        }
        parser->header = (http_header){0};
        parser->header_offset = (u32)(parser->arena.used - parser->root_start - allocated.allocation.len);
        parser->state = HTTP_HEADER_KEY;
        /* Process the first key byte using the same path as later bytes. */
        return HttpStep(parser, value);
    }
    case HTTP_HEADER_KEY:
        if (value == ':') {
            parser->state = HTTP_HEADER_VALUE;
            break;
        }
        if (!HttpIsToken(value)) {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        status = HttpAppend(parser, value, &parser->header.KeyOffset);
        if (status.tag != HTTP_APPEND_OK) {
            return (http_step_result){.tag = HTTP_STEP_NO_SPACE};
        }
        ++parser->header.KeyLength;
        break;
    case HTTP_HEADER_VALUE:
        if (value == '\r') {
            parser->state = HTTP_HEADER_LF;
            break;
        }
        if ((value < 0x20 && value != '\t') || value == 0x7f) {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        if (parser->header.ValueOffset == 0 && (value == ' ' || value == '\t')) {
            break;
        }
        status = HttpAppend(parser, value, &parser->header.ValueOffset);
        if (status.tag != HTTP_APPEND_OK) {
            return (http_step_result){.tag = HTTP_STEP_NO_SPACE};
        }
        if (value != ' ' && value != '\t') {
            parser->header.ValueLength = (u32)(parser->arena.used - parser->root_start)
                                        - parser->header.ValueOffset;
        }
        break;
    case HTTP_HEADER_LF: {
        if (value != '\n') {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        byte_slice message;
        byte_slice allocated = {parser->arena.storage.ptr, parser->arena.used};
        if (parser->root_start > allocated.len ||
            ByteSliceSubslice(allocated, parser->root_start, allocated.len - parser->root_start, &message).tag != BYTE_SLICE_SUBSLICE_OK ||
            ByteSliceCopy(message, parser->header_offset, BytesOf(parser->header)).tag != BYTE_SLICE_COPY_OK) {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        if (parser->previous_header_offset == 0) {
            parser->request.FirstHeaderOffset = parser->header_offset;
        } else {
            byte_slice previous;
            if (ByteSliceSubslice(message, parser->previous_header_offset, sizeof(http_header), &previous).tag != BYTE_SLICE_SUBSLICE_OK ||
                ByteSliceCopy(previous, offsetof(http_header, NextHeaderOffset), BytesOf(parser->header_offset)).tag != BYTE_SLICE_COPY_OK) {
                return (http_step_result){.tag = HTTP_STEP_INVALID};
            }
        }
        parser->previous_header_offset = parser->header_offset;
        ++parser->request.HeaderCount;
        parser->state = HTTP_HEADER_START;
        break;
    }
    case HTTP_END_LF: {
        if (value != '\n') {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        parser->request.MessageLength = (u32)(parser->arena.used - parser->root_start);
        byte_slice allocated = {parser->arena.storage.ptr, parser->arena.used};
        if (ByteSliceCopy(allocated, parser->root_start, BytesOf(parser->request)).tag != BYTE_SLICE_COPY_OK) {
            return (http_step_result){.tag = HTTP_STEP_INVALID};
        }
        return (http_step_result){.tag = HTTP_STEP_COMPLETE};
    }
    default:
        return (http_step_result){.tag = HTTP_STEP_INVALID};
    }
    return (http_step_result){.tag = HTTP_STEP_NEED_MORE};
}

http_parse_result HttpParserParse(http_parser *parser, const_byte_slice input)
{
    http_parse_result result = {.status = HTTP_PARSE_INVALID, .remainder = input};
    if (parser == NULL) {
        return result;
    }
    if (parser->status == HTTP_PARSE_NEED_MORE && input.ptr == NULL && input.len != 0) {
        parser->status = HTTP_PARSE_INVALID;
    }
    while (parser->status == HTTP_PARSE_NEED_MORE && result.consumed < input.len) {
        if (parser->wire_bytes == parser->limits.max_bytes) {
            parser->status = HTTP_PARSE_LIMIT;
            break;
        }
        switch (HttpStep(parser, input.ptr[result.consumed]).tag) {
        case HTTP_STEP_NEED_MORE: parser->status = HTTP_PARSE_NEED_MORE; break;
        case HTTP_STEP_COMPLETE: parser->status = HTTP_PARSE_COMPLETE; break;
        case HTTP_STEP_INVALID: parser->status = HTTP_PARSE_INVALID; break;
        case HTTP_STEP_NO_SPACE: parser->status = HTTP_PARSE_NO_SPACE; break;
        case HTTP_STEP_LIMIT: parser->status = HTTP_PARSE_LIMIT; break;
        }
        if (parser->status != HTTP_PARSE_NEED_MORE && parser->status != HTTP_PARSE_COMPLETE) {
            break;
        }
        ++result.consumed;
        ++parser->wire_bytes;
    }
    result.status = parser->status;
    if (result.consumed != 0) {
        result.remainder.ptr += result.consumed;
        result.remainder.len -= result.consumed;
    }
    return result;
}

http_parser_finish_result HttpParserFinish(http_parser *parser)
{
    if (parser == NULL) return (http_parser_finish_result){.tag = HTTP_PARSER_FINISH_INVALID};
    if (parser->status == HTTP_PARSE_NEED_MORE) parser->status = HTTP_PARSE_TRUNCATED;
    switch (parser->status) {
    case HTTP_PARSE_COMPLETE: return (http_parser_finish_result){.tag = HTTP_PARSER_FINISH_COMPLETE};
    case HTTP_PARSE_TRUNCATED: return (http_parser_finish_result){.tag = HTTP_PARSER_FINISH_TRUNCATED};
    case HTTP_PARSE_NO_SPACE: return (http_parser_finish_result){.tag = HTTP_PARSER_FINISH_NO_SPACE};
    case HTTP_PARSE_LIMIT: return (http_parser_finish_result){.tag = HTTP_PARSER_FINISH_LIMIT};
    case HTTP_PARSE_INVALID:
    case HTTP_PARSE_NEED_MORE: return (http_parser_finish_result){.tag = HTTP_PARSER_FINISH_INVALID};
    }
    __builtin_unreachable();
}

const_byte_slice HttpParserMessage(const http_parser *parser)
{
    if (parser == NULL || parser->status != HTTP_PARSE_COMPLETE) {
        return (const_byte_slice){0};
    }
    return (const_byte_slice){parser->arena.storage.ptr + parser->root_start,
                              parser->request.MessageLength};
}
