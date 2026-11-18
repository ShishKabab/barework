#ifndef SECURE_ROUTE_H
#define SECURE_ROUTE_H

#include "http.h"
#include "slice.h"
#include "connection_protocol.h"

/* Deployment-local little-endian ABI. HTTP offsets remain relative to the
 * embedded http_request; capture ranges are relative to the dispatch root.
 * The generated payload follows this root, then the unchanged HTTP bytes. */
typedef struct route_request {
    byte_range payload;
    byte_range http;
} route_request;

_Static_assert(sizeof(route_request) == 16 && sizeof(byte_range) == 8, "route ABI");

typedef enum route_match_tag {
    ROUTE_MATCH_OK,
    ROUTE_MATCH_NOT_FOUND,
    ROUTE_MATCH_INVALID_REQUEST,
    ROUTE_MATCH_NO_SPACE,
} route_match_tag;

typedef struct route_match_result {
    route_match_tag tag;
    const_byte_slice message;
} route_match_result;

typedef enum route_request_read_tag {
    ROUTE_REQUEST_READ_OK,
    ROUTE_REQUEST_READ_INVALID,
} route_request_read_tag;

typedef struct route_request_read_result {
    route_request_read_tag tag;
    const_byte_slice http;
} route_request_read_result;

/* Read the origin-form target and exclude its query. capture_base is the
 * root-relative position of path byte zero in the eventual dispatch message.
 * Check sizes before narrowing indices to the fixed-width protocol fields. */
typedef enum route_path_read_tag {
    ROUTE_PATH_READ_OK,
    ROUTE_PATH_READ_INVALID,
} route_path_read_tag;

typedef struct route_path_read_result {
    route_path_read_tag tag;
    const_byte_slice path;
    u32 capture_base;
} route_path_read_result;

