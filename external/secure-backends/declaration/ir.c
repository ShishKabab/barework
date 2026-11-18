#include "ir.h"
#include "slice.h"
#include <stdlib.h>

/* Each pass has its own result tag. The diagnostic is separate from control flow. */
#define RefResult(type, prefix) typedef enum type##_tag { prefix##_OK, prefix##_ERROR } type##_tag; typedef struct type { type##_tag tag; ir_ref ref; } type
#define StepResult(type, prefix) typedef enum type##_tag { prefix##_OK, prefix##_ERROR } type##_tag; typedef struct type { type##_tag tag; } type
RefResult(add_result, ADD);
RefResult(expression_result, EXPRESSION);
RefResult(filter_result, FILTER);
RefResult(query_result, QUERY);
RefResult(member_result, MEMBER);
StepResult(schema_result, SCHEMA);
StepResult(relationship_result, RELATIONSHIP);
StepResult(body_result, BODY);
StepResult(selection_result, SELECTION);
StepResult(register_result, REGISTER);
StepResult(constraint_result, CONSTRAINT);
StepResult(strings_result, STRINGS);
#undef RefResult
#undef StepResult

typedef struct resolver {
    const declaration_ast *ast;
    declaration_ir *ir;
    usize capacity, extra_used, strings_used, bytes_used, scratch_used;
    ir_ref_slice scratch;
    declaration_u32_slice text_ids;
    ir_ref_slice declarations;
    declaration_u32_slice counts;
    u32 error_offset;
    string_slice error;
} resolver;
typedef struct scope { ir_ref owner, collection; } scope;
static ir_ref None(void) { return (ir_ref){0}; }
static bool SameRef(ir_ref a, ir_ref b) { return a.table == b.table && a.index == b.index; }
static ir_type Type(ir_value_kind kind, ir_ref entity) { return (ir_type){.kind = kind, .entity = entity}; }
const ir_table *DeclarationIrTable(const declaration_ir *ir, ir_table_kind kind)
{
    switch (kind) {
    case IR_COLLECTION: return &ir->collections; case IR_FIELD: return &ir->fields;
    case IR_RELATIONSHIP: return &ir->relationships; case IR_ROUTE: return &ir->routes;
    case IR_QUERY: return &ir->queries; case IR_CONSTRAINT: return &ir->constraints;
    case IR_BINDING: return &ir->bindings; case IR_EXPRESSION: return &ir->expressions;
    case IR_SELECTION: return &ir->selections; case IR_STATEMENT: return &ir->statements;
    default: return NULL;
    }
}
/* Mutable access stays private to the builder. */
static ir_table *Table(resolver *r, ir_table_kind kind)
{ return (ir_table *)DeclarationIrTable(r->ir, kind); }
static declaration_u32_slice Children(resolver *r, u32 node)
{
    /* Private view of an unmodified successful parser AST. The parser owns
     * bounds/shape validation; all node IDs here come from that AST's edges. */
    declaration_node_data data = r->ast->node_data.ptr[node];
    return (declaration_u32_slice){data.count ? r->ast->extra.ptr + data.start : NULL, data.count};
}
static declaration_node_tag Tag(resolver *r, u32 node) { return (declaration_node_tag)r->ast->node_tags.ptr[node]; }
static u32 Source(resolver *r, u32 node) { return r->ast->token_starts.ptr[r->ast->main_tokens.ptr[node]]; }
static void Diagnose(resolver *r, u32 node, string_slice text) { r->error_offset = Source(r, node); r->error = text; }
#define Check(condition, node, text) do { if (!(condition)) { Diagnose(r, node, StringSlice(text)); return failure; } } while (0)
#define Take(variable, type, call, ok) type variable = call; if (variable.tag != ok) return failure
static bool Equal(string_slice a, string_slice b)
{
    if (a.len != b.len) return false;
    SliceFor(a, i) { if (a.ptr[i] != b.ptr[i]) return false; }
    return true;
}
static bool Text(resolver *r, u32 id, string_slice text)
{ return id < r->strings_used && Equal(r->ir->strings.ptr[id], text); }
static u32 Name(resolver *r, u32 node) { return r->text_ids.ptr[node]; }
static bool Named(resolver *r, u32 node, string_slice text) { return Text(r, Name(r, node), text); }
static ir_ref Find(resolver *r, ir_table_kind kind, u32 name, ir_ref owner)
{
    ir_table *table = Table(r, kind);
    declaration_u32_slice names = {table->names.ptr, r->counts.ptr[kind]};
    SliceFor(names, i) {
        if (names.ptr[i] == name && (kind == IR_COLLECTION || kind == IR_ROUTE || SameRef(table->data.ptr[i].a, owner))) return (ir_ref){kind, (u32)i};
    }
    return None();
}
static add_result Add(resolver *r, ir_table_kind table_kind, u32 kind, u32 name, ir_type type, ir_data data, u32 node)
{
    if (r->counts.ptr[table_kind] == r->capacity) { Diagnose(r, node, StringSlice("IR capacity exceeded")); return (add_result){.tag = ADD_ERROR}; }
    u32 index = (u32)r->counts.ptr[table_kind]++;
    ir_table *table = Table(r, table_kind);
    table->names.ptr[index] = name; table->sources.ptr[index] = Source(r, node);
    table->kinds.ptr[index] = kind; table->types.ptr[index] = type; table->data.ptr[index] = data;
    return (add_result){.tag = ADD_OK, .ref = {table_kind, index}};
}
typedef enum append_tag { APPEND_OK, APPEND_FULL } append_tag;
typedef struct append_result { append_tag tag; } append_result;
static append_result Append(resolver *r, ir_ref ref)
{
    if (r->scratch_used == r->scratch.len) { Diagnose(r, 0, StringSlice("IR list capacity exceeded")); return (append_result){APPEND_FULL}; }
    r->scratch.ptr[r->scratch_used++] = ref;
    return (append_result){APPEND_OK};
}
typedef enum save_list_tag { SAVE_LIST_OK, SAVE_LIST_FULL } save_list_tag;
typedef struct save_list_result { save_list_tag tag; ir_range range; } save_list_result;
static save_list_result SaveList(resolver *r, usize mark)
{
    usize count = r->scratch_used - mark;
    if (count > r->ir->extra.len - r->extra_used) { Diagnose(r, 0, StringSlice("IR extra capacity exceeded")); return (save_list_result){.tag = SAVE_LIST_FULL}; }
    ir_range range = {(u32)r->extra_used, (u32)count};
    ir_ref_slice items = {count ? r->scratch.ptr + mark : NULL, count};
    SliceFor(items, i) { r->ir->extra.ptr[r->extra_used++] = items.ptr[i]; }
    r->scratch_used = mark;
    return (save_list_result){.tag = SAVE_LIST_OK, .range = range};
}
/* Every source name/literal is decoded once and interned; equal spellings share
 * IDs, including escaped spelling. The resulting IR owns the string bytes. */
