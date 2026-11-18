#include "http_response.h"
#include "slice.h"
#include "http_response_message.h"

static u8 Lower(u8 value)
{
    return value >= 'A' && value <= 'Z' ? (u8)(value + ('a' - 'A')) : value;
}

static bool Equal(const_byte_slice bytes, string_slice text, i32 ignore_case)
{
    if (bytes.len != text.len) return false;
    SliceFor(bytes, index) {
        u8 left = bytes.ptr[index], right = (u8)text.ptr[index];
        if ((ignore_case ? Lower(left) : left) != (ignore_case ? Lower(right) : right)) return false;
    }
    return true;
}

static string_slice SelectResponse(u32 code, i32 head, i32 keep_alive)
{
#define ResponseBytes(headers, body) (keep_alive ? \
    StringSlice(headers "Connection: keep-alive\r\n\r\n" body) : \
    StringSlice(headers "Connection: close\r\n\r\n" body))
    string_slice response;
    switch (code) {
    case 200:
        response = ResponseBytes("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 14\r\n", "Hello, world!\n");
        break;
    case 404:
        response = ResponseBytes("HTTP/1.1 404 Not Found\r\nContent-Length: 10\r\n", "Not found\n");
        break;
    case 405:
        response = ResponseBytes("HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 19\r\n", "Method not allowed\n");
        break;
    case 417:
        response = ResponseBytes("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 19\r\n", "Expectation failed\n");
        break;
    case 431:
        response = ResponseBytes("HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 18\r\n", "Headers too large\n");
        break;
    case 501:
        response = ResponseBytes("HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n", "Not implemented\n");
        break;
    case 500:
        response = ResponseBytes("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 15\r\n", "Internal error\n");
        break;
    default:
        response = ResponseBytes("HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n", "Bad request\n");
        break;
    }
#undef ResponseBytes
    if (head) {
        SliceFor(response, index) {
            if (response.len - index >= 4 && response.ptr[index] == '\r' &&
                response.ptr[index + 1] == '\n' && response.ptr[index + 2] == '\r' &&
                response.ptr[index + 3] == '\n') {
                response.len = index + 4;
                break;
            }
        }
    }
    return response;
}

