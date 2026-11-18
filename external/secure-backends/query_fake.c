#include "query.h"

typedef struct fake_writer { byte_slice output; usize used, rows; u64 id; bool failed; } fake_writer;
static byte_range Allocate(fake_writer *writer, usize size)
{
    if (writer->failed || size > writer->output.len - writer->used) {
        writer->failed = true;
        return (byte_range){0};
    }
    byte_range range = {(u32)writer->used, (u32)size};
    writer->used += size;
    return range;
}
static void Store(fake_writer *writer, usize offset, const_byte_slice value)
{
    if (writer->failed) return;
    if (ByteSliceCopy(writer->output, offset, value).tag != BYTE_SLICE_COPY_OK) writer->failed = true;
}
static query_records Rows(fake_writer *writer, const query_shape *shape, u32 count, usize depth)
{
    query_records records = {0};
    if (!shape || depth > QUERY_DEPTH_LIMIT || count > QUERY_ROW_LIMIT - writer->rows ||
        !shape->row_size || count > QUERY_MESSAGE_LIMIT / shape->row_size) { writer->failed = true; return records; }
    if (!count) return records;
    writer->rows += count;
    records.bytes = Allocate(writer, count * shape->row_size);
    records.count = count;
    byte_slice rows;
    if (writer->failed || ByteSliceSubslice(writer->output, records.bytes.index, records.bytes.len, &rows).tag != BYTE_SLICE_SUBSLICE_OK) { writer->failed = true; return records; }
    usize row_offset = records.bytes.index;
    while (rows.len) {
        SliceFor(shape->fields, i) {
            query_field field = shape->fields.ptr[i];
            usize field_size;
            switch (field.kind) {
            case QUERY_ID: field_size = sizeof(query_id); break;
            case QUERY_TEXT: field_size = sizeof(query_text); break;
            case QUERY_BOOL: field_size = sizeof(query_bool); break;
            case QUERY_NUMBER: field_size = sizeof(query_number); break;
            case QUERY_TIMESTAMP: field_size = sizeof(query_timestamp); break;
            case QUERY_RELATION: field_size = sizeof(query_records); break;
            default: writer->failed = true; return records;
            }
            if (field.offset > shape->row_size || field_size > shape->row_size - field.offset) {
                writer->failed = true;
                return records;
            }
            usize offset = row_offset + field.offset;
            bool present = !(field.optional && writer->id == 2);
            switch (field.kind) {
            case QUERY_ID: {
                query_id value = {.present = present, .value = present ? writer->id : 0};
                Store(writer, offset, BytesOf(value)); break;
            }
            case QUERY_TEXT: {
                query_text value = {.present = present};
                if (present) {
                    string_slice text = StringSlice("Synthetic example");
                    value.value = Allocate(writer, text.len);
                    Store(writer, value.value.index, (const_byte_slice){(const u8 *)text.ptr, text.len});
                }
                Store(writer, offset, BytesOf(value)); break;
            }
            case QUERY_BOOL: {
                query_bool value = {.present = present, .value = present};
                Store(writer, offset, BytesOf(value)); break;
            }
            case QUERY_NUMBER: {
                query_number value = {.present = present, .value = present ? 1.5 : 0};
                Store(writer, offset, BytesOf(value)); break;
            }
            case QUERY_TIMESTAMP: {
                query_timestamp value = {.present = present, .value = present ? 1794960000 : 0};
                Store(writer, offset, BytesOf(value)); break;
            }
            case QUERY_RELATION: {
                query_records value = Rows(writer, field.nested, present ? 1 : 0, depth + 1);
                Store(writer, offset, BytesOf(value)); break;
            }
            }
        }
        row_offset += shape->row_size;
        if (ByteSliceSubslice(rows, shape->row_size, rows.len - shape->row_size, &rows).tag != BYTE_SLICE_SUBSLICE_OK) writer->failed = true;
        if (writer->failed) break;
    }
    return records;
}
query_fake_result QueryFake(query_arguments arguments, const query_shape *shape, byte_slice output)
{
    if (!arguments.operation || !arguments.sequence || !shape || (!output.ptr && output.len) || arguments.id > INT64_MAX)
        return (query_fake_result){.tag = QUERY_FAKE_INVALID};
    if (output.len > QUERY_MESSAGE_LIMIT) output.len = QUERY_MESSAGE_LIMIT;
    if (ByteSliceZero(output).tag != BYTE_SLICE_ZERO_OK) return (query_fake_result){.tag = QUERY_FAKE_INVALID};
    fake_writer writer = {.output = output, .id = arguments.id};
    Allocate(&writer, sizeof(query_result));
    query_result root = {.operation = arguments.operation, .sequence = arguments.sequence,
        .outcome = arguments.id == 13 ? QUERY_BACKEND_FAILED : QUERY_SUCCESS};
    if (root.outcome == QUERY_SUCCESS) root.rows = Rows(&writer, shape, arguments.id ? 1 : 0, 0);
    Store(&writer, 0, BytesOf(root));
    if (writer.failed) return (query_fake_result){.tag = QUERY_FAKE_NO_SPACE};
    const_byte_slice message = {output.ptr, writer.used};
    if (QueryValidate(message, arguments.operation, arguments.sequence, shape).tag != QUERY_VALIDATE_OK)
        return (query_fake_result){.tag = QUERY_FAKE_INVALID};
    return (query_fake_result){QUERY_FAKE_OK, message};
}
