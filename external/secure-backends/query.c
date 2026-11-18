#include "query.h"
#include <math.h>

query_id_parse_result QueryIdParse(const_byte_slice text)
{
    query_id_parse_result result = {.tag = QUERY_ID_PARSE_INVALID};
    if (!text.ptr || !text.len) return result;
    u64 value = 0;
    SliceFor(text, i) {
        u8 c = text.ptr[i];
        if (c < '0' || c > '9' || value > (UINT64_C(9223372036854775807) - (c - '0')) / 10) return result;
        value = value * 10 + c - '0';
    }
    return (query_id_parse_result){QUERY_ID_PARSE_OK, value};
}

typedef enum read_tag { READ_OK, READ_INVALID } read_tag;
typedef struct read_result { read_tag tag; } read_result;
static read_result Read(const_byte_slice message, usize offset, byte_slice target)
{
    return (read_result){.tag = ByteSliceCopyRange(target, 0, message, offset, target.len).tag == BYTE_SLICE_COPY_RANGE_OK ? READ_OK : READ_INVALID};
}
static bool RecordsValid(const_byte_slice message, query_records records, const query_shape *shape, usize depth, usize *budget)
{
    if (!shape || depth > QUERY_DEPTH_LIMIT || !shape->row_size || records.count > *budget ||
        records.count > QUERY_MESSAGE_LIMIT / shape->row_size || records.bytes.len != records.count * shape->row_size ||
        (!records.count && records.bytes.index != 0) || (records.count && records.bytes.index < sizeof(query_result))) return false;
    const_byte_range_resolve_result rows = ConstByteRangeResolve(message, records.bytes);
    if (rows.tag != CONST_BYTE_RANGE_RESOLVE_OK) return false;
    *budget -= records.count;
    /* Walk rows by byte subslices; bounds reflect exactly the copied row array. */
    const_byte_slice remaining = rows.bytes;
    while (remaining.len) {
        const_byte_slice row;
        if (ConstByteSliceSubslice(remaining, 0, shape->row_size, &row).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return false;
        SliceFor(shape->fields, i) {
            query_field field = shape->fields.ptr[i];
            u32 present;
            if (field.kind == QUERY_RELATION) {
                query_records child;
                if (Read(row, field.offset, MutableBytesOf(child)).tag != READ_OK || (!field.many && child.count > 1) ||
                    (!field.many && !field.optional && child.count != 1) ||
                    !RecordsValid(message, child, field.nested, depth + 1, budget)) return false;
                continue;
            }
            if (Read(row, field.offset, MutableBytesOf(present)).tag != READ_OK || present > 1 || (!field.optional && !present)) return false;
            switch (field.kind) {
            case QUERY_ID: {
                query_id value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK || value.reserved || value.value > INT64_MAX || (!present && value.value)) return false;
                break;
            }
            case QUERY_TEXT: {
                query_text value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK || (!present && (value.value.index || value.value.len)) ||
                    (value.value.len && value.value.index < sizeof(query_result))) return false;
                if (ConstByteRangeResolve(message, value.value).tag != CONST_BYTE_RANGE_RESOLVE_OK) return false;
                break;
            }
            case QUERY_BOOL: {
                query_bool value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK || value.value > 1 || (!present && value.value)) return false;
                break;
            }
            case QUERY_NUMBER: {
                query_number value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK || value.reserved || !isfinite(value.value) || (!present && value.value != 0)) return false;
                break;
            }
            case QUERY_TIMESTAMP: {
                query_timestamp value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK || value.reserved || (!present && value.value)) return false;
                break;
            }
            case QUERY_RELATION: return false;
            }
        }
        if (ConstByteSliceSubslice(remaining, shape->row_size, remaining.len - shape->row_size, &remaining).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return false;
    }
    return true;
}
query_validate_result QueryValidate(const_byte_slice message, u32 operation, u32 sequence, const query_shape *shape)
{
    query_result root;
    usize budget = QUERY_ROW_LIMIT;
    if (!operation || !sequence || message.len > QUERY_MESSAGE_LIMIT || Read(message, 0, MutableBytesOf(root)).tag != READ_OK ||
        root.operation != operation || root.sequence != sequence || root.outcome > QUERY_INVALID_PARAMETER ||
        (root.outcome != QUERY_SUCCESS && (message.len != sizeof(root) || root.rows.count || root.rows.bytes.index || root.rows.bytes.len)) ||
        !RecordsValid(message, root.rows, shape, 0, &budget)) return (query_validate_result){.tag = QUERY_VALIDATE_INVALID};
    return (query_validate_result){QUERY_VALIDATE_OK, root};
}

