#include "slice.h"

#include <assert.h>

static void TestByteRanges(void)
{
    u8 original[] = {10, 20, 30, 40};
    u8 relocated[sizeof(original)] = {0};
    byte_slice destination = {relocated, sizeof(relocated)};
    assert(ByteSliceCopy(destination, 0, BytesOf(original)).tag == BYTE_SLICE_COPY_OK);
    byte_range capture = {1, 2};
    const_byte_range_resolve_result before = ConstByteRangeResolve(BytesOf(original), capture);
    byte_range_resolve_result after = ByteRangeResolve(destination, capture);
    assert(before.tag == CONST_BYTE_RANGE_RESOLVE_OK);
    assert(after.tag == BYTE_RANGE_RESOLVE_OK);
    assert(before.bytes.ptr == original + 1 && before.bytes.len == 2);
    assert(after.bytes.ptr == relocated + 1 && after.bytes.len == 2);
    assert(memcmp(before.bytes.ptr, after.bytes.ptr, after.bytes.len) == 0);
    after.bytes.ptr[0] = 99;
    assert(relocated[1] == 99 && original[1] == 20);

    const byte_range cases[] = {{0, 0}, {0, 4}, {4, 0}, {3, 1},
        {4, 1}, {5, 0}, {1, UINT32_MAX}, {UINT32_MAX, 1}, {UINT32_MAX, UINT32_MAX}};
    typedef struct range_cases { const byte_range *ptr; usize len; } range_cases;
    range_cases ranges = {cases, sizeof(cases) / sizeof(cases[0])};
    SliceFor(ranges, index) {
        byte_range range = ranges.ptr[index];
        bool valid = range.index <= destination.len && range.len <= destination.len - range.index;
        byte_range_resolve_result mutable = ByteRangeResolve(destination, range);
        const_byte_range_resolve_result immutable = ConstByteRangeResolve(BytesOf(relocated), range);
        if (valid) {
            assert(mutable.tag == BYTE_RANGE_RESOLVE_OK);
            assert(immutable.tag == CONST_BYTE_RANGE_RESOLVE_OK);
            assert(mutable.bytes.ptr == relocated + range.index && mutable.bytes.len == range.len);
            assert(immutable.bytes.ptr == mutable.bytes.ptr && immutable.bytes.len == range.len);
        } else {
            assert(mutable.tag == BYTE_RANGE_RESOLVE_OUT_OF_RANGE);
            assert(immutable.tag == CONST_BYTE_RANGE_RESOLVE_OUT_OF_RANGE);
        }
    }
    byte_range_resolve_result empty = ByteRangeResolve((byte_slice){0}, (byte_range){0});
    const_byte_range_resolve_result const_empty = ConstByteRangeResolve((const_byte_slice){0}, (byte_range){0});
    assert(empty.tag == BYTE_RANGE_RESOLVE_OK);
    assert(empty.bytes.ptr == NULL && empty.bytes.len == 0);
    assert(const_empty.tag == CONST_BYTE_RANGE_RESOLVE_OK);
    assert(const_empty.bytes.ptr == NULL && const_empty.bytes.len == 0);
    assert(ByteRangeResolve((byte_slice){0}, (byte_range){1, 0}).tag == BYTE_RANGE_RESOLVE_OUT_OF_RANGE);
    assert(ConstByteRangeResolve((const_byte_slice){0}, (byte_range){0, 1}).tag == CONST_BYTE_RANGE_RESOLVE_OUT_OF_RANGE);
    assert(ByteRangeResolve((byte_slice){NULL, 1}, (byte_range){0}).tag == BYTE_RANGE_RESOLVE_INVALID_ARGUMENT);
    assert(ConstByteRangeResolve((const_byte_slice){NULL, 1}, (byte_range){0}).tag == CONST_BYTE_RANGE_RESOLVE_INVALID_ARGUMENT);
}