static inline route_path_read_result RoutePathRead(const_byte_slice http, usize payload_size)
{
    route_path_read_result result = {.tag = ROUTE_PATH_READ_INVALID};
    http_request request;
    if (http.len > CONNECTION_MESSAGE_LIMIT ||
        payload_size > CONNECTION_DISPATCH_LIMIT - sizeof(route_request) ||
        http.len > CONNECTION_DISPATCH_LIMIT - sizeof(route_request) - payload_size) {
        return result;
    }
    if (ByteSliceCopyRange(MutableBytesOf(request), 0, http, 0, sizeof(request)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        request.MessageLength != http.len || request.TargetOffset < sizeof(request)) {
        return result;
    }

    byte_range target_range = {request.TargetOffset, request.TargetLength};
    const_byte_range_resolve_result target = ConstByteRangeResolve(http, target_range);
    if (target.tag != CONST_BYTE_RANGE_RESOLVE_OK || !target.bytes.len || target.bytes.ptr[0] != '/') {
        return result;
    }
    const_byte_slice path = target.bytes;
    SliceFor(path, index) {
        if (path.ptr[index] == '?') {
            path.len = index;
            break;
        }
    }

    result.tag = ROUTE_PATH_READ_OK;
    result.path = path;
    result.capture_base = (u32)(sizeof(route_request) + payload_size + request.TargetOffset);
    return result;
}

/* Copy an unaligned envelope and its payload, validating layout first. The
 * caller subsequently validates the generated tag and route-specific captures.
 * payload may be modified on failure and must be inspected only on success. */
typedef enum route_envelope_read_tag {
    ROUTE_ENVELOPE_READ_OK,
    ROUTE_ENVELOPE_READ_INVALID,
} route_envelope_read_tag;

typedef struct route_envelope_read_result {
    route_envelope_read_tag tag;
    const_byte_slice http;
    byte_range path;
} route_envelope_read_result;

static inline route_envelope_read_result RouteEnvelopeRead(const_byte_slice message, byte_slice payload)
{
    route_envelope_read_result result = {.tag = ROUTE_ENVELOPE_READ_INVALID};
    route_request root;
    if (!payload.ptr || !payload.len || message.len > CONNECTION_DISPATCH_LIMIT ||
        ByteSliceCopyRange(MutableBytesOf(root), 0, message, 0, sizeof(root)).tag != BYTE_SLICE_COPY_RANGE_OK) {
        return result;
    }
    if (root.payload.index != sizeof(root) || root.payload.len != payload.len ||
        payload.len > message.len - sizeof(root) ||
        root.http.index != sizeof(root) + payload.len || root.http.index > message.len ||
        root.http.len != message.len - root.http.index) {
        return result;
    }

    const_byte_range_resolve_result http = ConstByteRangeResolve(message, root.http);
    if (http.tag != CONST_BYTE_RANGE_RESOLVE_OK) {
        return result;
    }
    route_path_read_result path = RoutePathRead(http.bytes, payload.len);
    if (path.tag != ROUTE_PATH_READ_OK ||
        ByteSliceCopyRange(payload, 0, message, root.payload.index, root.payload.len).tag != BYTE_SLICE_COPY_RANGE_OK) {
        return result;
    }

    result.tag = ROUTE_ENVELOPE_READ_OK;
    result.http = http.bytes;
    result.path = (byte_range){path.capture_base, (u32)path.path.len};
    return result;
}

static inline bool RouteCaptureValid(const_byte_slice message, byte_range path, byte_range capture)
{
    if (!capture.len || capture.index < path.index ||
        capture.index - path.index > path.len || capture.len > path.len - (capture.index - path.index)) {
        return false;
    }
    const_byte_range_resolve_result field = ConstByteRangeResolve(message, capture);
    if (field.tag != CONST_BYTE_RANGE_RESOLVE_OK) {
        return false;
    }
    SliceFor(field.bytes, index) {
        if (field.bytes.ptr[index] == '/') {
            return false;
        }
    }
    return true;
}

/* Input ranges must describe live storage and survive allocation. Captures in
 * payload already refer to their final positions. Allocate only after matching. */
typedef enum route_envelope_build_tag {
    ROUTE_ENVELOPE_BUILD_OK,
    ROUTE_ENVELOPE_BUILD_INVALID,
    ROUTE_ENVELOPE_BUILD_NO_SPACE,
} route_envelope_build_tag;

typedef struct route_envelope_build_result {
    route_envelope_build_tag tag;
    const_byte_slice message;
} route_envelope_build_result;

static inline route_envelope_build_result RouteEnvelopeBuild(
    const_byte_slice http, const_byte_slice payload, bump_allocator *arena)
{
    route_envelope_build_result result = {.tag = ROUTE_ENVELOPE_BUILD_INVALID};
    if (!arena || !payload.ptr || !payload.len || (!http.ptr && http.len) ||
        http.len > CONNECTION_MESSAGE_LIMIT ||
        payload.len > CONNECTION_DISPATCH_LIMIT - sizeof(route_request) ||
        http.len > CONNECTION_DISPATCH_LIMIT - sizeof(route_request) - payload.len) {
        return result;
    }

    usize prefix = sizeof(route_request) + payload.len;
    bump_allocate_result allocation = BumpAllocate(arena, prefix + http.len, _Alignof(route_request));
    if (allocation.tag != BUMP_ALLOCATE_OK) {
        result.tag = ROUTE_ENVELOPE_BUILD_NO_SPACE;
        return result;
    }
    route_request root = {
        .payload = {sizeof(root), (u32)payload.len},
        .http = {(u32)prefix, (u32)http.len},
    };
    if (ByteSliceCopy(allocation.allocation, 0, BytesOf(root)).tag != BYTE_SLICE_COPY_OK ||
        ByteSliceCopy(allocation.allocation, root.payload.index, payload).tag != BYTE_SLICE_COPY_OK ||
        ByteSliceCopy(allocation.allocation, root.http.index, http).tag != BYTE_SLICE_COPY_OK) {
        return result;
    }
    result.tag = ROUTE_ENVELOPE_BUILD_OK;
    result.message = (const_byte_slice){allocation.allocation.ptr, allocation.allocation.len};
    return result;
}

/* Helpers used by emitted branching code, not a route interpreter. */
typedef enum route_segment_tag {
    ROUTE_SEGMENT_OK,
    ROUTE_SEGMENT_MISSING,
} route_segment_tag;

typedef struct route_segment_result {
    route_segment_tag tag;
    const_byte_slice bytes;
    byte_range range;
    usize next;
} route_segment_result;

static inline route_segment_result RouteSegment(const_byte_slice path, usize cursor)
{
    route_segment_result result = {.tag = ROUTE_SEGMENT_MISSING};
    if (!path.ptr || cursor >= path.len || path.len > UINT32_MAX) {
        return result;
    }
    usize end = cursor;
    while (end < path.len && path.ptr[end] != '/') {
        ++end;
    }
    const_byte_slice bytes;
    if (end == cursor || ConstByteSliceSubslice(path, cursor, end - cursor, &bytes).tag != CONST_BYTE_SLICE_SUBSLICE_OK) {
        return result;
    }
    result.tag = ROUTE_SEGMENT_OK;
    result.bytes = bytes;
    result.range = (byte_range){(u32)cursor, (u32)(end - cursor)};
    result.next = end < path.len ? end + 1 : end;
    return result;
}

static inline bool RouteLiteralEqual(const_byte_slice bytes, string_slice literal)
{
    if (bytes.len != literal.len) {
        return false;
    }
    SliceFor(bytes, index) {
        if (bytes.ptr[index] != (u8)literal.ptr[index]) {
            return false;
        }
    }
    return true;
}

#endif
