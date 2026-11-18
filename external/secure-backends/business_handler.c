#include "business_handler.h"

business_json_response_result BusinessJsonResponse(bump_allocator *arena, const_byte_slice message, const query_shape *shape)
{
    business_json_response_result failure = {.tag = BUSINESS_JSON_RESPONSE_FAILED};
    query_result root;
    if (ByteSliceCopyRange(MutableBytesOf(root), 0, message, 0, sizeof(root)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        QueryValidate(message, root.operation, root.sequence, shape).tag != QUERY_VALIDATE_OK) return failure;
    u32 status = 200;
    const_byte_slice body;
    if (root.outcome == QUERY_SUCCESS) {
        bump_allocate_result allocated = BumpAllocate(arena, HTTP_RESPONSE_LIMIT, 1);
        if (allocated.tag != BUMP_ALLOCATE_OK) return failure;
        query_json_result json = QueryJson(message, shape, allocated.allocation);
        if (json.tag != QUERY_JSON_OK) return failure;
        body = json.bytes;
    } else {
        status = root.outcome == QUERY_INVALID_PARAMETER ? 400 : 503;
        string_slice error = root.outcome == QUERY_INVALID_PARAMETER ? StringSlice("{\"error\":\"invalid route parameter\"}") : StringSlice("{\"error\":\"query unavailable\"}");
        body = (const_byte_slice){(const u8 *)error.ptr, error.len};
    }
    const http_response_header headers[] = {{StringSlice("Content-Type"), StringSlice("application/json")}};
    http_response_build_result built = HttpResponseBuild(arena, status, (http_response_header_slice){headers, 1}, body);
    if (built.tag != HTTP_RESPONSE_BUILD_OK) return failure;
    return (business_json_response_result){BUSINESS_JSON_RESPONSE_OK, built.message};
}
