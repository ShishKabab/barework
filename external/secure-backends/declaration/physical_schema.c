#include "physical_schema.h"
#include "sql.h"
#include "slice.h"
#include <stdlib.h>

bool PhysicalEqual(string_slice a, string_slice b)
{
    if (a.len != b.len) return false;
    SliceFor(a, i) { if (a.ptr[i] != b.ptr[i]) return false; }
    return true;
}
physical_add_result PhysicalAdd(physical_schema *schema, physical_record record)
{
    if (schema->records.len == schema->capacity) {
        usize capacity = schema->capacity ? schema->capacity * 2 : 32;
        if (capacity < schema->capacity || capacity > SIZE_MAX / sizeof(physical_record)) return (physical_add_result){PHYSICAL_ADD_NO_SPACE};
        void *storage = realloc(schema->records.ptr, capacity * sizeof(physical_record));
        if (!storage) return (physical_add_result){PHYSICAL_ADD_NO_SPACE};
        schema->records.ptr = storage; schema->capacity = capacity;
    }
    if (record.owner.len > SIZE_MAX - record.name.len || record.owner.len + record.name.len > SIZE_MAX - record.value.len) return (physical_add_result){PHYSICAL_ADD_NO_SPACE};
    usize length = record.owner.len + record.name.len + record.value.len;
    byte_slice storage = {length ? malloc(length) : NULL, length};
    if (length && !storage.ptr) return (physical_add_result){PHYSICAL_ADD_NO_SPACE};
    if (ByteSliceCopy(storage, 0, (const_byte_slice){(const u8 *)record.owner.ptr, record.owner.len}).tag != BYTE_SLICE_COPY_OK ||
        ByteSliceCopy(storage, record.owner.len, (const_byte_slice){(const u8 *)record.name.ptr, record.name.len}).tag != BYTE_SLICE_COPY_OK ||
        ByteSliceCopy(storage, record.owner.len + record.name.len, (const_byte_slice){(const u8 *)record.value.ptr, record.value.len}).tag != BYTE_SLICE_COPY_OK) {
        free(storage.ptr); return (physical_add_result){PHYSICAL_ADD_NO_SPACE};
    }
    record.owner.ptr = (const char *)storage.ptr;
    record.name.ptr = storage.ptr ? (const char *)storage.ptr + record.owner.len : NULL;
    record.value.ptr = storage.ptr ? (const char *)storage.ptr + record.owner.len + record.name.len : NULL;
    schema->records.ptr[schema->records.len++] = record;
    return (physical_add_result){PHYSICAL_ADD_OK};
}
void PhysicalDestroy(physical_schema *schema)
{
    SliceFor(schema->records, i) { free((void *)schema->records.ptr[i].owner.ptr); }
    free(schema->records.ptr); *schema = (physical_schema){0};
}
static const physical_record *Find(const physical_schema *schema, physical_kind kind, string_slice owner, string_slice name)
{
    SliceFor(schema->records, i) {
        const physical_record *r = &schema->records.ptr[i];
        if (r->kind == kind && PhysicalEqual(r->name, name) && (kind == PHYSICAL_TABLE || kind == PHYSICAL_INDEX || kind == PHYSICAL_PROCEDURE || PhysicalEqual(r->owner, owner))) return r;
    }
    return NULL;
}
static bool Same(physical_record a, physical_record b)
{ return a.kind == b.kind && a.detail == b.detail && PhysicalEqual(a.owner,b.owner) && PhysicalEqual(a.name,b.name) && PhysicalEqual(a.value,b.value); }
static const physical_record *Key(const physical_schema *schema, string_slice index, u32 ordinal)
{
    SliceFor(schema->records, i) {
        const physical_record *r = &schema->records.ptr[i];
        if (r->kind == PHYSICAL_INDEX_KEY && r->detail == ordinal && PhysicalEqual(r->owner,index)) return r;
    }
    return NULL;
}
static bool Valid(const physical_schema *s)
{
    if (!s->version) return false;
    SliceFor(s->records, i) {
        physical_record r = s->records.ptr[i];
        if (!DeclarationSqlIdentifier(r.name)) return false;
        if (Find(s,r.kind,r.owner,r.name) != &s->records.ptr[i]) return false;
        switch (r.kind) {
        case PHYSICAL_TABLE:
            if (r.detail || r.owner.len || r.value.len) return false;
            { const physical_record *id = Find(s,PHYSICAL_COLUMN,r.name,StringSlice("id"));
              if (!id || id->detail != PHYSICAL_PRIMARY) return false; }
            break;
        case PHYSICAL_COLUMN:
            if (!Find(s,PHYSICAL_TABLE,(string_slice){0},r.owner) || (r.detail & ~(PHYSICAL_OPTIONAL|PHYSICAL_PRIMARY))) return false;
            if (r.detail & PHYSICAL_PRIMARY) {
                if (r.detail != PHYSICAL_PRIMARY || !PhysicalEqual(r.name,StringSlice("id")) || !PhysicalEqual(r.value,StringSlice("bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY"))) return false;
            } else if (!PhysicalEqual(r.value,StringSlice("text")) && !PhysicalEqual(r.value,StringSlice("boolean")) && !PhysicalEqual(r.value,StringSlice("double precision")) && !PhysicalEqual(r.value,StringSlice("timestamp with time zone")) && !PhysicalEqual(r.value,StringSlice("bigint"))) return false;
            break;
        case PHYSICAL_INDEX:
            if (r.detail || r.value.len || !Find(s,PHYSICAL_TABLE,(string_slice){0},r.owner) || Find(s,PHYSICAL_TABLE,(string_slice){0},r.name) || !Key(s,r.name,0)) return false;
            break;
        case PHYSICAL_INDEX_KEY: {
            const physical_record *index = Find(s,PHYSICAL_INDEX,(string_slice){0},r.owner);
            if (!index || r.value.len || r.detail >= 32 || Key(s,r.owner,r.detail) != &s->records.ptr[i] || (r.detail && !Key(s,r.owner,r.detail-1)) || !Find(s,PHYSICAL_COLUMN,index->owner,r.name)) return false;
            break;
        }
        case PHYSICAL_PROCEDURE:
            if (!r.detail || r.detail > s->version || !DeclarationSqlText(r.owner) || !r.owner.len || !DeclarationSqlText(r.value) || !r.value.len) return false;
            break;
        case PHYSICAL_DEPENDENCY:
            if (r.detail || r.value.len || !Find(s,PHYSICAL_PROCEDURE,(string_slice){0},r.owner) || !Find(s,PHYSICAL_TABLE,(string_slice){0},r.name)) return false;
            break;
        default: return false;
        }
    }
    return true;
}
/* Snapshot format 1: decimal header fields followed by exact-length UTF-8
 * strings. No native structs/padding, SQL parsing, or identifier escaping. */