static u32 HexValue(u8 c) { return c <= '9' ? c - '0' : c <= 'F' ? c - 'A' + 10 : c - 'a' + 10; }
static strings_result Strings(resolver *r)
{
    strings_result failure = {STRINGS_ERROR};
    SliceFor(r->ast->node_tags, index) {
        u32 node = (u32)index;
        r->text_ids.ptr[index] = UINT32_MAX;
        declaration_node_tag tag = Tag(r, node);
        if (tag != NODE_IDENTIFIER && tag != NODE_STRING && tag != NODE_BOOLEAN && tag != NODE_NUMBER && tag != NODE_NULL) continue;
        u32 token = r->ast->main_tokens.ptr[index];
        u32 start = r->ast->token_starts.ptr[token], end = r->ast->token_ends.ptr[token];
        const_byte_slice raw;
        Check(end >= start && ConstByteSliceSubslice(r->ast->source, start, end - start, &raw).tag == CONST_BYTE_SLICE_SUBSLICE_OK, node, "invalid token span");
        usize begin = r->bytes_used, first = tag == NODE_STRING ? 1 : 0, last = raw.len - (tag == NODE_STRING ? 1 : 0);
        for (usize p = first; p < last; ++p) {
            u32 c = raw.ptr[p];
            bool unicode_escape = false;
            if (tag == NODE_STRING && c == '\\') {
                Check(++p < last, node, "incomplete string escape");
                c = raw.ptr[p];
                switch (c) {
                case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
                case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'u': {
                    unicode_escape = true;
                    Check(last - p > 4, node, "incomplete Unicode escape");
                    c = 0; for (usize h = 0; h < 4; ++h) c = (c << 4) | HexValue(raw.ptr[++p]);
                    if (c >= 0xd800 && c <= 0xdbff) {
                        Check(last - p > 6 && raw.ptr[p + 1] == '\\' && raw.ptr[p + 2] == 'u', node, "unpaired Unicode surrogate");
                        p += 2; u32 low = 0; for (usize h = 0; h < 4; ++h) low = (low << 4) | HexValue(raw.ptr[++p]);
                        Check(low >= 0xdc00 && low <= 0xdfff, node, "invalid Unicode surrogate pair");
                        c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                    } else Check(c < 0xdc00 || c > 0xdfff, node, "unpaired Unicode surrogate");
                    break;
                }
                default: break;
                }
            }
            /* Raw UTF-8 bytes are copied as bytes; only escaped code points need encoding. */
            usize needed = c < 0x100 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
            /* ASCII and raw bytes above 127 are already encoded. A Unicode escape
             * below 256 still needs two bytes when non-ASCII. */
            if (unicode_escape && c >= 0x80 && c < 0x100) needed = 2;
            Check(needed <= r->ir->string_storage.len - r->bytes_used, node, "string storage exhausted");
            if (needed == 1) r->ir->string_storage.ptr[r->bytes_used++] = (u8)c;
            else {
                u8 lead = needed == 2 ? 0xc0 : needed == 3 ? 0xe0 : 0xf0;
                r->ir->string_storage.ptr[r->bytes_used++] = lead | (u8)(c >> (6 * (needed - 1)));
                for (usize b = needed - 1; b; --b) r->ir->string_storage.ptr[r->bytes_used++] = 0x80 | (u8)((c >> (6 * (b - 1))) & 63);
            }
        }
        string_slice decoded = {(const char *)r->ir->string_storage.ptr + begin, r->bytes_used - begin};
        u32 id = UINT32_MAX;
        ir_string_slice strings = {r->ir->strings.ptr, r->strings_used};
        SliceFor(strings, i) { if (Equal(strings.ptr[i], decoded)) { id = (u32)i; break; } }
        if (id == UINT32_MAX) {
            Check(r->strings_used < r->ir->strings.len, node, "too many strings");
            id = (u32)r->strings_used++; r->ir->strings.ptr[id] = decoded;
        } else r->bytes_used = begin;
        r->text_ids.ptr[index] = id;
    }
    return (strings_result){STRINGS_OK};
}
static bool ValidName(resolver *r, u32 id)
{
    if (id >= r->strings_used || !r->ir->strings.ptr[id].len) return false;
    string_slice text = r->ir->strings.ptr[id];
    SliceFor(text, i) { if ((u8)text.ptr[i] < 32) return false; }
    return true;
}
/* Lookup is a genuine predicate: absence is not an error until its caller knows
 * whether the property/member is required. */