/* JSON output is built with checked copies. Input is validated before walking. */
typedef struct json_writer { byte_slice output; usize used; bool full; } json_writer;
static void Write(json_writer *writer, const_byte_slice bytes)
{
    if (writer->full) return;
    if (ByteSliceCopy(writer->output, writer->used, bytes).tag != BYTE_SLICE_COPY_OK) writer->full = true;
    else writer->used += bytes.len;
}
static void Text(json_writer *writer, string_slice text)
{ Write(writer, (const_byte_slice){(const u8 *)text.ptr, text.len}); }
static void Quote(json_writer *writer, const_byte_slice text)
{
    Text(writer, StringSlice("\""));
    SliceFor(text, i) {
        u8 c = text.ptr[i];
        if (c == '"' || c == '\\') { Text(writer, StringSlice("\\")); Write(writer, BytesOf(c)); }
        else if (c < 32 || c >= 127) {
            /* Synthetic text is ASCII. Encode arbitrary bytes without emitting
             * invalid UTF-8; real DB UTF-8 decoding is a later adapter concern. */
            const char hex[] = "0123456789abcdef";
            char escaped[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            Write(writer, BytesOf(escaped));
        } else Write(writer, BytesOf(c));
    }
    Text(writer, StringSlice("\""));
}
static void Unsigned(json_writer *writer, u64 value)
{
    u8 digits[20];
    usize count = 0;
    do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
    while (count) Write(writer, BytesOf(digits[--count]));
}

/* Exact finite IEEE-754 decimal conversion. Base-ten digits avoid libc's
 * stdio/WASI imports and rounding guesses. Binary fractions can need up to
 * 1074 decimal places; the checked writer still bounds the final response. */
static void Number(json_writer *writer, f64 value)
{
    u64 bits;
    if (ByteSliceCopy(MutableBytesOf(bits), 0, BytesOf(value)).tag != BYTE_SLICE_COPY_OK) { writer->full = true; return; }
    if (bits >> 63) Text(writer, StringSlice("-"));
    i32 exponent = (i32)((bits >> 52) & 2047);
    u64 mantissa = bits & UINT64_C(0xfffffffffffff);
    if (exponent) mantissa |= UINT64_C(0x10000000000000);
    exponent = exponent ? exponent - 1075 : -1074;
    if (!mantissa) { Text(writer, StringSlice("0")); return; }
    while (exponent < 0 && !(mantissa & 1)) { mantissa >>= 1; ++exponent; }
    u8 digits[1100];
    usize count = 0;
    do { digits[count++] = mantissa % 10; mantissa /= 10; } while (mantissa);
    i32 scale = exponent < 0 ? -exponent : 0;
    i32 times = exponent < 0 ? -exponent : exponent;
    u32 multiplier = exponent < 0 ? 5 : 2;
    while (times--) {
        u32 carry = 0;
        byte_slice active = {digits, count};
        SliceFor(active, i) {
            u32 product = active.ptr[i] * multiplier + carry;
            active.ptr[i] = product % 10;
            carry = product / 10;
        }
        if (carry) {
            if (count == sizeof(digits)) { writer->full = true; return; }
            digits[count++] = carry;
        }
    }
    bool fraction_prefix = scale >= (i32)count;
    if (fraction_prefix) {
        Text(writer, StringSlice("0."));
        for (i32 zeros = scale - (i32)count; zeros > 0; --zeros) Text(writer, StringSlice("0"));
    }
    while (count) {
        if (!fraction_prefix && scale && count == (usize)scale) Text(writer, StringSlice("."));
        u8 digit = '0' + digits[--count];
        Write(writer, BytesOf(digit));
    }
}

static void RowsJson(json_writer *writer, const_byte_slice message, query_records records, const query_shape *shape, bool array)
{
    if (array) Text(writer, StringSlice("["));
    if (!array && !records.count) Text(writer, StringSlice("null"));
    const_byte_range_resolve_result resolved = ConstByteRangeResolve(message, records.bytes);
    if (resolved.tag != CONST_BYTE_RANGE_RESOLVE_OK) { writer->full = true; return; }
    const_byte_slice remaining = resolved.bytes;
    usize row_index = 0;
    while (remaining.len) {
        if (row_index++) Text(writer, StringSlice(","));
        Text(writer, StringSlice("{"));
        const_byte_slice row;
        if (ConstByteSliceSubslice(remaining, 0, shape->row_size, &row).tag != CONST_BYTE_SLICE_SUBSLICE_OK) { writer->full = true; return; }
        SliceFor(shape->fields, i) {
            query_field field = shape->fields.ptr[i];
            if (i) Text(writer, StringSlice(","));
            Quote(writer, (const_byte_slice){(const u8 *)field.name.ptr, field.name.len});
            Text(writer, StringSlice(":"));
            if (field.kind == QUERY_RELATION) {
                query_records child;
                if (Read(row, field.offset, MutableBytesOf(child)).tag != READ_OK) { writer->full = true; return; }
                RowsJson(writer, message, child, field.nested, field.many);
                continue;
            }
            u32 present;
            if (Read(row, field.offset, MutableBytesOf(present)).tag != READ_OK) { writer->full = true; return; }
            if (!present) { Text(writer, StringSlice("null")); continue; }
            switch (field.kind) {
            case QUERY_TEXT: {
                query_text value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK) { writer->full = true; return; }
                const_byte_range_resolve_result text = ConstByteRangeResolve(message, value.value);
                if (text.tag != CONST_BYTE_RANGE_RESOLVE_OK) { writer->full = true; return; }
                Quote(writer, text.bytes);
                break;
            }
            case QUERY_ID: {
                query_id value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK) { writer->full = true; return; }
                Unsigned(writer, value.value);
                break;
            }
            case QUERY_BOOL: {
                query_bool value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK) { writer->full = true; return; }
                Text(writer, value.value ? StringSlice("true") : StringSlice("false"));
                break;
            }
            case QUERY_NUMBER: {
                query_number value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK) { writer->full = true; return; }
                Number(writer, value.value);
                break;
            }
            case QUERY_TIMESTAMP: {
                query_timestamp value;
                if (Read(row, field.offset, MutableBytesOf(value)).tag != READ_OK) { writer->full = true; return; }
                if (value.value < 0) Text(writer, StringSlice("-"));
                Unsigned(writer, value.value < 0 ? UINT64_C(0) - (u64)value.value : (u64)value.value);
                break;
            }
            case QUERY_RELATION: break;
            }
        }
        Text(writer, StringSlice("}"));
        if (ConstByteSliceSubslice(remaining, shape->row_size, remaining.len - shape->row_size, &remaining).tag != CONST_BYTE_SLICE_SUBSLICE_OK) { writer->full = true; return; }
    }
    if (array) Text(writer, StringSlice("]"));
}
query_json_result QueryJson(const_byte_slice message, const query_shape *shape, byte_slice output)
{
    query_result root;
    if (Read(message, 0, MutableBytesOf(root)).tag != READ_OK ||
        QueryValidate(message, root.operation, root.sequence, shape).tag != QUERY_VALIDATE_OK || root.outcome != QUERY_SUCCESS)
        return (query_json_result){.tag = QUERY_JSON_INVALID};
    json_writer writer = {.output = output};
    RowsJson(&writer, message, root.rows, shape, true);
    if (writer.full) return (query_json_result){.tag = QUERY_JSON_NO_SPACE};
    return (query_json_result){.tag = QUERY_JSON_OK, .bytes = {output.ptr, writer.used}};
}
