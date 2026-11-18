#include "http_response_message.h"
#include "slice.h"

static bool HeaderName(string_slice key, string_slice expected)
{
    if (key.len != expected.len) return false;
    SliceFor(key, i) {
        u8 c = (u8)key.ptr[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (u8)expected.ptr[i]) return false;
    }
    return true;
}
bool HttpResponseHeaderValid(string_slice key, string_slice value)
{
    if (!key.ptr || !key.len || (!value.ptr && value.len)) return false;
    SliceFor(key, index) {
        u8 c = (u8)key.ptr[index];
        bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        string_slice punctuation = StringSlice("!#$%&'*+-.^_`|~");
        SliceFor(punctuation, symbol) { if (c == (u8)punctuation.ptr[symbol]) valid = true; }
        if (!valid) return false;
    }
    SliceFor(value, index) {
        u8 c = (u8)value.ptr[index];
        if ((c < 32 && c != '\t') || c == 127) return false;
    }
    return !(HeaderName(key, StringSlice("content-length")) || HeaderName(key, StringSlice("transfer-encoding")) ||
        HeaderName(key, StringSlice("connection")) || HeaderName(key, StringSlice("trailer")) ||
        HeaderName(key, StringSlice("upgrade")) || HeaderName(key, StringSlice("keep-alive")) ||
        HeaderName(key, StringSlice("proxy-connection")));
}
static bool Field(const_byte_slice message, u32 offset, u32 length, const_byte_slice *out)
{
    return (length == 0 || offset >= sizeof(http_response)) &&
        ConstByteSliceSubslice(message, offset, length, out).tag == CONST_BYTE_SLICE_SUBSLICE_OK;
}
http_response_validate_result HttpResponseValidate(const_byte_slice message)
{
    http_response root;
    const_byte_slice body;
    if (message.len > HTTP_RESPONSE_LIMIT ||
        ByteSliceCopyRange((byte_slice){(u8 *)&root, sizeof(root)}, 0, message, 0, sizeof(root)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        root.MessageLength != message.len || root.StatusCode < 200 || root.StatusCode > 599 || root.HeaderCount > HTTP_RESPONSE_HEADER_LIMIT ||
        (root.Body.len == 0 && root.Body.index != 0) ||
        ((root.StatusCode == 204 || root.StatusCode == 205 || root.StatusCode == 304) && root.Body.len != 0) ||
        !Field(message, root.Body.index, root.Body.len, &body)) goto invalid;
    u32 offset = root.FirstHeaderOffset;
    for (u32 i = 0; i < root.HeaderCount; ++i) {
        http_header header;
        const_byte_slice key, value;
        if (offset < sizeof(root) ||
            ByteSliceCopyRange((byte_slice){(u8 *)&header, sizeof(header)}, 0, message, offset, sizeof(header)).tag != BYTE_SLICE_COPY_RANGE_OK ||
            !Field(message, header.KeyOffset, header.KeyLength, &key) ||
            !Field(message, header.ValueOffset, header.ValueLength, &value) ||
            (header.NextHeaderOffset && header.NextHeaderOffset < offset + sizeof(header)) ||
            !HttpResponseHeaderValid((string_slice){(const char *)key.ptr, key.len}, (string_slice){(const char *)value.ptr, value.len})) goto invalid;
        offset = header.NextHeaderOffset;
    }
    if (offset != 0) goto invalid;
    return (http_response_validate_result){HTTP_RESPONSE_VALIDATE_OK};
invalid:
    return (http_response_validate_result){HTTP_RESPONSE_VALIDATE_INVALID};
}