static bool Property(resolver *r, u32 object, string_slice name, u32 *value)
{
    declaration_u32_slice properties = Children(r, object);
    SliceFor(properties, i) {
        declaration_u32_slice pair = Children(r, properties.ptr[i]);
        if (pair.len == 2 && Named(r, pair.ptr[0], name)) { *value = pair.ptr[1]; return true; }
    }
    return false;
}
static bool AllowedObject(resolver *r, u32 node, string_slice one, string_slice two, string_slice three)
{
    if (Tag(r, node) != NODE_OBJECT) return false;
    declaration_u32_slice properties = Children(r, node);
    SliceFor(properties, i) {
        declaration_u32_slice pair = Children(r, properties.ptr[i]);
        if (pair.len != 2 || (!(one.len && Named(r, pair.ptr[0], one)) && !(two.len && Named(r, pair.ptr[0], two)) && !(three.len && Named(r, pair.ptr[0], three)))) return false;
        for (usize j = 0; j < i; ++j) {
            declaration_u32_slice previous = Children(r, properties.ptr[j]);
            if (Name(r, previous.ptr[0]) == Name(r, pair.ptr[0])) return false;
        }
    }
    return true;
}
static declaration_u32_slice Arguments(resolver *r, u32 call)
{
    declaration_u32_slice children = Children(r, call);
    return children.len == 2 && Tag(r, children.ptr[1]) == NODE_ARGUMENTS ? Children(r, children.ptr[1]) : (declaration_u32_slice){0};
}
static bool CallNamed(resolver *r, u32 call, string_slice name)
{
    declaration_u32_slice children = Children(r, call);
    return children.len == 2 && Tag(r, children.ptr[0]) == NODE_IDENTIFIER && Named(r, children.ptr[0], name);
}
static member_result Member(resolver *r, ir_ref collection, u32 name, u32 node)
{
    member_result failure = {.tag = MEMBER_ERROR};
    ir_ref field = Find(r, IR_FIELD, name, collection), relationship = Find(r, IR_RELATIONSHIP, name, collection);
    Check(field.table != IR_NONE || relationship.table != IR_NONE, node, "unknown field or relationship in collection");
    Check(field.table == IR_NONE || relationship.table == IR_NONE, node, "ambiguous field/relationship name");
    return (member_result){.tag = MEMBER_OK, .ref = field.table != IR_NONE ? field : relationship};
}
typedef enum intern_tag { INTERN_OK, INTERN_FULL } intern_tag;
typedef struct intern_result { intern_tag tag; u32 id; } intern_result;
static intern_result Intern(resolver *r, string_slice text)
{
    ir_string_slice strings = {r->ir->strings.ptr, r->strings_used};
    SliceFor(strings, i) { if (Equal(strings.ptr[i], text)) return (intern_result){.tag = INTERN_OK, .id = (u32)i}; }
    if (r->strings_used == r->ir->strings.len || text.len > r->ir->string_storage.len - r->bytes_used) {
        Diagnose(r, 0, StringSlice("IR string capacity exceeded")); return (intern_result){.tag = INTERN_FULL};
    }
    if (ByteSliceCopy(r->ir->string_storage, r->bytes_used, (const_byte_slice){(const u8 *)text.ptr, text.len}).tag != BYTE_SLICE_COPY_OK) {
        Diagnose(r, 0, StringSlice("IR string copy failed")); return (intern_result){.tag = INTERN_FULL};
    }
    u32 id = (u32)r->strings_used++;
    r->ir->strings.ptr[id] = (string_slice){(const char *)r->ir->string_storage.ptr + r->bytes_used, text.len};
    r->bytes_used += text.len;
    return (intern_result){.tag = INTERN_OK, .id = id};
}
static register_result Register(resolver *r)
{
    register_result failure = {REGISTER_ERROR};
    declaration_u32_slice roots = Children(r, 0);
    SliceFor(roots, i) {
        u32 node = roots.ptr[i];
        if (Tag(r, node) != NODE_VERSION) continue;
        Check(!r->ir->version, node, "exactly one version declaration is required");
        declaration_u32_slice args = Arguments(r, node);
        Check(args.len == 1 && Tag(r, args.ptr[0]) == NODE_NUMBER, node, "version requires one positive integer");
        string_slice number = r->ir->strings.ptr[Name(r, args.ptr[0])];
        u32 value = 0;
        SliceFor(number, digit) {
            u8 c = (u8)number.ptr[digit];
            Check(c >= '0' && c <= '9' && value <= (UINT32_MAX - (c - '0')) / 10, node, "version must be an integer from 1 to 4294967295");
            value = value * 10 + c - '0';
        }
        Check(value != 0, node, "version must start at 1");
        r->ir->version = value;
    }
    Check(r->ir->version != 0, 0, "exactly one version(integer) declaration is required");
    Take(id_name, intern_result, Intern(r, StringSlice("id")), INTERN_OK);
    SliceFor(roots, i) {
        u32 node = roots.ptr[i];
        if (Tag(r, node) == NODE_QUERY || Tag(r, node) == NODE_VERSION) continue;
        bool collection = Tag(r, node) == NODE_COLLECTION;
        Check(collection || Tag(r, node) == NODE_ROUTE, node, "unsupported declaration");
        declaration_u32_slice args = Arguments(r, node);
        Check(args.len == (collection ? 2 : 3), node, "wrong declaration argument count");
        Check(Tag(r, args.ptr[0]) == NODE_STRING && ValidName(r, Name(r, args.ptr[0])), args.ptr[0], "declaration name must be a nonempty string");
        u32 name = Name(r, args.ptr[0]);
        ir_table_kind table = collection ? IR_COLLECTION : IR_ROUTE;
        Check(Find(r, table, name, None()).table == IR_NONE, args.ptr[0], "duplicate declaration name");
        u32 callback = args.ptr[args.len - 1];
        Check(Tag(r, callback) == NODE_CALLBACK, callback, "expected declaration callback");
        declaration_u32_slice callback_parts = Children(r, callback), params = Children(r, callback_parts.ptr[0]);
        Check(params.len == (collection ? 0 : 1), callback, "collection callback takes no parameters; route callback takes one");
        Take(added, add_result, Add(r, table, 0, name, Type(IR_VOID, None()), (ir_data){0}, node), ADD_OK);
        r->declarations.ptr[node] = added.ref;
        if (collection) {
            Take(field, add_result, Add(r, IR_FIELD, 0, id_name.id, Type(IR_ID, added.ref), (ir_data){.a = added.ref}, node), ADD_OK);
            (void)field;
        } else {
            Check(Tag(r, args.ptr[1]) == NODE_STRING, args.ptr[1], "route path must be a string");
            string_slice path = r->ir->strings.ptr[Name(r, args.ptr[1])];
            Check(path.len && path.ptr[0] == '/', args.ptr[1], "route path must start with '/'");
            Check(ValidName(r, Name(r, args.ptr[1])), args.ptr[1], "route path contains control bytes");
            Take(path_binding, add_result, Add(r, IR_BINDING, IR_BINDING_PATH, Name(r, args.ptr[1]), Type(IR_STRING, None()), (ir_data){.a = added.ref}, args.ptr[1]), ADD_OK);
            Table(r, IR_ROUTE)->data.ptr[added.ref.index].b = path_binding.ref;
            for (usize start = 1; start < path.len;) {
                usize end = start;
                while (end < path.len && path.ptr[end] != '/') ++end;
                Check(end > start, args.ptr[1], "empty route path segment");
                bool parameter = path.ptr[start] == ':';
                usize offset = start + (parameter ? 1 : 0);
                const_byte_slice part;
                Check(ConstByteSliceSubslice((const_byte_slice){(const u8 *)path.ptr, path.len}, offset, end - offset, &part).tag == CONST_BYTE_SLICE_SUBSLICE_OK, node, "invalid path segment");
                Take(segment_name, intern_result, Intern(r, (string_slice){(const char *)part.ptr, part.len}), INTERN_OK);
                Check(ValidName(r, segment_name.id), args.ptr[1], "invalid route segment name");
                if (parameter) {
                    ir_table *bindings = Table(r, IR_BINDING);
                    declaration_u32_slice names = {bindings->names.ptr, r->counts.ptr[IR_BINDING]};
                    SliceFor(names, b) { Check(!(names.ptr[b] == segment_name.id && bindings->kinds.ptr[b] == IR_BINDING_PATH_PARAMETER && SameRef(bindings->data.ptr[b].a, added.ref)), args.ptr[1], "duplicate route parameter"); }
                }
                Take(segment, add_result, Add(r, IR_BINDING, parameter ? IR_BINDING_PATH_PARAMETER : IR_BINDING_PATH_LITERAL, segment_name.id, Type(IR_STRING, None()), (ir_data){.a = added.ref}, args.ptr[1]), ADD_OK);
                (void)segment;
                start = end + 1;
            }
            Take(context, add_result, Add(r, IR_BINDING, IR_BINDING_CONTEXT, Name(r, params.ptr[0]), Type(IR_ROUTE_CONTEXT, added.ref), (ir_data){.a = added.ref}, params.ptr[0]), ADD_OK);
            Table(r, IR_ROUTE)->data.ptr[added.ref.index].a = context.ref;
        }
    }
    return (register_result){REGISTER_OK};
}
static relationship_result Relationship(resolver *r, ir_ref owner, u32 target_node, u32 alias, bool single, u32 node)
{
    relationship_result failure = {RELATIONSHIP_ERROR};
    Check(Tag(r, target_node) == NODE_STRING, target_node, "relationship target must be a collection name");
    ir_ref target = Find(r, IR_COLLECTION, Name(r, target_node), None());
    Check(target.table != IR_NONE, target_node, "unknown relationship target collection");
    u32 forward = alias == UINT32_MAX ? Name(r, target_node) : alias;
    u32 reverse = Table(r, IR_COLLECTION)->names.ptr[owner.index];
    Check(ValidName(r, forward), node, "invalid relationship alias");
    Check(Find(r, IR_FIELD, forward, owner).table == IR_NONE && Find(r, IR_RELATIONSHIP, forward, owner).table == IR_NONE, node, "duplicate field/relationship name");
    Check(Find(r, IR_FIELD, reverse, target).table == IR_NONE && Find(r, IR_RELATIONSHIP, reverse, target).table == IR_NONE && !(SameRef(owner, target) && forward == reverse), node, "ambiguous generated reverse relationship");
    Take(a, add_result, Add(r, IR_RELATIONSHIP, 0, forward, Type(IR_RECORD, target), (ir_data){.a = owner, .b = target}, node), ADD_OK);
    ir_type reverse_type = Type(single ? IR_RECORD : IR_RECORDS, owner); reverse_type.optional = single;
    Take(b, add_result, Add(r, IR_RELATIONSHIP, IR_RELATION_REVERSE | (single ? IR_RELATION_OPTIONAL : IR_RELATION_MANY), reverse, reverse_type, (ir_data){.a = target, .b = owner, .c = a.ref}, node), ADD_OK);
    Table(r, IR_RELATIONSHIP)->data.ptr[a.ref.index].c = b.ref;
    return (relationship_result){RELATIONSHIP_OK};
}
/* Field pass precedes relationship pass so collisions are independent of source order. */
static schema_result Schema(resolver *r, bool relationships)
{
    schema_result failure = {SCHEMA_ERROR};
    declaration_u32_slice roots = Children(r, 0);
    SliceFor(roots, i) {
        u32 root = roots.ptr[i];
        if (Tag(r, root) != NODE_COLLECTION) continue;
        ir_ref owner = r->declarations.ptr[root];
        declaration_u32_slice args = Arguments(r, root), callback = Children(r, args.ptr[1]), statements = Children(r, callback.ptr[1]);
        SliceFor(statements, j) {
            u32 node = statements.ptr[j];
            Check(Tag(r, node) == NODE_CALL, node, "collection body must contain schema helper calls");
            declaration_u32_slice arguments = Arguments(r, node);
            if (CallNamed(r, node, StringSlice("field"))) {
                if (relationships) continue;
                Check(arguments.len == 2 && Tag(r, arguments.ptr[0]) == NODE_STRING && ValidName(r, Name(r, arguments.ptr[0])), node, "field requires a name and options");
                u32 name = Name(r, arguments.ptr[0]), options = arguments.ptr[1], type_node, optional_node;
                Check(AllowedObject(r, options, StringSlice("type"), StringSlice("optional"), (string_slice){0}), options, "unknown or duplicate field option");
                Check(Property(r, options, StringSlice("type"), &type_node) && Tag(r, type_node) == NODE_STRING, options, "field type must be a string");
                ir_value_kind kind;
                if (Named(r, type_node, StringSlice("string"))) kind = IR_STRING;
                else if (Named(r, type_node, StringSlice("boolean"))) kind = IR_BOOLEAN;
                else if (Named(r, type_node, StringSlice("timestamp"))) kind = IR_TIMESTAMP;
                else if (Named(r, type_node, StringSlice("number"))) kind = IR_NUMBER;
                else { Diagnose(r, type_node, StringSlice("unknown field type")); return failure; }
                ir_type type = Type(kind, None());
                if (Property(r, options, StringSlice("optional"), &optional_node)) {
                    Check(Tag(r, optional_node) == NODE_BOOLEAN, optional_node, "optional must be a boolean");
                    type.optional = Named(r, optional_node, StringSlice("true"));
                }
                Check(Find(r, IR_FIELD, name, owner).table == IR_NONE, arguments.ptr[0], "duplicate field (id is implicit)");
                Take(field, add_result, Add(r, IR_FIELD, 0, name, type, (ir_data){.a = owner}, node), ADD_OK); (void)field;
            } else if (CallNamed(r, node, StringSlice("singleChildOf")) || CallNamed(r, node, StringSlice("multipleChildOf"))) {
                if (!relationships) continue;
                Check(arguments.len == 1 || arguments.len == 2, node, "child relationship requires target and optional alias options");
                u32 alias = UINT32_MAX, alias_node;
                if (arguments.len == 2) {
                    Check(AllowedObject(r, arguments.ptr[1], StringSlice("alias"), (string_slice){0}, (string_slice){0}), arguments.ptr[1], "unknown or duplicate relationship option");
                    if (Property(r, arguments.ptr[1], StringSlice("alias"), &alias_node)) {
                        Check(Tag(r, alias_node) == NODE_STRING, alias_node, "alias must be a string"); alias = Name(r, alias_node);
                    }
                }
                /* Relationship has its own outcome; translate at this boundary. */
                relationship_result relationship = Relationship(r, owner, arguments.ptr[0], alias, CallNamed(r, node, StringSlice("singleChildOf")), node);
                if (relationship.tag != RELATIONSHIP_OK) return failure;
            } else if (CallNamed(r, node, StringSlice("connects"))) {
                if (!relationships) continue;
                Check(arguments.len == 2, node, "connects requires two target collections");
                SliceFor(arguments, k) {
                    relationship_result relationship = Relationship(r, owner, arguments.ptr[k], UINT32_MAX, false, node);
                    if (relationship.tag != RELATIONSHIP_OK) return failure;
                }
            } else Check(CallNamed(r, node, StringSlice("unique")) || CallNamed(r, node, StringSlice("constraint")) || CallNamed(r, node, StringSlice("checkUpdate")), node, "unknown schema helper");
        }
    }
    return (schema_result){SCHEMA_OK};
}
static expression_result Expression(resolver *r, u32 node, scope context, usize depth);
static filter_result Filter(resolver *r, u32 node, ir_ref collection, scope context);
static query_result Query(resolver *r, u32 node, scope context);
static ir_type RefType(resolver *r, ir_ref ref) { return Table(r, ref.table)->types.ptr[ref.index]; }
static bool Compatible(ir_type a, ir_type b)
{ return a.kind == b.kind && SameRef(a.entity, b.entity); }
static ir_ref Binding(resolver *r, u32 name, ir_ref owner, bool path)
{
    ir_table *table = Table(r, IR_BINDING);
    declaration_u32_slice names = {table->names.ptr, r->counts.ptr[IR_BINDING]};
    SliceFor(names, i) {
        bool matches_kind = path ? table->kinds.ptr[i] == IR_BINDING_PATH_PARAMETER : (table->kinds.ptr[i] == IR_BINDING_CONTEXT || table->kinds.ptr[i] == IR_BINDING_LOCAL);
        if (matches_kind && names.ptr[i] == name && SameRef(table->data.ptr[i].a, owner)) return (ir_ref){IR_BINDING, (u32)i};
    }
    return None();
}
static expression_result Expression(resolver *r, u32 node, scope context, usize depth)
{
    expression_result failure = {.tag = EXPRESSION_ERROR};
    Check(depth <= 128, node, "expression nesting limit exceeded");
    declaration_node_tag tag = Tag(r, node);
    ir_ref result;
    if (tag == NODE_STRING || tag == NODE_NUMBER || tag == NODE_BOOLEAN || tag == NODE_NULL) {
        ir_value_kind kind = tag == NODE_STRING ? IR_STRING : tag == NODE_NUMBER ? IR_NUMBER : tag == NODE_BOOLEAN ? IR_BOOLEAN : IR_NULL;
        Take(literal, add_result, Add(r, IR_EXPRESSION, IR_LITERAL, Name(r, node), Type(kind, None()), (ir_data){0}, node), ADD_OK);
        result = literal.ref;
    } else if (tag == NODE_IDENTIFIER) {
        ir_ref binding = Binding(r, Name(r, node), context.owner, false);
        Check(binding.table != IR_NONE, node, "unresolved binding name");
        Take(load, add_result, Add(r, IR_EXPRESSION, IR_LOAD_BINDING, UINT32_MAX, RefType(r, binding), (ir_data){.a = binding}, node), ADD_OK);
        result = load.ref;
    } else if (tag == NODE_MEMBER) {
        declaration_u32_slice parts = Children(r, node);
        Take(base, expression_result, Expression(r, parts.ptr[0], context, depth + 1), EXPRESSION_OK);
        ir_type type = RefType(r, base.ref);
        u32 name_node = parts.ptr[1], name = Name(r, name_node);
        ir_expression_kind operation;
        ir_data data = {.a = base.ref};
        if (type.kind == IR_ROUTE_CONTEXT) {
            Check(Named(r, name_node, StringSlice("route")), name_node, "unknown route context member");
            operation = IR_ROUTE_VALUES; type.kind = IR_ROUTE_PARAMETERS;
        } else if (type.kind == IR_ROUTE_PARAMETERS) {
            ir_ref parameter = Binding(r, name, type.entity, true);
            Check(parameter.table != IR_NONE, name_node, "unknown route parameter");
            operation = IR_ROUTE_PARAMETER; data.a = parameter; type = Type(IR_STRING, None());
        } else if (type.kind == IR_UPDATE_CONTEXT) {
            Check(Named(r, name_node, StringSlice("new")), name_node, "unknown update context member");
            operation = IR_NEW_RECORD; type.kind = IR_RECORD;
        } else {
            Check(type.kind == IR_RECORD && !type.optional, name_node, "member access requires a nonoptional single record");
            Take(member, member_result, Member(r, type.entity, name, name_node), MEMBER_OK);
            operation = member.ref.table == IR_FIELD ? IR_FIELD_ACCESS : IR_RELATION_ACCESS;
            data.b = member.ref; type = RefType(r, member.ref);
        }
        Take(access, add_result, Add(r, IR_EXPRESSION, operation, UINT32_MAX, type, data, node), ADD_OK);
        result = access.ref;
    } else if (tag == NODE_QUERY) {
        Check(context.owner.table == IR_ROUTE || context.owner.table == IR_NONE, node, "query declarations are only supported at file or route scope");
        Take(query, query_result, Query(r, node, context), QUERY_OK);
        Take(run, add_result, Add(r, IR_EXPRESSION, IR_RUN_QUERY, UINT32_MAX, RefType(r, query.ref), (ir_data){.a = query.ref}, node), ADD_OK);
        result = run.ref;
    } else if (tag == NODE_CALL && CallNamed(r, node, StringSlice("exists"))) {
        declaration_u32_slice arguments = Arguments(r, node);
        Check(arguments.len == 2 && Tag(r, arguments.ptr[0]) == NODE_STRING, node, "exists requires collection name and filter");
        ir_ref collection = Find(r, IR_COLLECTION, Name(r, arguments.ptr[0]), None());
        Check(collection.table != IR_NONE, arguments.ptr[0], "unknown exists collection");
        Take(predicate, filter_result, Filter(r, arguments.ptr[1], collection, context), FILTER_OK);
        Take(exists, add_result, Add(r, IR_EXPRESSION, IR_EXISTS, UINT32_MAX, Type(IR_BOOLEAN, None()), (ir_data){.a = collection, .b = predicate.ref}, node), ADD_OK);
        result = exists.ref;
    } else { Diagnose(r, node, StringSlice("unsupported expression or unknown builtin")); return failure; }
    return (expression_result){.tag = EXPRESSION_OK, .ref = result};
}
static filter_result Filter(resolver *r, u32 node, ir_ref collection, scope context)
{
    filter_result failure = {.tag = FILTER_ERROR};
    u32 source = node == UINT32_MAX ? 0 : node;
    Take(true_name, intern_result, Intern(r, StringSlice("true")), INTERN_OK);
    if (node == UINT32_MAX || (Tag(r, node) == NODE_OBJECT && Children(r, node).len == 0)) {
        Take(always, add_result, Add(r, IR_EXPRESSION, IR_LITERAL, true_name.id, Type(IR_BOOLEAN, None()), (ir_data){0}, source), ADD_OK);
        return (filter_result){.tag = FILTER_OK, .ref = always.ref};
    }
    Check(Tag(r, node) == NODE_OBJECT, node, "filter must be an object of equality conditions");
    declaration_u32_slice properties = Children(r, node);
    Take(row, add_result, Add(r, IR_EXPRESSION, IR_CURRENT_RECORD, UINT32_MAX, Type(IR_RECORD, collection), (ir_data){.a = collection}, node), ADD_OK);
    ir_ref predicate = None();
    SliceFor(properties, i) {
        declaration_u32_slice pair = Children(r, properties.ptr[i]);
        for (usize j = 0; j < i; ++j) { declaration_u32_slice prior = Children(r, properties.ptr[j]); Check(Name(r, prior.ptr[0]) != Name(r, pair.ptr[0]), pair.ptr[0], "duplicate filter key"); }
        Take(member, member_result, Member(r, collection, Name(r, pair.ptr[0]), pair.ptr[0]), MEMBER_OK);
        ir_type member_type = RefType(r, member.ref);
        Check(member_type.kind != IR_RECORDS, pair.ptr[0], "equality filter cannot compare a to-many relationship");
        Take(access, add_result, Add(r, IR_EXPRESSION, member.ref.table == IR_FIELD ? IR_FIELD_ACCESS : IR_RELATION_ACCESS, UINT32_MAX, member_type, (ir_data){.a = row.ref, .b = member.ref}, pair.ptr[0]), ADD_OK);
        ir_ref left = access.ref;
        if (member.ref.table == IR_RELATIONSHIP) {
            Take(id_name, intern_result, Intern(r, StringSlice("id")), INTERN_OK);
            ir_ref id = Find(r, IR_FIELD, id_name.id, member_type.entity);
            Check(id.table != IR_NONE, pair.ptr[0], "relationship target is missing implicit id");
            ir_type id_type = RefType(r, id); id_type.optional = member_type.optional;
            Take(key, add_result, Add(r, IR_EXPRESSION, IR_FIELD_ACCESS, UINT32_MAX, id_type, (ir_data){.a = left, .b = id}, pair.ptr[0]), ADD_OK);
            left = key.ref; member_type = id_type;
        }
        Take(value, expression_result, Expression(r, pair.ptr[1], context, 0), EXPRESSION_OK);
        ir_ref right = value.ref;
        ir_type value_type = RefType(r, right);
        if (Table(r, IR_EXPRESSION)->kinds.ptr[right.index] == IR_ROUTE_PARAMETER && member_type.kind != IR_STRING) {
            Check(member_type.kind == IR_ID || member_type.kind == IR_NUMBER || member_type.kind == IR_BOOLEAN || member_type.kind == IR_TIMESTAMP, pair.ptr[1], "route parameter cannot convert to this type");
            ir_type conversion_type = member_type; conversion_type.optional = false;
            Take(convert, add_result, Add(r, IR_EXPRESSION, IR_PARSE_PARAMETER, UINT32_MAX, conversion_type, (ir_data){.a = right}, pair.ptr[1]), ADD_OK);
            right = convert.ref; value_type = conversion_type;
        }
        Check(Compatible(member_type, value_type) || (member_type.optional && value_type.kind == IR_NULL), pair.ptr[1], "filter value type does not match field or relationship ID");
        Take(equal, add_result, Add(r, IR_EXPRESSION, IR_EQUAL, UINT32_MAX, Type(IR_BOOLEAN, None()), (ir_data){.a = left, .b = right}, properties.ptr[i]), ADD_OK);
        if (predicate.table == IR_NONE) predicate = equal.ref;
        else {
            Take(both, add_result, Add(r, IR_EXPRESSION, IR_AND, UINT32_MAX, Type(IR_BOOLEAN, None()), (ir_data){.a = predicate, .b = equal.ref}, properties.ptr[i]), ADD_OK);
            predicate = both.ref;
        }
    }
    return (filter_result){.tag = FILTER_OK, .ref = predicate};
}
/* Appends selection references to the caller's scratch range. Nested selections
 * finish their own ranges first, preserving contiguous sibling ranges. */
