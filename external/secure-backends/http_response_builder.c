#include "http_response_message.h"
#include "slice.h"

http_response_build_result HttpResponseBuild(bump_allocator *arena, u32 status,
    http_response_header_slice headers, const_byte_slice body)
{
    if (!arena || (!headers.ptr && headers.len) || (!body.ptr && body.len) ||
        status < 200 || status > 599 || ((status == 204 || status == 205 || status == 304) && body.len))
        return (http_response_build_result){.tag = HTTP_RESPONSE_BUILD_INVALID_ARGUMENT};
    if (headers.len > HTTP_RESPONSE_HEADER_LIMIT) goto limit;
    usize size = sizeof(http_response) + headers.len * sizeof(http_header);
    if (body.len > HTTP_RESPONSE_LIMIT - size) goto limit;
    size += body.len;
    SliceFor(headers, i) {
        http_response_header header = headers.ptr[i];
        if ((!header.key.ptr && header.key.len) || (!header.value.ptr && header.value.len))
            return (http_response_build_result){.tag = HTTP_RESPONSE_BUILD_INVALID_ARGUMENT};
        if (header.key.len > HTTP_RESPONSE_LIMIT - size) goto limit;
        size += header.key.len;
        if (header.value.len > HTTP_RESPONSE_LIMIT - size) goto limit;
        size += header.value.len;
        if (!HttpResponseHeaderValid(header.key, header.value))
            return (http_response_build_result){.tag = HTTP_RESPONSE_BUILD_INVALID_ARGUMENT};
    }
    bump_allocate_result allocated = BumpAllocate(arena, size, _Alignof(http_response));
    if (allocated.tag != BUMP_ALLOCATE_OK) return (http_response_build_result){.tag = HTTP_RESPONSE_BUILD_NO_SPACE};
    byte_slice output = allocated.allocation;
    usize used = sizeof(http_response) + headers.len * sizeof(http_header);
    http_response root = {.MessageLength = (u32)size, .StatusCode = status,
        .FirstHeaderOffset = headers.len ? sizeof(root) : 0, .HeaderCount = (u32)headers.len};
    SliceFor(headers, i) {
        http_response_header input = headers.ptr[i];
        http_header header = {.KeyOffset = (u32)used, .KeyLength = (u32)input.key.len};
        /* Sizing above proves these operations fit; checked primitives remain
         * the only copy boundary. A failure would be an implementation defect. */
        if (ByteSliceCopy(output, used, (const_byte_slice){(const u8 *)input.key.ptr, input.key.len}).tag != BYTE_SLICE_COPY_OK) __builtin_trap();
        used += input.key.len;
        header.ValueOffset = (u32)used; header.ValueLength = (u32)input.value.len;
        if (ByteSliceCopy(output, used, (const_byte_slice){(const u8 *)input.value.ptr, input.value.len}).tag != BYTE_SLICE_COPY_OK) __builtin_trap();
        used += input.value.len;
        header.NextHeaderOffset = i + 1 < headers.len ? (u32)(sizeof(root) + (i + 1) * sizeof(header)) : 0;
        if (ByteSliceCopy(output, sizeof(root) + i * sizeof(header), BytesOf(header)).tag != BYTE_SLICE_COPY_OK) __builtin_trap();
    }
    root.Body = (byte_range){body.len ? (u32)used : 0, (u32)body.len};
    if (ByteSliceCopy(output, used, body).tag != BYTE_SLICE_COPY_OK ||
        ByteSliceCopy(output, 0, BytesOf(root)).tag != BYTE_SLICE_COPY_OK) __builtin_trap();
    return (http_response_build_result){.tag = HTTP_RESPONSE_BUILD_OK, .message = {output.ptr, output.len}};
limit:
    return (http_response_build_result){.tag = HTTP_RESPONSE_BUILD_LIMIT};
}