typedef enum connection_options_tag { CONNECTION_OPTIONS_OK, CONNECTION_OPTIONS_INVALID } connection_options_tag;
typedef struct connection_options_result { connection_options_tag tag; } connection_options_result;
static connection_options_result ConnectionOptions(const_byte_slice value, i32 *close, i32 *keep_alive)
{
    while (value.len != 0) {
        usize end = value.len;
        SliceFor(value, index) { if (value.ptr[index] == ',') { end = index; break; } }
        const_byte_slice token;
        if (ConstByteSliceSubslice(value, 0, end, &token).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return (connection_options_result){.tag = CONNECTION_OPTIONS_INVALID};
        usize start = 0, length = token.len;
        while (start < length && (token.ptr[start] == ' ' || token.ptr[start] == '\t')) ++start;
        while (length > start && (token.ptr[length - 1] == ' ' || token.ptr[length - 1] == '\t')) --length;
        if (ConstByteSliceSubslice(token, start, length - start, &token).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return (connection_options_result){.tag = CONNECTION_OPTIONS_INVALID};
        SliceFor(token, index) {
            u8 byte = Lower(token.ptr[index]);
            i32 valid = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
            string_slice punctuation = StringSlice("!#$%&'*+-.^_`|~");
            SliceFor(punctuation, symbol) { if (byte == (u8)punctuation.ptr[symbol]) valid = 1; }
            if (!valid) return (connection_options_result){.tag = CONNECTION_OPTIONS_INVALID};
        }
        if (Equal(token, StringSlice("close"), 1)) *close = 1;
        if (Equal(token, StringSlice("keep-alive"), 1)) *keep_alive = 1;
        usize consumed = end + (end < value.len);
        if (ConstByteSliceSubslice(value, consumed, value.len - consumed, &value).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return (connection_options_result){.tag = CONNECTION_OPTIONS_INVALID};
    }
    return (connection_options_result){.tag = CONNECTION_OPTIONS_OK};
}

typedef enum request_status_tag { REQUEST_STATUS_OK, REQUEST_STATUS_BAD_REQUEST, REQUEST_STATUS_NOT_FOUND, REQUEST_STATUS_METHOD_NOT_ALLOWED, REQUEST_STATUS_EXPECTATION_FAILED, REQUEST_STATUS_NOT_IMPLEMENTED } request_status_tag;
typedef struct request_status_result { request_status_tag tag; } request_status_result;
static request_status_result RequestStatus(const_byte_slice message, http_request request, i32 *persistent)
{
    u32 host_count = 0, length_count = 0, header_count = 0;
    u64 content_length = 0;
    i32 transfer_encoding = 0, upgrade = 0, expect = 0, close = 0, keep_alive = 0;
    for (u32 offset = request.FirstHeaderOffset; offset != 0;) {
        http_header header;
        if (++header_count > request.HeaderCount ||
            ByteSliceCopyRange((byte_slice){(u8 *)&header, sizeof(header)}, 0,
                               message, offset, sizeof(header)).tag != BYTE_SLICE_COPY_RANGE_OK) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
        const_byte_slice key, value;
        if (ConstByteSliceSubslice(message, header.KeyOffset, header.KeyLength, &key).tag != CONST_BYTE_SLICE_SUBSLICE_OK ||
            ConstByteSliceSubslice(message, header.ValueOffset, header.ValueLength, &value).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
        if (Equal(key, StringSlice("host"), 1)) {
            if (++host_count > 1 || value.len == 0) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
            SliceFor(value, index) {
                u8 byte = value.ptr[index];
                if (byte <= 0x20 || byte >= 0x7f || byte == '/' || byte == '\\' ||
                    byte == '@' || byte == ',' || byte == '#' || byte == '?') return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
            }
        } else if (Equal(key, StringSlice("content-length"), 1)) {
            if (++length_count > 1 || value.len == 0) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
            SliceFor(value, index) {
                u8 byte = value.ptr[index];
                if (byte < '0' || byte > '9' || content_length > (UINT64_MAX - (byte - '0')) / 10) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
                content_length = content_length * 10 + (byte - '0');
            }
        } else if (Equal(key, StringSlice("transfer-encoding"), 1)) {
            transfer_encoding = 1;
        } else if (Equal(key, StringSlice("upgrade"), 1)) {
            upgrade = 1;
        } else if (Equal(key, StringSlice("expect"), 1)) {
            expect = 1;
        } else if (Equal(key, StringSlice("connection"), 1)) {
            if (ConnectionOptions(value, &close, &keep_alive).tag != CONNECTION_OPTIONS_OK) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
        }
        offset = header.NextHeaderOffset;
    }
    if (header_count != request.HeaderCount ||
        (request.VersionMinor == 1 && host_count != 1) || (length_count && transfer_encoding)) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
    if (expect) return (request_status_result){.tag = REQUEST_STATUS_EXPECTATION_FAILED};
    /* No body decoder yet: reject and close, never reinterpret body bytes as
     * another request. Persistence is safe only for a known zero-length body. */
    if (transfer_encoding || content_length != 0 || upgrade) return (request_status_result){.tag = REQUEST_STATUS_NOT_IMPLEMENTED};
    *persistent = !close && (request.VersionMinor == 1 || keep_alive);
    const_byte_slice method, target;
    if (ConstByteSliceSubslice(message, request.MethodOffset, request.MethodLength, &method).tag != CONST_BYTE_SLICE_SUBSLICE_OK ||
        ConstByteSliceSubslice(message, request.TargetOffset, request.TargetLength, &target).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
    if (target.len == 0 || target.ptr[0] != '/') return (request_status_result){.tag = REQUEST_STATUS_BAD_REQUEST};
    if (!Equal(method, StringSlice("GET"), 0) && !Equal(method, StringSlice("HEAD"), 0))
        return (request_status_result){.tag = REQUEST_STATUS_METHOD_NOT_ALLOWED};
    return (request_status_result){.tag = REQUEST_STATUS_OK};
}

http_wire_response HttpResponse(const_byte_slice message, http_parse_status status)
{
    u32 code = status == HTTP_PARSE_LIMIT || status == HTTP_PARSE_NO_SPACE ? 431 : 400;
    i32 head = 0, keep_alive = 0;
    if (status == HTTP_PARSE_COMPLETE) {
        http_request request;
        const_byte_slice method;
        if (ByteSliceCopyRange((byte_slice){(u8 *)&request, sizeof(request)}, 0,
                               message, 0, sizeof(request)).tag == BYTE_SLICE_COPY_RANGE_OK &&
            ConstByteSliceSubslice(message, request.MethodOffset, request.MethodLength, &method).tag == CONST_BYTE_SLICE_SUBSLICE_OK) {
            /* Translate policy outcomes to HTTP wire status codes here. */
            switch (RequestStatus(message, request, &keep_alive).tag) {
            case REQUEST_STATUS_OK: code = 200; break;
            case REQUEST_STATUS_BAD_REQUEST: code = 400; break;
            case REQUEST_STATUS_NOT_FOUND: code = 404; break;
            case REQUEST_STATUS_METHOD_NOT_ALLOWED: code = 405; break;
            case REQUEST_STATUS_EXPECTATION_FAILED: code = 417; break;
            case REQUEST_STATUS_NOT_IMPLEMENTED: code = 501; break;
            }
            head = Equal(method, StringSlice("HEAD"), 0);
        }
    }
    if (code != 200 && code != 404 && code != 405) keep_alive = 0;
    return (http_wire_response){.bytes = SelectResponse(code, head, keep_alive), .keep_alive = keep_alive, .code = code, .head = head};
}

http_wire_response HttpNotFoundResponse(i32 head, i32 keep_alive)
{
    return (http_wire_response){.bytes = SelectResponse(404, head, keep_alive), .code = 404, .head = head, .keep_alive = keep_alive};
}

http_wire_response HttpFailureResponse(void)
{
    return (http_wire_response){.bytes = SelectResponse(500, 0, 0), .code = 500};
}

typedef enum append_tag { APPEND_OK, APPEND_NO_SPACE } append_tag;
typedef struct append_result { append_tag tag; } append_result;
static append_result Append(byte_slice output, usize *used, string_slice text)
{
    if (ByteSliceCopy(output, *used, (const_byte_slice){(const u8 *)text.ptr, text.len}).tag != BYTE_SLICE_COPY_OK)
        return (append_result){APPEND_NO_SPACE};
    *used += text.len;
    return (append_result){APPEND_OK};
}
typedef enum append_number_tag { APPEND_NUMBER_OK, APPEND_NUMBER_NO_SPACE } append_number_tag;
typedef struct append_number_result { append_number_tag tag; } append_number_result;
static append_number_result AppendNumber(byte_slice output, usize *used, u32 number)
{
    /* A u32 has at most ten decimal digits; reverse into bounded local storage. */
    u8 digits_storage[10];
    byte_slice digits = {digits_storage, sizeof(digits_storage)};
    usize count = 0;
    do {
        if (count == digits.len) return (append_number_result){APPEND_NUMBER_NO_SPACE};
        digits.ptr[count++] = (u8)('0' + number % 10);
        number /= 10;
    } while (number);
    while (count) {
        u8 digit = digits.ptr[--count];
        if (ByteSliceCopy(output, *used, BytesOf(digit)).tag != BYTE_SLICE_COPY_OK)
            return (append_number_result){APPEND_NUMBER_NO_SPACE};
        ++*used;
    }
    return (append_number_result){APPEND_NUMBER_OK};
}

http_response_serialize_result HttpResponseSerialize(const_byte_slice message, i32 head, i32 keep_alive, byte_slice output)
{
    http_response root;
    if (HttpResponseValidate(message).tag != HTTP_RESPONSE_VALIDATE_OK ||
        ByteSliceCopyRange((byte_slice){(u8 *)&root, sizeof(root)}, 0, message, 0, sizeof(root)).tag != BYTE_SLICE_COPY_RANGE_OK) goto invalid;
    usize used = 0;
    string_slice reason = root.StatusCode == 200 ? StringSlice(" OK\r\n") :
        root.StatusCode == 204 ? StringSlice(" No Content\r\n") : StringSlice(" Response\r\n");
    if (Append(output, &used, StringSlice("HTTP/1.1 ")).tag != APPEND_OK ||
        AppendNumber(output, &used, root.StatusCode).tag != APPEND_NUMBER_OK ||
        Append(output, &used, reason).tag != APPEND_OK) goto invalid;
    for (u32 offset = root.FirstHeaderOffset; offset != 0;) {
        http_header header;
        const_byte_slice key, value;
        if (ByteSliceCopyRange((byte_slice){(u8 *)&header, sizeof(header)}, 0, message, offset, sizeof(header)).tag != BYTE_SLICE_COPY_RANGE_OK ||
            ConstByteSliceSubslice(message, header.KeyOffset, header.KeyLength, &key).tag != CONST_BYTE_SLICE_SUBSLICE_OK ||
            ConstByteSliceSubslice(message, header.ValueOffset, header.ValueLength, &value).tag != CONST_BYTE_SLICE_SUBSLICE_OK) goto invalid;
        if (ByteSliceCopy(output, used, key).tag != BYTE_SLICE_COPY_OK) goto invalid;
        used += key.len;
        if (Append(output, &used, StringSlice(": ")).tag != APPEND_OK) goto invalid;
        if (ByteSliceCopy(output, used, value).tag != BYTE_SLICE_COPY_OK) goto invalid;
        used += value.len;
        if (Append(output, &used, StringSlice("\r\n")).tag != APPEND_OK) goto invalid;
        offset = header.NextHeaderOffset;
    }
    /* 204 forbids Content-Length. 304's hypothetical representation length is
     * not part of this ABI, so omit it there too. HEAD retains the GET length. */
    if (root.StatusCode != 204 && root.StatusCode != 304) {
        if (Append(output, &used, StringSlice("Content-Length: ")).tag != APPEND_OK ||
            AppendNumber(output, &used, root.Body.len).tag != APPEND_NUMBER_OK ||
            Append(output, &used, StringSlice("\r\n")).tag != APPEND_OK) goto invalid;
    }
    string_slice ending = keep_alive ? StringSlice("Connection: keep-alive\r\n\r\n") : StringSlice("Connection: close\r\n\r\n");
    if (ByteSliceCopy(output, used, (const_byte_slice){(const u8 *)ending.ptr, ending.len}).tag != BYTE_SLICE_COPY_OK) goto invalid;
    used += ending.len;
    if (!head) {
        if (ByteSliceCopyRange(output, used, message, root.Body.index, root.Body.len).tag != BYTE_SLICE_COPY_RANGE_OK) goto invalid;
        used += root.Body.len;
    }
    return (http_response_serialize_result){.tag = HTTP_RESPONSE_SERIALIZE_OK, .bytes = {output.ptr, used}};
invalid:
    return (http_response_serialize_result){.tag = HTTP_RESPONSE_SERIALIZE_INVALID};
}