static selection_result Selections(resolver *r, u32 object, ir_ref collection, scope context, usize depth)
{
    selection_result failure = {SELECTION_ERROR};
    Check(depth <= 128 && Tag(r, object) == NODE_OBJECT, object, "relations must be an object within nesting limit");
    declaration_u32_slice properties = Children(r, object);
    SliceFor(properties, i) {
        declaration_u32_slice pair = Children(r, properties.ptr[i]);
        for (usize j = 0; j < i; ++j) { declaration_u32_slice prior = Children(r, properties.ptr[j]); Check(Name(r, prior.ptr[0]) != Name(r, pair.ptr[0]), pair.ptr[0], "duplicate relation selection"); }
        ir_ref relationship = Find(r, IR_RELATIONSHIP, Name(r, pair.ptr[0]), collection);
        Check(relationship.table != IR_NONE, pair.ptr[0], "unknown relationship in query selection (use its alias)");
        ir_ref target = Table(r, IR_RELATIONSHIP)->data.ptr[relationship.index].b;
        u32 options = pair.ptr[1], filter = UINT32_MAX, nested;
        bool object_options = Tag(r, options) == NODE_OBJECT;
        Check(object_options || (Tag(r, options) == NODE_BOOLEAN && Named(r, options, StringSlice("true"))), options, "relationship selection must be true or options");
        if (object_options) {
            Check(AllowedObject(r, options, StringSlice("where"), StringSlice("relations"), (string_slice){0}), options, "unknown or duplicate selection option");
            Property(r, options, StringSlice("where"), &filter);
        }
        Take(predicate, filter_result, Filter(r, filter, target, context), FILTER_OK);
        usize mark = r->scratch_used;
        if (object_options && Property(r, options, StringSlice("relations"), &nested)) {
            Take(children, selection_result, Selections(r, nested, target, context, depth + 1), SELECTION_OK); (void)children;
        }
        Take(list, save_list_result, SaveList(r, mark), SAVE_LIST_OK);
        Take(selection, add_result, Add(r, IR_SELECTION, 0, UINT32_MAX, RefType(r, relationship), (ir_data){.a = relationship, .b = predicate.ref, .items = list.range}, properties.ptr[i]), ADD_OK);
        Take(appended, append_result, Append(r, selection.ref), APPEND_OK); (void)appended;
    }
    return (selection_result){SELECTION_OK};
}
static query_result Query(resolver *r, u32 node, scope context)
{
    query_result failure = {.tag = QUERY_ERROR};
    declaration_u32_slice args = Arguments(r, node);
    Check(args.len == 2 && Tag(r, args.ptr[0]) == NODE_STRING && ValidName(r, Name(r, args.ptr[0])), node, "query requires name and options");
    u32 name = Name(r, args.ptr[0]), options = args.ptr[1], collection_node, filter = UINT32_MAX, relations;
    Check(Find(r, IR_QUERY, name, context.owner).table == IR_NONE, args.ptr[0], "duplicate query name in scope");
    Check(AllowedObject(r, options, StringSlice("collection"), StringSlice("where"), StringSlice("relations")), options, "unknown or duplicate query option");
    Check(Property(r, options, StringSlice("collection"), &collection_node) && Tag(r, collection_node) == NODE_STRING, options, "query requires a collection name");
    ir_ref collection = Find(r, IR_COLLECTION, Name(r, collection_node), None());
    Check(collection.table != IR_NONE, collection_node, "unknown query collection");
    Property(r, options, StringSlice("where"), &filter);
    Take(predicate, filter_result, Filter(r, filter, collection, context), FILTER_OK);
    usize mark = r->scratch_used;
    if (Property(r, options, StringSlice("relations"), &relations)) {
        Take(selected, selection_result, Selections(r, relations, collection, context, 0), SELECTION_OK); (void)selected;
    }
    Take(list, save_list_result, SaveList(r, mark), SAVE_LIST_OK);
    Take(query, add_result, Add(r, IR_QUERY, 0, name, Type(IR_RECORDS, collection), (ir_data){.a = context.owner, .b = collection, .c = predicate.ref, .items = list.range}, node), ADD_OK);
    return (query_result){.tag = QUERY_OK, .ref = query.ref};
}
static body_result Body(resolver *r, u32 block, scope context)
{
    body_result failure = {BODY_ERROR};
    declaration_u32_slice statements = Children(r, block);
    usize mark = r->scratch_used;
    bool returned = false;
    SliceFor(statements, i) {
        u32 node = statements.ptr[i];
        Check(!returned, node, "unreachable statement after return");
        declaration_node_tag tag = Tag(r, node);
        ir_ref value;
        ir_statement_kind kind;
        if (tag == NODE_LET) {
            declaration_u32_slice parts = Children(r, node);
            u32 name = Name(r, parts.ptr[0]);
            Check(Binding(r, name, context.owner, false).table == IR_NONE, parts.ptr[0], "duplicate local or callback parameter name");
            Take(initializer, expression_result, Expression(r, parts.ptr[1], context, 0), EXPRESSION_OK);
            Take(binding, add_result, Add(r, IR_BINDING, IR_BINDING_LOCAL, name, RefType(r, initializer.ref), (ir_data){.a = context.owner, .b = initializer.ref}, parts.ptr[0]), ADD_OK);
            value = binding.ref; kind = IR_LET;
        } else if (tag == NODE_RETURN) {
            declaration_u32_slice parts = Children(r, node);
            value = None(); kind = IR_RETURN; returned = true;
            if (parts.len) {
                Take(expression, expression_result, Expression(r, parts.ptr[0], context, 0), EXPRESSION_OK); value = expression.ref;
            }
            if (context.owner.table == IR_CONSTRAINT)
                Check(value.table != IR_NONE && RefType(r, value).kind == IR_BOOLEAN && !RefType(r, value).optional, node, "update check must return a boolean");
        } else {
            Take(expression, expression_result, Expression(r, node, context, 0), EXPRESSION_OK);
            value = expression.ref; kind = IR_EVALUATE;
        }
        Take(statement, add_result, Add(r, IR_STATEMENT, kind, UINT32_MAX, Type(IR_VOID, None()), (ir_data){.a = value}, node), ADD_OK);
        Take(appended, append_result, Append(r, statement.ref), APPEND_OK); (void)appended;
    }
    Check(context.owner.table != IR_CONSTRAINT || returned, block, "update check must end with a boolean return");
    Take(list, save_list_result, SaveList(r, mark), SAVE_LIST_OK);
    Table(r, context.owner.table)->data.ptr[context.owner.index].items = list.range;
    return (body_result){BODY_OK};
}
static constraint_result Constraints(resolver *r)
{
    constraint_result failure = {CONSTRAINT_ERROR};
    declaration_u32_slice roots = Children(r, 0);
    SliceFor(roots, i) {
        u32 root = roots.ptr[i];
        if (Tag(r, root) != NODE_COLLECTION) continue;
        ir_ref collection = r->declarations.ptr[root];
        declaration_u32_slice args = Arguments(r, root), callback = Children(r, args.ptr[1]), statements = Children(r, callback.ptr[1]);
        SliceFor(statements, j) {
            u32 node = statements.ptr[j];
            bool check = CallNamed(r, node, StringSlice("checkUpdate")), unique = CallNamed(r, node, StringSlice("unique"));
            if (!check && !unique && !CallNamed(r, node, StringSlice("constraint"))) continue;
            declaration_u32_slice arguments = Arguments(r, node);
            Check(arguments.len == 2 && Tag(r, arguments.ptr[0]) == NODE_STRING && ValidName(r, Name(r, arguments.ptr[0])), node, "constraint requires name and definition");
            u32 name = Name(r, arguments.ptr[0]), definition = arguments.ptr[1];
            Check(Find(r, IR_CONSTRAINT, name, collection).table == IR_NONE, arguments.ptr[0], "duplicate constraint name");
            Take(constraint, add_result, Add(r, IR_CONSTRAINT, check ? IR_CHECK_UPDATE : unique ? IR_UNIQUE : IR_FILTERED_UNIQUE, name, Type(IR_VOID, None()), (ir_data){.a = collection}, node), ADD_OK);
            if (check) {
                Check(Tag(r, definition) == NODE_CALLBACK, definition, "checkUpdate requires a callback");
                declaration_u32_slice parts = Children(r, definition), params = Children(r, parts.ptr[0]);
                Check(params.len == 1, definition, "update callback takes one context parameter");
                Take(binding, add_result, Add(r, IR_BINDING, IR_BINDING_CONTEXT, Name(r, params.ptr[0]), Type(IR_UPDATE_CONTEXT, collection), (ir_data){.a = constraint.ref}, params.ptr[0]), ADD_OK);
                Table(r, IR_CONSTRAINT)->data.ptr[constraint.ref.index].b = binding.ref;
                Take(body, body_result, Body(r, parts.ptr[1], (scope){constraint.ref, collection}), BODY_OK); (void)body;
            } else {
                Check(AllowedObject(r, definition, StringSlice("target"), unique ? (string_slice){0} : StringSlice("filter"), (string_slice){0}), definition, "unknown or duplicate constraint option");
                u32 target, filter = UINT32_MAX;
                Check(Property(r, definition, StringSlice("target"), &target) && Tag(r, target) == NODE_ARRAY, definition, "constraint requires target array");
                declaration_u32_slice keys = Children(r, target);
                Check(keys.len, target, "constraint target must not be empty");
                usize mark = r->scratch_used;
                SliceFor(keys, k) {
                    Check(Tag(r, keys.ptr[k]) == NODE_STRING, keys.ptr[k], "constraint target must be a member name");
                    Take(member, member_result, Member(r, collection, Name(r, keys.ptr[k]), keys.ptr[k]), MEMBER_OK);
                    Check(RefType(r, member.ref).kind != IR_RECORDS, keys.ptr[k], "constraint key cannot be a to-many relationship");
                    for (usize previous = mark; previous < r->scratch_used; ++previous) Check(!SameRef(r->scratch.ptr[previous], member.ref), keys.ptr[k], "duplicate constraint target");
                    Take(appended, append_result, Append(r, member.ref), APPEND_OK); (void)appended;
                }
                Take(list, save_list_result, SaveList(r, mark), SAVE_LIST_OK);
                if (!unique) Check(Property(r, definition, StringSlice("filter"), &filter), definition, "filtered constraint requires filter");
                Take(predicate, filter_result, Filter(r, filter, collection, (scope){None(), collection}), FILTER_OK);
                Table(r, IR_CONSTRAINT)->data.ptr[constraint.ref.index].b = predicate.ref;
                Table(r, IR_CONSTRAINT)->data.ptr[constraint.ref.index].items = list.range;
            }
        }
    }
    return (constraint_result){CONSTRAINT_OK};
}
void DeclarationIrDestroy(declaration_ir *ir)
{
    if (!ir) return;
    for (ir_table_kind kind = IR_COLLECTION; kind <= IR_STATEMENT; ++kind) {
        ir_table *table = (ir_table *)DeclarationIrTable(ir, kind);
        free(table->names.ptr); free(table->sources.ptr); free(table->kinds.ptr); free(table->types.ptr); free(table->data.ptr);
    }
    free(ir->extra.ptr); free(ir->strings.ptr); free(ir->string_storage.ptr);
    *ir = (declaration_ir){0};
}
declaration_resolve_result DeclarationResolve(const declaration_ast *ast, declaration_ir *output)
{
    if (!output) return (declaration_resolve_result){.tag = DECLARATION_RESOLVE_INVALID_ARGUMENT, .message = StringSlice("null IR output")};
    *output = (declaration_ir){0};
    if (!ast || !ast->node_tags.len || ast->node_tags.ptr[0] != NODE_FILE)
        return (declaration_resolve_result){.tag = DECLARATION_RESOLVE_INVALID_ARGUMENT, .message = StringSlice("expected parsed declaration AST")};
    usize slashes = 0;
    SliceFor(ast->source, i) { if (ast->source.ptr[i] == '/') ++slashes; }
    if (ast->source.len > (SIZE_MAX - 128) / 2 || ast->node_tags.len > (UINT32_MAX - 32) / 16 || slashes > (UINT32_MAX - 32) / 2)
        return (declaration_resolve_result){.tag = DECLARATION_RESOLVE_NO_SPACE, .message = StringSlice("IR exceeds index limits")};
    usize capacity = ast->node_tags.len * 8 + slashes + 32;
    if (capacity > UINT32_MAX / 2 || capacity > SIZE_MAX / (2 * sizeof(ir_data)))
        return (declaration_resolve_result){.tag = DECLARATION_RESOLVE_NO_SPACE, .message = StringSlice("IR exceeds allocation limits")};
    declaration_ir ir = {0};
    u32 counts_storage[IR_STATEMENT + 1] = {0};
    resolver r = {.ast = ast, .ir = &ir, .capacity = capacity,
        .counts = {counts_storage, sizeof(counts_storage) / sizeof(counts_storage[0])}};
    bool allocated = true;
#define Allocate(slice, count) do { (slice).ptr = calloc(count, sizeof(*(slice).ptr)); (slice).len = count; if (!(slice).ptr) allocated = false; } while (0)
    for (ir_table_kind kind = IR_COLLECTION; kind <= IR_STATEMENT; ++kind) {
        ir_table *table = Table(&r, kind);
        Allocate(table->names, capacity); Allocate(table->sources, capacity); Allocate(table->kinds, capacity);
        Allocate(table->types, capacity); Allocate(table->data, capacity);
    }
    Allocate(ir.extra, capacity * 2); Allocate(ir.strings, capacity); Allocate(ir.string_storage, ast->source.len * 2 + 128);
    Allocate(r.scratch, capacity); Allocate(r.text_ids, ast->node_tags.len); Allocate(r.declarations, ast->node_tags.len);
#undef Allocate
    declaration_resolve_result result = {.tag = DECLARATION_RESOLVE_NO_SPACE, .message = StringSlice("cannot allocate IR tables")};
    if (!allocated) goto done;
    result.tag = DECLARATION_RESOLVE_SEMANTIC_ERROR;
    if (Strings(&r).tag != STRINGS_OK || Register(&r).tag != REGISTER_OK ||
        Schema(&r, false).tag != SCHEMA_OK || Schema(&r, true).tag != SCHEMA_OK || Constraints(&r).tag != CONSTRAINT_OK) goto semantic_error;
    declaration_u32_slice roots = Children(&r, 0);
    SliceFor(roots, i) {
        u32 node = roots.ptr[i];
        if (Tag(&r, node) == NODE_ROUTE) {
            ir_ref route = r.declarations.ptr[node];
            declaration_u32_slice arguments = Arguments(&r, node), callback = Children(&r, arguments.ptr[2]);
            if (Body(&r, callback.ptr[1], (scope){route, None()}).tag != BODY_OK) goto semantic_error;
        } else if (Tag(&r, node) == NODE_QUERY) {
            if (Query(&r, node, (scope){0}).tag != QUERY_OK) goto semantic_error;
        }
    }
    /* Attach resolved member ranges after all forward/reverse members exist. */
    for (usize c = 0; c < r.counts.ptr[IR_COLLECTION]; ++c) {
        usize mark = r.scratch_used;
        ir_ref owner = {IR_COLLECTION, (u32)c};
        const ir_table_kind kinds_storage[] = {IR_FIELD, IR_RELATIONSHIP, IR_CONSTRAINT};
        struct kinds_slice { const ir_table_kind *ptr; usize len; } kinds = {kinds_storage, 3};
        SliceFor(kinds, k) {
            ir_table_kind kind = kinds.ptr[k];
            ir_data_slice entries = {Table(&r, kind)->data.ptr, r.counts.ptr[kind]};
            SliceFor(entries, i) {
                if (SameRef(entries.ptr[i].a, owner) && Append(&r, (ir_ref){kind, (u32)i}).tag != APPEND_OK) goto semantic_error;
            }
        }
        save_list_result list = SaveList(&r, mark);
        if (list.tag != SAVE_LIST_OK) goto semantic_error;
        ir.collections.data.ptr[c].items = list.range;
    }
    for (ir_table_kind kind = IR_COLLECTION; kind <= IR_STATEMENT; ++kind) {
        ir_table *table = Table(&r, kind);
        table->names.len = table->sources.len = table->kinds.len = table->types.len = table->data.len = r.counts.ptr[kind];
    }
    ir.extra.len = r.extra_used; ir.strings.len = r.strings_used; ir.string_storage.len = r.bytes_used;
    *output = ir;
    ir = (declaration_ir){0};
    result = (declaration_resolve_result){.tag = DECLARATION_RESOLVE_OK};
    goto done;
semantic_error:
    result.offset = r.error_offset; result.message = r.error;
done:
    free(r.scratch.ptr); free(r.text_ids.ptr); free(r.declarations.ptr);
    DeclarationIrDestroy(&ir);
    return result;
}
string_slice DeclarationIrTableName(ir_table_kind kind)
{
    switch (kind) {
    case IR_NONE: return StringSlice("none"); case IR_COLLECTION: return StringSlice("collection");
    case IR_FIELD: return StringSlice("field"); case IR_RELATIONSHIP: return StringSlice("relationship");
    case IR_ROUTE: return StringSlice("route"); case IR_QUERY: return StringSlice("query");
    case IR_CONSTRAINT: return StringSlice("constraint"); case IR_BINDING: return StringSlice("binding");
    case IR_EXPRESSION: return StringSlice("expression"); case IR_SELECTION: return StringSlice("selection");
    case IR_STATEMENT: return StringSlice("statement");
    }
    return StringSlice("invalid");
}
string_slice DeclarationIrTypeName(ir_value_kind kind)
{
    const string_slice names[] = {StringSlice("void"), StringSlice("string"), StringSlice("boolean"), StringSlice("number"), StringSlice("timestamp"),
        StringSlice("id"), StringSlice("record"), StringSlice("records"), StringSlice("route_context"), StringSlice("route_parameters"), StringSlice("update_context"), StringSlice("null")};
    return (usize)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : StringSlice("invalid");
}
string_slice DeclarationIrExpressionName(ir_expression_kind kind)
{
    const string_slice names[] = {StringSlice("literal"), StringSlice("load_binding"), StringSlice("current_record"), StringSlice("new_record"), StringSlice("route_values"),
        StringSlice("route_parameter"), StringSlice("field_access"), StringSlice("relationship_access"), StringSlice("equal"), StringSlice("and"),
        StringSlice("parse_parameter_checked"), StringSlice("exists"), StringSlice("run_query")};
    return (usize)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : StringSlice("invalid");
}
