#include "sql.h"
#include "query_sql.h"
#include "index_sql.h"

static bool EqualText(string_slice a, string_slice b)
{
    if (a.len != b.len) return false;
    SliceFor(a, i) { if (a.ptr[i] != b.ptr[i]) return false; }
    return true;
}
/* PostgreSQL's default identifier limit is bytes, not code points. Reject
 * truncation and malformed UTF-8 rather than generating colliding identifiers. */
bool DeclarationSqlText(string_slice text)
{
    if (!text.ptr && text.len) return false;
    for (usize i = 0; i < text.len;) {
        u8 lead = (u8)text.ptr[i++];
        if (!lead) return false;
        if (lead < 128) continue;
        usize continuation;
        u32 value, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) { continuation = 1; value = lead & 31; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { continuation = 2; value = lead & 15; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { continuation = 3; value = lead & 7; minimum = 0x10000; }
        else return false;
        if (continuation > text.len - i) return false;
        for (usize j = 0; j < continuation; ++j) {
            u8 byte = (u8)text.ptr[i++];
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
bool DeclarationSqlIdentifier(string_slice text)
{ return text.len && text.len <= 63 && DeclarationSqlText(text); }
static bool SystemColumn(string_slice name)
{
    const string_slice storage[] = {StringSlice("tableoid"), StringSlice("xmin"), StringSlice("cmin"), StringSlice("xmax"), StringSlice("cmax"), StringSlice("ctid")};
    struct name_slice { const string_slice *ptr; usize len; } names = {storage, sizeof(storage) / sizeof(storage[0])};
    SliceFor(names, i) { if (EqualText(name, names.ptr[i])) return true; }
    return false;
}
typedef enum sql_column_tag { SQL_COLUMN_OK, SQL_COLUMN_SKIP, SQL_COLUMN_INVALID, SQL_COLUMN_UNSUPPORTED } sql_column_tag;
typedef struct sql_column_result {
    sql_column_tag tag;
    string_slice name, type;
    bool primary, optional;
    u32 offset;
} sql_column_result;
static sql_column_result SqlColumn(const declaration_ir *ir, u32 owner, ir_ref member)
{
    if (member.table != IR_FIELD && member.table != IR_RELATIONSHIP && member.table != IR_CONSTRAINT)
        return (sql_column_result){.tag = SQL_COLUMN_INVALID};
    const ir_table *table = DeclarationIrTable(ir, member.table);
    u32 index = member.index;
    if (index >= table->names.len || index >= table->sources.len || index >= table->data.len || index >= table->types.len || index >= table->kinds.len)
        return (sql_column_result){.tag = SQL_COLUMN_INVALID};
    ir_data data = table->data.ptr[index];
    if (data.a.table != IR_COLLECTION || data.a.index != owner) return (sql_column_result){.tag = SQL_COLUMN_INVALID};
    if (member.table == IR_CONSTRAINT || (member.table == IR_RELATIONSHIP && (table->kinds.ptr[index] & IR_RELATION_REVERSE)))
        return (sql_column_result){.tag = SQL_COLUMN_SKIP};
    u32 name = table->names.ptr[index];
    if (name >= ir->strings.len) return (sql_column_result){.tag = SQL_COLUMN_INVALID};
    sql_column_result result = {.tag = SQL_COLUMN_UNSUPPORTED, .name = ir->strings.ptr[name], .offset = table->sources.ptr[index]};
    if (!DeclarationSqlIdentifier(result.name) || SystemColumn(result.name)) return result;
    ir_type type = table->types.ptr[index];
    result.optional = type.optional;
    if (member.table == IR_RELATIONSHIP) {
        if (type.kind != IR_RECORD || (table->kinds.ptr[index] & IR_RELATION_MANY) || data.b.table != IR_COLLECTION || data.b.index >= ir->collections.names.len ||
            type.entity.table != IR_COLLECTION || type.entity.index != data.b.index) return result;
        result.type = StringSlice("bigint");
    } else switch (type.kind) {
    case IR_ID:
        if (type.entity.table != IR_COLLECTION || type.entity.index != owner || type.optional || !EqualText(result.name, StringSlice("id"))) return result;
        result.primary = true;
        result.type = StringSlice("bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY");
        break;
    case IR_STRING: result.type = StringSlice("text"); break;
    case IR_BOOLEAN: result.type = StringSlice("boolean"); break;
    case IR_NUMBER: result.type = StringSlice("double precision"); break;
    case IR_TIMESTAMP: result.type = StringSlice("timestamp with time zone"); break;
    default: return result;
    }
    result.tag = SQL_COLUMN_OK;
    return result;
}
typedef enum sql_validate_tag { SQL_VALIDATE_OK, SQL_VALIDATE_INVALID, SQL_VALIDATE_UNSUPPORTED } sql_validate_tag;
typedef struct sql_validate_result { sql_validate_tag tag; u32 offset; string_slice message; } sql_validate_result;
static sql_validate_result SqlValidate(const declaration_ir *ir)
{
    if (!ir || ir->collections.names.len != ir->collections.data.len || ir->collections.names.len != ir->collections.sources.len)
        return (sql_validate_result){.tag = SQL_VALIDATE_INVALID, .message = StringSlice("invalid collection table")};
    SliceFor(ir->collections.names, i) {
        u32 name = ir->collections.names.ptr[i], offset = ir->collections.sources.ptr[i];
        if (name >= ir->strings.len) return (sql_validate_result){.tag = SQL_VALIDATE_INVALID, .offset = offset, .message = StringSlice("invalid collection name reference")};
        if (!DeclarationSqlIdentifier(ir->strings.ptr[name])) return (sql_validate_result){.tag = SQL_VALIDATE_UNSUPPORTED, .offset = offset, .message = StringSlice("SQL names must be valid UTF-8, nonempty, NUL-free, and at most 63 bytes")};
        ir_range range = ir->collections.data.ptr[i].items;
        if (range.start > ir->extra.len || range.count > ir->extra.len - range.start)
            return (sql_validate_result){.tag = SQL_VALIDATE_INVALID, .offset = offset, .message = StringSlice("invalid collection member range")};
        ir_ref_slice members = {range.count ? ir->extra.ptr + range.start : NULL, range.count};
        usize primary_count = 0;
        SliceFor(members, j) {
            sql_column_result column = SqlColumn(ir, (u32)i, members.ptr[j]);
            switch (column.tag) {
            case SQL_COLUMN_OK: if (column.primary) ++primary_count; break;
            case SQL_COLUMN_SKIP: break;
            case SQL_COLUMN_INVALID: return (sql_validate_result){.tag = SQL_VALIDATE_INVALID, .offset = offset, .message = StringSlice("invalid collection member")};
            case SQL_COLUMN_UNSUPPORTED: return (sql_validate_result){.tag = SQL_VALIDATE_UNSUPPORTED, .offset = column.offset, .message = StringSlice("unsupported SQL column type or identifier (including PostgreSQL system columns and names longer than 63 bytes)")};
            }
        }
        if (primary_count != 1) return (sql_validate_result){.tag = SQL_VALIDATE_INVALID, .offset = offset, .message = StringSlice("collection must have exactly one implicit ID")};
    }
    return (sql_validate_result){.tag = SQL_VALIDATE_OK};
}
physical_build_result PhysicalBuild(const declaration_ir *ir, physical_schema *schema)
{
    sql_validate_result checked = SqlValidate(ir);
    if (checked.tag != SQL_VALIDATE_OK) return (physical_build_result){PHYSICAL_BUILD_UNSUPPORTED,checked.message};
    schema->version=ir->version;
    SliceFor(ir->collections.names,i) {
        string_slice name=ir->strings.ptr[ir->collections.names.ptr[i]];
        if (PhysicalAdd(schema,(physical_record){.kind=PHYSICAL_TABLE,.name=name}).tag != PHYSICAL_ADD_OK) goto no_space;
        ir_range range=ir->collections.data.ptr[i].items;
        ir_ref_slice members={ir->extra.ptr+range.start,range.count};
        SliceFor(members,j) {
            sql_column_result column=SqlColumn(ir,(u32)i,members.ptr[j]);
            if (column.tag == SQL_COLUMN_SKIP) continue;
            if (column.tag != SQL_COLUMN_OK) return (physical_build_result){PHYSICAL_BUILD_UNSUPPORTED,StringSlice("invalid physical column")};
            u32 flags=(column.optional ? PHYSICAL_OPTIONAL : 0) | (column.primary ? PHYSICAL_PRIMARY : 0);
            if (PhysicalAdd(schema,(physical_record){PHYSICAL_COLUMN,flags,name,column.name,column.type}).tag != PHYSICAL_ADD_OK) goto no_space;
        }
    }
    index_physical_result indexes=IndexPhysical(ir,schema);
    if (indexes.tag == INDEX_PHYSICAL_NO_SPACE) goto no_space;
    if (indexes.tag != INDEX_PHYSICAL_OK) return (physical_build_result){PHYSICAL_BUILD_UNSUPPORTED,indexes.message};
    query_physical_result queries=QueryPhysical(ir,schema);
    switch (queries.tag) {
    case QUERY_PHYSICAL_NO_SPACE: goto no_space;
    case QUERY_PHYSICAL_UNSUPPORTED: return (physical_build_result){PHYSICAL_BUILD_UNSUPPORTED,queries.message};
    case QUERY_PHYSICAL_IO_ERROR: return (physical_build_result){PHYSICAL_BUILD_IO_ERROR,queries.message};
    case QUERY_PHYSICAL_OK: break;
    }
    return (physical_build_result){.tag=PHYSICAL_BUILD_OK};
no_space:
    return (physical_build_result){PHYSICAL_BUILD_NO_SPACE,StringSlice("cannot allocate physical schema")};
}
