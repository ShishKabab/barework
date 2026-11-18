#include "ir_dump.h"

typedef enum dump_string_tag { DUMP_STRING_OK, DUMP_STRING_IO_ERROR } dump_string_tag;
typedef struct dump_string_result { dump_string_tag tag; } dump_string_result;
static dump_string_result DumpString(FILE *stream, string_slice text)
{
    if (fputc('"', stream) == EOF) return (dump_string_result){DUMP_STRING_IO_ERROR};
    SliceFor(text, i) {
        u8 c = (u8)text.ptr[i];
        if (c == '"' || c == '\\') {
            if (fputc('\\', stream) == EOF || fputc(c, stream) == EOF) return (dump_string_result){DUMP_STRING_IO_ERROR};
        } else if (c < 32 || c == 127) {
            if (fprintf(stream, "\\u%04x", c) < 0) return (dump_string_result){DUMP_STRING_IO_ERROR};
        } else if (fputc(c, stream) == EOF) return (dump_string_result){DUMP_STRING_IO_ERROR};
    }
    return (dump_string_result){fputc('"', stream) == EOF ? DUMP_STRING_IO_ERROR : DUMP_STRING_OK};
}
typedef enum dump_ref_tag { DUMP_REF_OK, DUMP_REF_IO_ERROR, DUMP_REF_INVALID } dump_ref_tag;
typedef struct dump_ref_result { dump_ref_tag tag; } dump_ref_result;
static dump_ref_result DumpRef(FILE *stream, const declaration_ir *ir, ir_ref ref)
{
    string_slice table_name = DeclarationIrTableName(ref.table);
    if (fwrite(table_name.ptr, 1, table_name.len, stream) != table_name.len) return (dump_ref_result){DUMP_REF_IO_ERROR};
    if (ref.table == IR_NONE) return (dump_ref_result){DUMP_REF_OK};
    const ir_table *table = DeclarationIrTable(ir, ref.table);
    if (!table || ref.index >= table->names.len) return (dump_ref_result){DUMP_REF_INVALID};
    if (fprintf(stream, "#%u", ref.index) < 0) return (dump_ref_result){DUMP_REF_IO_ERROR};
    u32 name = table->names.ptr[ref.index];
    if (name != UINT32_MAX) {
        if (name >= ir->strings.len) return (dump_ref_result){DUMP_REF_INVALID};
        if (fputc('(', stream) == EOF || DumpString(stream, ir->strings.ptr[name]).tag != DUMP_STRING_OK || fputc(')', stream) == EOF) return (dump_ref_result){DUMP_REF_IO_ERROR};
    }
    return (dump_ref_result){DUMP_REF_OK};
}
declaration_ir_dump_result DeclarationIrDump(const declaration_ir *ir, FILE *stream)
{
    if (!ir || !stream) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_INVALID};
#define Write(text) do { string_slice text_slice = StringSlice(text); if (fwrite(text_slice.ptr, 1, text_slice.len, stream) != text_slice.len) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_IO_ERROR}; } while (0)
#define Ref(value) do { dump_ref_result dumped_ref = DumpRef(stream, ir, value); if (dumped_ref.tag == DUMP_REF_INVALID) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_INVALID}; if (dumped_ref.tag == DUMP_REF_IO_ERROR) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_IO_ERROR}; } while (0)
#define Number(format, ...) do { if (fprintf(stream, format, __VA_ARGS__) < 0) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_IO_ERROR}; } while (0)
    Write("application_ir v1\n");
    Number("version %u\n", ir->version);
    for (ir_table_kind kind = IR_COLLECTION; kind <= IR_STATEMENT; ++kind) {
        const ir_table *table = DeclarationIrTable(ir, kind);
        Write("\n");
        SliceFor(table->names, i) {
            Ref(((ir_ref){kind, (u32)i}));
            Number(" @%u", table->sources.ptr[i]);
            ir_type type = table->types.ptr[i];
            string_slice type_name = DeclarationIrTypeName(type.kind);
            Write(" type=");
            if (fwrite(type_name.ptr, 1, type_name.len, stream) != type_name.len) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_IO_ERROR};
            if (type.entity.table != IR_NONE) { Write("<"); Ref(type.entity); Write(">"); }
            if (type.optional) Write("?");
            u32 tag = table->kinds.ptr[i];
            ir_data data = table->data.ptr[i];
            switch (kind) {
            case IR_COLLECTION: break;
            case IR_FIELD: Write(" owner="); Ref(data.a); break;
            case IR_RELATIONSHIP:
                Write(" from="); Ref(data.a); Write(" to="); Ref(data.b); Write(" inverse="); Ref(data.c);
                if (tag & IR_RELATION_REVERSE) Write(" reverse");
                if (tag & IR_RELATION_MANY) Write(" many"); else if (tag & IR_RELATION_OPTIONAL) Write(" zero_or_one"); else Write(" one");
                break;
            case IR_ROUTE: Write(" context="); Ref(data.a); Write(" path="); Ref(data.b); break;
            case IR_QUERY: Write(" scope="); Ref(data.a); Write(" source="); Ref(data.b); Write(" filter="); Ref(data.c); break;
            case IR_CONSTRAINT:
                if (tag == IR_CHECK_UPDATE) Write(" check_update"); else if (tag == IR_UNIQUE) Write(" unique"); else Write(" filtered_unique");
                Write(" owner="); Ref(data.a);
                if (tag == IR_CHECK_UPDATE) Write(" context="); else Write(" filter=");
                Ref(data.b); break;
            case IR_BINDING:
                if (tag == IR_BINDING_CONTEXT) Write(" context"); else if (tag == IR_BINDING_LOCAL) Write(" local");
                else if (tag == IR_BINDING_PATH_PARAMETER) Write(" path_parameter"); else if (tag == IR_BINDING_PATH) Write(" path"); else Write(" path_literal");
                Write(" scope="); Ref(data.a); if (data.b.table != IR_NONE) { Write(" value="); Ref(data.b); } break;
            case IR_EXPRESSION: {
                string_slice operation = DeclarationIrExpressionName((ir_expression_kind)tag);
                Write(" op=");
                if (fwrite(operation.ptr, 1, operation.len, stream) != operation.len) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_IO_ERROR};
                if (data.a.table != IR_NONE) { Write(" a="); Ref(data.a); }
                if (data.b.table != IR_NONE) { Write(" b="); Ref(data.b); }
                break;
            }
            case IR_SELECTION: Write(" relationship="); Ref(data.a); Write(" filter="); Ref(data.b); break;
            case IR_STATEMENT:
                if (tag == IR_LET) Write(" let"); else if (tag == IR_RETURN) Write(" return"); else Write(" evaluate");
                Write(" value="); Ref(data.a); break;
            default: return (declaration_ir_dump_result){DECLARATION_IR_DUMP_INVALID};
            }
            if (data.items.count) {
                if (data.items.start > ir->extra.len || data.items.count > ir->extra.len - data.items.start) return (declaration_ir_dump_result){DECLARATION_IR_DUMP_INVALID};
                ir_ref_slice items = {ir->extra.ptr + data.items.start, data.items.count};
                Write(" items=[");
                SliceFor(items, item) { if (item) Write(", "); Ref(items.ptr[item]); }
                Write("]");
            }
            Write("\n");
        }
    }
#undef Number
#undef Ref
#undef Write
    return (declaration_ir_dump_result){fflush(stream) == 0 ? DECLARATION_IR_DUMP_OK : DECLARATION_IR_DUMP_IO_ERROR};
}
