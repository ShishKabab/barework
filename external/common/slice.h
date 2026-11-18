#ifndef COMMON_SLICE_H
#define COMMON_SLICE_H
#include "slice_results.h"

#include "types.h"

#include <string.h>

/* View an addressable object's representation without copying it. */
#define BytesOf(object) ((const_byte_slice){(const u8 *)&(object), sizeof(object)})

/* Writable representation of a non-const addressable object. The object must
 * remain live for the returned view; do not use this to discard constness. */
#define MutableBytesOf(object) ((byte_slice){(u8 *)&(object), sizeof(object)})

typedef enum byte_slice_zero_tag { BYTE_SLICE_ZERO_OK, BYTE_SLICE_ZERO_INVALID_ARGUMENT } byte_slice_zero_tag;
typedef struct byte_slice_zero_result { byte_slice_zero_tag tag; } byte_slice_zero_result;
/* The slice must describe live writable storage, never reserved/uncommitted pages. */
static inline byte_slice_zero_result ByteSliceZero(byte_slice bytes)
{
    if (!bytes.ptr && bytes.len) return (byte_slice_zero_result){BYTE_SLICE_ZERO_INVALID_ARGUMENT};
    if (bytes.len) memset(bytes.ptr, 0, bytes.len);
    return (byte_slice_zero_result){BYTE_SLICE_ZERO_OK};
}

/* Caller guarantees each nonempty slice describes live accessible storage.
 * Bounds are checked without offset+length overflow. Errors leave outputs and
 * destination bytes unchanged. Empty slices may have a null pointer. */
static inline byte_slice_subslice_result ByteSliceSubslice(byte_slice source, usize offset, usize length,
                                   byte_slice *result)
{
    if (result == NULL || (source.ptr == NULL && source.len != 0)) {
        return (byte_slice_subslice_result){.tag = BYTE_SLICE_SUBSLICE_INVALID_ARGUMENT};
    }
    if (offset > source.len || length > source.len - offset) {
        return (byte_slice_subslice_result){.tag = BYTE_SLICE_SUBSLICE_OUT_OF_RANGE};
    }
    *result = (byte_slice){offset == 0 ? source.ptr : source.ptr + offset, length};
    return (byte_slice_subslice_result){.tag = BYTE_SLICE_SUBSLICE_OK};
}

static inline const_byte_slice_subslice_result ConstByteSliceSubslice(const_byte_slice source, usize offset,
                                        usize length, const_byte_slice *result)
{
    if (result == NULL || (source.ptr == NULL && source.len != 0)) {
        return (const_byte_slice_subslice_result){.tag = CONST_BYTE_SLICE_SUBSLICE_INVALID_ARGUMENT};
    }
    if (offset > source.len || length > source.len - offset) {
        return (const_byte_slice_subslice_result){.tag = CONST_BYTE_SLICE_SUBSLICE_OUT_OF_RANGE};
    }
    *result = (const_byte_slice){offset == 0 ? source.ptr : source.ptr + offset, length};
    return (const_byte_slice_subslice_result){.tag = CONST_BYTE_SLICE_SUBSLICE_OK};
}

/* Resolve relative bytes against live accessible backing storage. Returned
 * slices borrow that storage and remain valid only for its lifetime. Neither
 * resolver changes storage. Bounds checking is delegated to the slice helpers. */
static inline byte_range_resolve_result ByteRangeResolve(byte_slice backing, byte_range range)
{
    byte_slice bytes;
    switch (ByteSliceSubslice(backing, range.index, range.len, &bytes).tag) {
    case BYTE_SLICE_SUBSLICE_OK:
        return (byte_range_resolve_result){.tag = BYTE_RANGE_RESOLVE_OK, .bytes = bytes};
    case BYTE_SLICE_SUBSLICE_INVALID_ARGUMENT:
        return (byte_range_resolve_result){.tag = BYTE_RANGE_RESOLVE_INVALID_ARGUMENT};
    case BYTE_SLICE_SUBSLICE_OUT_OF_RANGE:
        return (byte_range_resolve_result){.tag = BYTE_RANGE_RESOLVE_OUT_OF_RANGE};
    }
    __builtin_unreachable();
}

static inline const_byte_range_resolve_result ConstByteRangeResolve(const_byte_slice backing, byte_range range)
{
    const_byte_slice bytes;
    switch (ConstByteSliceSubslice(backing, range.index, range.len, &bytes).tag) {
    case CONST_BYTE_SLICE_SUBSLICE_OK:
        return (const_byte_range_resolve_result){.tag = CONST_BYTE_RANGE_RESOLVE_OK, .bytes = bytes};
    case CONST_BYTE_SLICE_SUBSLICE_INVALID_ARGUMENT:
        return (const_byte_range_resolve_result){.tag = CONST_BYTE_RANGE_RESOLVE_INVALID_ARGUMENT};
    case CONST_BYTE_SLICE_SUBSLICE_OUT_OF_RANGE:
        return (const_byte_range_resolve_result){.tag = CONST_BYTE_RANGE_RESOLVE_OUT_OF_RANGE};
    }
    __builtin_unreachable();
}

/* Copies exactly length bytes, allowing overlap. No partial copies on error. */
static inline byte_slice_copy_range_result ByteSliceCopyRange(byte_slice destination, usize destination_offset,
                                    const_byte_slice source, usize source_offset,
                                    usize length)
{
    byte_slice target;
    const_byte_slice input;
    if ((destination.ptr == NULL && destination.len != 0) ||
        (source.ptr == NULL && source.len != 0))
        return (byte_slice_copy_range_result){.tag = BYTE_SLICE_COPY_RANGE_INVALID_ARGUMENT};
    if (destination_offset > destination.len || length > destination.len - destination_offset ||
        source_offset > source.len || length > source.len - source_offset)
        return (byte_slice_copy_range_result){.tag = BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE};
    target = (byte_slice){destination_offset == 0 ? destination.ptr : destination.ptr + destination_offset, length};
    input = (const_byte_slice){source_offset == 0 ? source.ptr : source.ptr + source_offset, length};
    if (length != 0) {
        memmove(target.ptr, input.ptr, length);
    }
    return (byte_slice_copy_range_result){.tag = BYTE_SLICE_COPY_RANGE_OK};
}

/* Copies an entire source slice into destination at the given offset. */
static inline byte_slice_copy_result ByteSliceCopy(byte_slice destination, usize offset, const_byte_slice source)
{
    switch (ByteSliceCopyRange(destination, offset, source, 0, source.len).tag) {
    case BYTE_SLICE_COPY_RANGE_OK: return (byte_slice_copy_result){.tag = BYTE_SLICE_COPY_OK};
    case BYTE_SLICE_COPY_RANGE_INVALID_ARGUMENT: return (byte_slice_copy_result){.tag = BYTE_SLICE_COPY_INVALID_ARGUMENT};
    case BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE: return (byte_slice_copy_result){.tag = BYTE_SLICE_COPY_OUT_OF_RANGE};
    }
    __builtin_unreachable();
}

#endif