int main(void)
{
    TestByteRanges();
    u8 storage[] = {0, 1, 2, 3, 4, 5, 6, 7};
    byte_slice bytes = {storage, sizeof(storage)};
    const_byte_slice input = {bytes.ptr, bytes.len};
    assert(ByteSliceCopyRange(bytes, 2, input, 0, 6).tag == BYTE_SLICE_COPY_RANGE_OK);
    const u8 right[] = {0, 1, 0, 1, 2, 3, 4, 5};
    assert(memcmp(bytes.ptr, right, sizeof(right)) == 0);
    assert(ByteSliceCopyRange(bytes, 0, input, 2, 6).tag == BYTE_SLICE_COPY_RANGE_OK);
    const u8 left[] = {0, 1, 2, 3, 4, 5, 4, 5};
    assert(memcmp(bytes.ptr, left, sizeof(left)) == 0);

    assert(ByteSliceCopyRange(bytes, 7, input, 0, 2).tag == BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE);
    assert(ByteSliceCopyRange(bytes, 0, input, 7, 2).tag == BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE);
    assert(ByteSliceCopyRange(bytes, SIZE_MAX, input, 0, 1).tag == BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE);
    assert(ByteSliceCopyRange(bytes, 0, input, SIZE_MAX, 1).tag == BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE);
    assert(ByteSliceCopyRange(bytes, 0, input, 1, SIZE_MAX).tag == BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE);
    assert(ByteSliceCopy(bytes, 0, (const_byte_slice){NULL, 1}).tag == BYTE_SLICE_COPY_INVALID_ARGUMENT);
    assert(memcmp(bytes.ptr, left, sizeof(left)) == 0);

    assert(ByteSliceCopy((byte_slice){0}, 0, (const_byte_slice){0}).tag == BYTE_SLICE_COPY_OK);
    assert(ByteSliceCopyRange(bytes, bytes.len, input, input.len, 0).tag == BYTE_SLICE_COPY_RANGE_OK);
    byte_slice part = bytes;
    assert(ByteSliceSubslice(bytes, 7, 2, &part).tag == BYTE_SLICE_SUBSLICE_OUT_OF_RANGE);
    assert(part.ptr == bytes.ptr && part.len == bytes.len);
    assert(ByteSliceSubslice(bytes, 2, 3, &part).tag == BYTE_SLICE_SUBSLICE_OK);
    assert(part.ptr == bytes.ptr + 2 && part.len == 3);
    const_byte_slice view = input;
    assert(ConstByteSliceSubslice(input, SIZE_MAX, 0, &view).tag == CONST_BYTE_SLICE_SUBSLICE_OUT_OF_RANGE);
    assert(view.ptr == input.ptr && view.len == input.len);
    assert(ConstByteSliceSubslice(input, input.len, 0, &view).tag == CONST_BYTE_SLICE_SUBSLICE_OK);
    assert(view.ptr == input.ptr + input.len && view.len == 0);

    u32 value = UINT32_C(0x12345678);
    u32 copy = 0;
    assert(ByteSliceCopy(MutableBytesOf(copy), 0, BytesOf(value)).tag == BYTE_SLICE_COPY_OK);
    assert(copy == value);
    assert(ByteSliceZero((byte_slice){0}).tag == BYTE_SLICE_ZERO_OK);
    assert(ByteSliceZero((byte_slice){NULL, 1}).tag == BYTE_SLICE_ZERO_INVALID_ARGUMENT);
    assert(ByteSliceSubslice(bytes, 2, 3, &part).tag == BYTE_SLICE_SUBSLICE_OK);
    u8 before = bytes.ptr[1], after = bytes.ptr[5];
    assert(ByteSliceZero(part).tag == BYTE_SLICE_ZERO_OK);
    SliceFor(part, index) { assert(part.ptr[index] == 0); }
    assert(bytes.ptr[1] == before && bytes.ptr[5] == after);
    return 0;
}
