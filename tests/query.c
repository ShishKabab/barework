#include "query.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct scalar_row { query_number number; } scalar_row;
int main(void)
{
    const query_field fields[] = {{.name = StringSlice("number"), .kind = QUERY_NUMBER, .offset = offsetof(scalar_row, number)}};
    const query_shape shape = {.row_size = sizeof(scalar_row), .fields = {fields, 1}};
    u8 storage[2048], json_storage[2048];
    byte_slice storage_bytes = {storage, sizeof(storage)}, json_bytes = {json_storage, sizeof(json_storage)};
    query_arguments arguments = {.operation = 7, .sequence = 2, .id = 1};
    query_fake_result fake = QueryFake(arguments, &shape, storage_bytes);
    assert(fake.tag == QUERY_FAKE_OK);
    assert(QueryValidate(fake.message, 7, 1, &shape).tag == QUERY_VALIDATE_INVALID);
    assert(QueryValidate(fake.message, 8, 2, &shape).tag == QUERY_VALIDATE_INVALID);
    f64 values[] = {0, -0.0, 0.5, 1.5, 0.125, -10, 1024, 0.1, DBL_MIN, DBL_MAX, 0x1p-1074};
    typedef struct numbers { f64 *ptr; usize len; } numbers;
    numbers cases = {values, sizeof(values) / sizeof(values[0])};
    SliceFor(cases, i) {
        scalar_row row = {.number = {.present = 1, .value = cases.ptr[i]}};
        assert(ByteSliceCopy(storage_bytes, sizeof(query_result), BytesOf(row)).tag == BYTE_SLICE_COPY_OK);
        query_json_result json = QueryJson(fake.message, &shape, json_bytes);
        assert(json.tag == QUERY_JSON_OK);
        string_slice prefix = StringSlice("[{\"number\":");
        assert(json.bytes.len > prefix.len + 2);
        SliceFor(prefix, c) { assert(json.bytes.ptr[c] == (u8)prefix.ptr[c]); }
        /* strtod's C-string representation is confined to this libc boundary. */
        char terminated[2048];
        usize length = json.bytes.len - prefix.len - 2;
        assert(ByteSliceCopyRange((byte_slice){(u8 *)terminated, sizeof(terminated)}, 0,
            json.bytes, prefix.len, length).tag == BYTE_SLICE_COPY_RANGE_OK);
        terminated[length] = 0;
        char *end;
        f64 parsed = strtod(terminated, &end);
        assert(end == terminated + length && parsed == cases.ptr[i]);
        assert(!cases.ptr[i] ? !!signbit(parsed) == !!signbit(cases.ptr[i]) : true);
    }
    scalar_row invalid = {.number = {.present = 1, .value = INFINITY}};
    assert(ByteSliceCopy(storage_bytes, sizeof(query_result), BytesOf(invalid)).tag == BYTE_SLICE_COPY_OK);
    assert(QueryValidate(fake.message, 7, 2, &shape).tag == QUERY_VALIDATE_INVALID);
    fake = QueryFake(arguments, &shape, storage_bytes);
    assert(fake.tag == QUERY_FAKE_OK);
    assert(QueryJson(fake.message, &shape, (byte_slice){json_storage, 1}).tag == QUERY_JSON_NO_SPACE);
    query_result malformed = {.operation = 7, .sequence = 2, .rows = {.bytes = {UINT32_MAX, sizeof(scalar_row)}, .count = 1}};
    assert(ByteSliceCopy(storage_bytes, 0, BytesOf(malformed)).tag == BYTE_SLICE_COPY_OK);
    assert(QueryValidate(fake.message, 7, 2, &shape).tag == QUERY_VALIDATE_INVALID);
    assert(QueryFake(arguments, &shape, (byte_slice){storage, 23}).tag == QUERY_FAKE_NO_SPACE);
    string_slice maximum = StringSlice("9223372036854775807");
    assert(QueryIdParse((const_byte_slice){(const u8 *)maximum.ptr, maximum.len}).tag == QUERY_ID_PARSE_OK);
    string_slice overflow = StringSlice("9223372036854775808");
    assert(QueryIdParse((const_byte_slice){(const u8 *)overflow.ptr, overflow.len}).tag == QUERY_ID_PARSE_INVALID);
    puts("PASS: query validation, numeric JSON round trips, malformed ranges, bounded outputs, ID overflow");
}