physical_write_result PhysicalWrite(const physical_schema *schema, FILE *stream)
{
    physical_write_result failure = {PHYSICAL_WRITE_IO_ERROR};
    if (fprintf(stream,"physical-schema 1 %u\n",schema->version) < 0) return failure;
    SliceFor(schema->records,i) {
        physical_record r = schema->records.ptr[i];
        if (fprintf(stream,"%u %u %zu %zu %zu\n",(u32)r.kind,r.detail,r.owner.len,r.name.len,r.value.len) < 0 ||
            (r.owner.len && fwrite(r.owner.ptr,1,r.owner.len,stream) != r.owner.len) ||
            (r.name.len && fwrite(r.name.ptr,1,r.name.len,stream) != r.name.len) ||
            (r.value.len && fwrite(r.value.ptr,1,r.value.len,stream) != r.value.len) || fputc('\n',stream) == EOF) return failure;
    }
    return (physical_write_result){PHYSICAL_WRITE_OK};
}
typedef enum decimal_tag { DECIMAL_OK, DECIMAL_INVALID } decimal_tag;
typedef struct decimal_result { decimal_tag tag; u32 value; } decimal_result;
static decimal_result Decimal(const_byte_slice bytes, usize *cursor, u8 separator)
{
    usize start = *cursor; u32 value = 0;
    while (*cursor < bytes.len && bytes.ptr[*cursor] != separator) {
        u8 digit = bytes.ptr[(*cursor)++];
        if (digit < '0' || digit > '9' || value > (UINT32_MAX - (digit-'0')) / 10) return (decimal_result){DECIMAL_INVALID,0};
        value = value * 10 + digit-'0';
    }
    if (*cursor == start || *cursor == bytes.len) return (decimal_result){DECIMAL_INVALID,0};
    ++*cursor; return (decimal_result){DECIMAL_OK,value};
}
physical_read_result PhysicalRead(const_byte_slice bytes, physical_schema *schema)
{
    physical_read_result result = {PHYSICAL_READ_INVALID};
    string_slice magic = StringSlice("physical-schema 1 ");
    if (bytes.len < magic.len) return result;
    SliceFor(magic,i) { if (bytes.ptr[i] != (u8)magic.ptr[i]) return result; }
    usize cursor = magic.len;
    decimal_result version = Decimal(bytes,&cursor,'\n');
    if (version.tag != DECIMAL_OK || !version.value) return result;
    schema->version = version.value;
    while (cursor < bytes.len) {
        u32 storage[5]; declaration_u32_slice fields = {storage,5};
        SliceFor(fields,i) {
            decimal_result n = Decimal(bytes,&cursor,i == 4 ? '\n' : ' ');
            if (n.tag != DECIMAL_OK) goto failed;
            fields.ptr[i] = n.value;
        }
        if (storage[0] < PHYSICAL_TABLE || storage[0] > PHYSICAL_DEPENDENCY) goto failed;
        string_slice strings[3]; struct string_views {string_slice *ptr; usize len;} views = {strings,3};
        SliceFor(views,i) {
            const_byte_slice part;
            if (ConstByteSliceSubslice(bytes,cursor,storage[i+2],&part).tag != CONST_BYTE_SLICE_SUBSLICE_OK) goto failed;
            views.ptr[i] = (string_slice){(const char *)part.ptr,part.len}; cursor += part.len;
        }
        if (cursor >= bytes.len || bytes.ptr[cursor++] != '\n') goto failed;
        if (PhysicalAdd(schema,(physical_record){(physical_kind)storage[0],storage[1],strings[0],strings[1],strings[2]}).tag != PHYSICAL_ADD_OK) { result.tag = PHYSICAL_READ_NO_SPACE; goto failed; }
    }
    if (!Valid(schema)) goto failed;
    return (physical_read_result){PHYSICAL_READ_OK};
failed:
    PhysicalDestroy(schema); return result;
}
static bool IndexFits(const physical_schema *source, physical_record index, const physical_schema *target)
{
    if (!Find(target,PHYSICAL_TABLE,(string_slice){0},index.owner)) return false;
    SliceFor(source->records,i) {
        physical_record key=source->records.ptr[i];
        if (key.kind == PHYSICAL_INDEX_KEY && PhysicalEqual(key.owner,index.name) && !Find(target,PHYSICAL_COLUMN,index.owner,key.name)) return false;
    }
    return true;
}
static bool IndexCovers(const physical_schema *a, physical_record broad, const physical_schema *b, physical_record narrow)
{
    if (!PhysicalEqual(broad.owner,narrow.owner)) return false;
    SliceFor(b->records,i) {
        physical_record key=b->records.ptr[i];
        if (key.kind != PHYSICAL_INDEX_KEY || !PhysicalEqual(key.owner,narrow.name)) continue;
        const physical_record *other=Key(a,broad.name,key.detail);
        if (!other || !PhysicalEqual(other->name,key.name)) return false;
    }
    return true;
}
typedef enum copy_index_tag { COPY_INDEX_OK, COPY_INDEX_NO_SPACE } copy_index_tag;
typedef struct copy_index_result { copy_index_tag tag; } copy_index_result;
static copy_index_result CopyIndex(physical_schema *target,const physical_schema *source,physical_record index)
{
    if (PhysicalAdd(target,index).tag != PHYSICAL_ADD_OK) return (copy_index_result){COPY_INDEX_NO_SPACE};
    SliceFor(source->records,i) {
        physical_record key=source->records.ptr[i];
        if (key.kind == PHYSICAL_INDEX_KEY && PhysicalEqual(key.owner,index.name) && PhysicalAdd(target,key).tag != PHYSICAL_ADD_OK) return (copy_index_result){COPY_INDEX_NO_SPACE};
    }
    return (copy_index_result){COPY_INDEX_OK};
}
static bool RetainedDependency(const physical_schema *old,u32 version,string_slice table)
{
    SliceFor(old->records,i) {
        physical_record r=old->records.ptr[i];
        if (r.kind != PHYSICAL_DEPENDENCY || !PhysicalEqual(r.name,table)) continue;
        const physical_record *procedure=Find(old,PHYSICAL_PROCEDURE,(string_slice){0},r.owner);
        if (procedure && procedure->detail < version) return true;
    }
    return false;
}
typedef enum physical_quote_tag { PHYSICAL_QUOTE_OK, PHYSICAL_QUOTE_IO_ERROR } physical_quote_tag;
typedef struct physical_quote_result { physical_quote_tag tag; } physical_quote_result;
static physical_quote_result PhysicalQuote(FILE *stream,string_slice text)
{
    physical_quote_result failure={PHYSICAL_QUOTE_IO_ERROR};
    if (fputc('"',stream) == EOF) return failure;
    SliceFor(text,i) {
        if (text.ptr[i] == '"' && fputc('"',stream) == EOF) return failure;
        if (fputc((u8)text.ptr[i],stream) == EOF) return failure;
    }
    return (physical_quote_result){fputc('"',stream) == EOF ? PHYSICAL_QUOTE_IO_ERROR : PHYSICAL_QUOTE_OK};
}
typedef enum physical_emit_tag { PHYSICAL_EMIT_OK, PHYSICAL_EMIT_IO_ERROR } physical_emit_tag;
typedef struct physical_emit_result { physical_emit_tag tag; } physical_emit_result;
#define Emit(text) do { string_slice t=(text); if (t.len && fwrite(t.ptr,1,t.len,stream) != t.len) return failure; } while(0)
#define Lit(text) Emit(StringSlice(text))
#define Quote(text) do { if (PhysicalQuote(stream,text).tag != PHYSICAL_QUOTE_OK) return failure; } while(0)
static physical_emit_result PhysicalEmit(const physical_schema *next,const physical_schema *old,FILE *stream)
{
    physical_emit_result failure={PHYSICAL_EMIT_IO_ERROR};
    /* Changed same-version procedures must go before column/table removals. */
    SliceFor(old->records,i) {
        physical_record r=old->records.ptr[i];
        if (r.kind != PHYSICAL_PROCEDURE) continue;
        const physical_record *match=Find(next,r.kind,r.owner,r.name);
        if (match && Same(r,*match)) continue;
        Lit("DROP PROCEDURE "); Emit(r.owner); Lit(";\n\n");
    }
    SliceFor(old->records,i) {
        physical_record r=old->records.ptr[i];
        if (r.kind == PHYSICAL_INDEX && !Find(next,r.kind,r.owner,r.name)) { Lit("DROP INDEX "); Quote(r.name); Lit(";\n\n"); }
    }
    SliceFor(old->records,i) {
        physical_record r=old->records.ptr[i];
        if (r.kind == PHYSICAL_TABLE && !Find(next,r.kind,r.owner,r.name)) { Lit("DROP TABLE "); Quote(r.name); Lit(";\n\n"); }
        if (r.kind == PHYSICAL_COLUMN && Find(next,PHYSICAL_TABLE,(string_slice){0},r.owner) && !Find(next,r.kind,r.owner,r.name)) {
            Lit("ALTER TABLE "); Quote(r.owner); Lit(" DROP COLUMN "); Quote(r.name); Lit(";\n\n");
        }
    }
    SliceFor(next->records,i) {
        physical_record r=next->records.ptr[i];
        if (r.kind == PHYSICAL_TABLE && !Find(old,r.kind,r.owner,r.name)) {
            Lit("CREATE TABLE "); Quote(r.name); Lit(" (\n"); bool first=true;
            SliceFor(next->records,j) {
                physical_record c=next->records.ptr[j];
                if (c.kind != PHYSICAL_COLUMN || !PhysicalEqual(c.owner,r.name)) continue;
                if (!first) Lit(",\n");
                first=false;
                Lit("    "); Quote(c.name); Lit(" "); Emit(c.value);
                if (!(c.detail & (PHYSICAL_OPTIONAL|PHYSICAL_PRIMARY))) Lit(" NOT NULL");
            }
            Lit("\n);\n\n");
        }
        if (r.kind == PHYSICAL_COLUMN && Find(old,PHYSICAL_TABLE,(string_slice){0},r.owner) && !Find(old,r.kind,r.owner,r.name)) {
            Lit("ALTER TABLE "); Quote(r.owner); Lit(" ADD COLUMN "); Quote(r.name); Lit(" "); Emit(r.value); Lit(";\n\n");
        }
        if (r.kind == PHYSICAL_INDEX && !Find(old,r.kind,r.owner,r.name)) {
            Lit("CREATE INDEX "); Quote(r.name); Lit(" ON "); Quote(r.owner); Lit(" (");
            for (u32 ordinal=0; ordinal<32; ++ordinal) {
                const physical_record *key=Key(next,r.name,ordinal); if (!key) break;
                if (ordinal) Lit(", ");
                Quote(key->name);
            }
            Lit(");\n\n");
        }
    }
    SliceFor(next->records,i) {
        physical_record r=next->records.ptr[i];
        if (r.kind != PHYSICAL_PROCEDURE) continue;
        const physical_record *match=Find(old,r.kind,r.owner,r.name);
        if (match && Same(r,*match)) continue;
        Emit(r.value); Lit("\n");
    }
    return (physical_emit_result){PHYSICAL_EMIT_OK};
}
#undef Quote
#undef Lit
#undef Emit
physical_diff_result PhysicalDiff(const physical_schema *desired,const physical_schema *previous,physical_schema *result,FILE *migration)
{
    physical_schema empty={0}; const physical_schema *old=previous ? previous : &empty;
    if (!Valid(desired) || (previous && (!Valid(previous) || desired->version < previous->version))) return (physical_diff_result){PHYSICAL_DIFF_INVALID,StringSlice("invalid physical schema or version")};
    result->version=desired->version;
    /* Reject incompatible operations before emitting SQL. No implicit renames,
     * type conversions, backfills, CASCADE, or retirement of historical procs. */
    SliceFor(old->records,i) {
        physical_record r=old->records.ptr[i];
        if (r.kind != PHYSICAL_TABLE && r.kind != PHYSICAL_COLUMN) continue;
        const physical_record *match=Find(desired,r.kind,r.owner,r.name);
        if (!match && RetainedDependency(old,desired->version,r.kind == PHYSICAL_TABLE ? r.name : r.owner))
            return (physical_diff_result){PHYSICAL_DIFF_UNSUPPORTED,StringSlice("cannot remove a table or column used by a retained procedure; retiring old versions is not implemented")};
        if (match && !Same(r,*match)) return (physical_diff_result){PHYSICAL_DIFF_UNSUPPORTED,StringSlice("column type, identity, and nullability changes require an explicit migration policy")};
    }
    SliceFor(desired->records,i) {
        physical_record r=desired->records.ptr[i];
        if (r.kind == PHYSICAL_COLUMN && Find(old,PHYSICAL_TABLE,(string_slice){0},r.owner) && !Find(old,r.kind,r.owner,r.name) && !(r.detail & PHYSICAL_OPTIONAL))
            return (physical_diff_result){PHYSICAL_DIFF_UNSUPPORTED,StringSlice("adding a required column to an existing table requires a default or backfill")};
        if (r.kind != PHYSICAL_INDEX && r.kind != PHYSICAL_INDEX_KEY && PhysicalAdd(result,r).tag != PHYSICAL_ADD_OK) goto no_space;
    }
    SliceFor(old->records,i) {
        physical_record r=old->records.ptr[i];
        if (r.kind == PHYSICAL_PROCEDURE && r.detail < desired->version) {
            if (Find(result,r.kind,r.owner,r.name)) return (physical_diff_result){PHYSICAL_DIFF_INVALID,StringSlice("historical procedure name collision")};
            if (PhysicalAdd(result,r).tag != PHYSICAL_ADD_OK) goto no_space;
        }
        if (r.kind == PHYSICAL_DEPENDENCY) {
            const physical_record *procedure=Find(old,PHYSICAL_PROCEDURE,(string_slice){0},r.owner);
            if (procedure && procedure->detail < desired->version && PhysicalAdd(result,r).tag != PHYSICAL_ADD_OK) goto no_space;
        }
        if (r.kind == PHYSICAL_INDEX && IndexFits(old,r,desired) && CopyIndex(result,old,r).tag != COPY_INDEX_OK) goto no_space;
    }
    SliceFor(desired->records,i) {
        physical_record r=desired->records.ptr[i];
        if (r.kind != PHYSICAL_INDEX) continue;
        const physical_record *named=Find(result,r.kind,r.owner,r.name);
        if (named && !(IndexCovers(result,*named,desired,r) && IndexCovers(desired,r,result,*named))) return (physical_diff_result){PHYSICAL_DIFF_INVALID,StringSlice("index name collision with a different physical definition")};
        bool covered=false;
        SliceFor(result->records,j) {
            physical_record existing=result->records.ptr[j];
            if (existing.kind == PHYSICAL_INDEX && IndexCovers(result,existing,desired,r)) { covered=true; break; }
        }
        if (!covered && CopyIndex(result,desired,r).tag != COPY_INDEX_OK) goto no_space;
    }
    if (!Valid(result)) return (physical_diff_result){PHYSICAL_DIFF_INVALID,StringSlice("physical schema contains conflicting names or missing dependencies")};
    if (migration && PhysicalEmit(result,old,migration).tag != PHYSICAL_EMIT_OK) return (physical_diff_result){PHYSICAL_DIFF_IO_ERROR,StringSlice("could not write physical schema migration")};
    return (physical_diff_result){.tag=PHYSICAL_DIFF_OK};
no_space:
    return (physical_diff_result){PHYSICAL_DIFF_NO_SPACE,StringSlice("cannot allocate physical schema diff")};
}
