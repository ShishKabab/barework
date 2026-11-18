#include "index_sql.h"
#include "sql.h"
#include <stdlib.h>

enum { INDEX_KEY_LIMIT = 32 };
typedef struct index_candidate { u32 collection; ir_range keys; u64 signature; bool active; } index_candidate;
typedef struct index_candidate_slice { index_candidate *ptr; usize len; } index_candidate_slice;
typedef struct index_plan {
    const declaration_ir *ir;
    index_candidate_slice candidates;
    ir_ref_slice keys;
    usize candidates_used, keys_used;
    u32 offset;
    string_slice error;
} index_plan;
typedef struct lookup_keys { ir_ref_slice storage; usize used; } lookup_keys;
#define StepResult(type, prefix) typedef enum type##_tag { prefix##_OK, prefix##_ERROR } type##_tag; typedef struct type { type##_tag tag; } type
StepResult(add_key_result, ADD_KEY);
StepResult(equalities_result, EQUALITIES);
StepResult(lookup_result, LOOKUP);
StepResult(walk_expression_result, WALK_EXPRESSION);
StepResult(walk_selections_result, WALK_SELECTIONS);
#undef StepResult
#define Check(condition, message) do { if (!(condition)) { p->error = StringSlice(message); return failure; } } while (0)
#define Take(variable, type, call, okay) type variable = call; if (variable.tag != okay) return failure
static bool Equal(string_slice a, string_slice b)
{
    if (a.len != b.len) return false;
    SliceFor(a, i) { if (a.ptr[i] != b.ptr[i]) return false; }
    return true;
}
static bool Before(string_slice a, string_slice b)
{
    usize length = a.len < b.len ? a.len : b.len;
    string_slice common = {a.ptr, length};
    SliceFor(common, i) { if ((u8)a.ptr[i] != (u8)b.ptr[i]) return (u8)a.ptr[i] < (u8)b.ptr[i]; }
    return a.len < b.len;
}
static bool SameRef(ir_ref a, ir_ref b) { return a.table == b.table && a.index == b.index; }
static bool HasRef(const declaration_ir *ir, ir_ref ref, ir_table_kind table)
{
    const ir_table *rows = DeclarationIrTable(ir, table);
    return ref.table == table && rows && ref.index < rows->data.len;
}
static string_slice Name(const declaration_ir *ir, ir_ref ref)
{
    const ir_table *table = DeclarationIrTable(ir, ref.table);
    return ir->strings.ptr[table->names.ptr[ref.index]];
}
static bool Primary(const declaration_ir *ir, ir_ref ref)
{ return HasRef(ir, ref, IR_FIELD) && ir->fields.types.ptr[ref.index].kind == IR_ID; }
static ir_ref IdField(const declaration_ir *ir, u32 collection)
{
    SliceFor(ir->fields.names, i) {
        if (ir->fields.data.ptr[i].a.table == IR_COLLECTION && ir->fields.data.ptr[i].a.index == collection && ir->fields.types.ptr[i].kind == IR_ID)
            return (ir_ref){IR_FIELD, (u32)i};
    }
    return (ir_ref){0};
}
static add_key_result AddKey(index_plan *p, lookup_keys *keys, ir_ref column)
{
    add_key_result failure = {ADD_KEY_ERROR};
    ir_ref_slice used = {keys->storage.ptr, keys->used};
    SliceFor(used, i) { if (SameRef(used.ptr[i], column)) return (add_key_result){ADD_KEY_OK}; }
    Check(keys->used < keys->storage.len, "query index exceeds PostgreSQL's 32-key limit");
    keys->storage.ptr[keys->used++] = column;
    return (add_key_result){ADD_KEY_OK};
}
/* Only physical columns of the current lookup row are equality keys. A forward
 * relationship ID is stored in the source row; other navigation is a subquery. */
static bool PhysicalColumn(const declaration_ir *ir, ir_ref expression, u32 collection, ir_ref *column)
{
    if (!HasRef(ir, expression, IR_EXPRESSION) || ir->expressions.kinds.ptr[expression.index] != IR_FIELD_ACCESS) return false;
    ir_data data = ir->expressions.data.ptr[expression.index];
    if (!HasRef(ir, data.a, IR_EXPRESSION) || !HasRef(ir, data.b, IR_FIELD)) return false;
    ir_data receiver = ir->expressions.data.ptr[data.a.index];
    u32 kind = ir->expressions.kinds.ptr[data.a.index];
    if (kind == IR_CURRENT_RECORD && receiver.a.table == IR_COLLECTION && receiver.a.index == collection && ir->fields.data.ptr[data.b.index].a.index == collection) {
        *column = data.b; return true;
    }
    if (kind == IR_RELATION_ACCESS && Primary(ir, data.b) && HasRef(ir, receiver.b, IR_RELATIONSHIP) &&
        !(ir->relationships.kinds.ptr[receiver.b.index] & IR_RELATION_REVERSE) && HasRef(ir, receiver.a, IR_EXPRESSION)) {
        ir_data base = ir->expressions.data.ptr[receiver.a.index];
        if (ir->expressions.kinds.ptr[receiver.a.index] == IR_CURRENT_RECORD && base.a.table == IR_COLLECTION && base.a.index == collection &&
            ir->relationships.data.ptr[receiver.b.index].a.index == collection) { *column = receiver.b; return true; }
    }
    return false;
}
static bool ExternalValue(const declaration_ir *ir, ir_ref expression, usize depth)
{
    if (depth > 256 || !HasRef(ir, expression, IR_EXPRESSION)) return false;
    ir_data data = ir->expressions.data.ptr[expression.index];
    switch ((ir_expression_kind)ir->expressions.kinds.ptr[expression.index]) {
    case IR_LITERAL: case IR_ROUTE_PARAMETER: return true;
    case IR_PARSE_PARAMETER: return ExternalValue(ir, data.a, depth + 1);
    case IR_LOAD_BINDING:
        return HasRef(ir, data.a, IR_BINDING) && ir->bindings.kinds.ptr[data.a.index] == IR_BINDING_LOCAL && ExternalValue(ir, ir->bindings.data.ptr[data.a.index].b, depth + 1);
    default: return false;
    }
}
static equalities_result Equalities(index_plan *p, ir_ref expression, u32 collection, lookup_keys *keys, usize depth)
{
    equalities_result failure = {EQUALITIES_ERROR};
    const declaration_ir *ir = p->ir;
    if (expression.table == IR_NONE) return (equalities_result){EQUALITIES_OK};
    Check(depth <= 256 && HasRef(ir, expression, IR_EXPRESSION), "invalid index predicate");
    p->offset = ir->expressions.sources.ptr[expression.index];
    ir_data data = ir->expressions.data.ptr[expression.index];
    u32 kind = ir->expressions.kinds.ptr[expression.index];
    if (kind == IR_AND) {
        Take(left, equalities_result, Equalities(p, data.a, collection, keys, depth + 1), EQUALITIES_OK); (void)left;
        Take(right, equalities_result, Equalities(p, data.b, collection, keys, depth + 1), EQUALITIES_OK); (void)right;
    } else if (kind == IR_EQUAL) {
        Check(HasRef(ir, data.a, IR_EXPRESSION) && HasRef(ir, data.b, IR_EXPRESSION), "invalid equality operands");
        ir_type left = ir->expressions.types.ptr[data.a.index], right = ir->expressions.types.ptr[data.b.index];
        /* The emitter uses IS NOT DISTINCT FROM for optional operands. Do not
         * assume that predicate will constrain a B-tree equality key. */
        if (!left.optional && !right.optional && left.kind != IR_NULL && right.kind != IR_NULL) {
            ir_ref column;
            if ((PhysicalColumn(ir, data.a, collection, &column) && ExternalValue(ir, data.b, 0)) ||
                (PhysicalColumn(ir, data.b, collection, &column) && ExternalValue(ir, data.a, 0))) {
                Take(added, add_key_result, AddKey(p, keys, column), ADD_KEY_OK); (void)added;
            }
        }
    }
    return (equalities_result){EQUALITIES_OK};
}
static walk_expression_result WalkExpression(index_plan *p, ir_ref expression, usize depth);
static lookup_result Lookup(index_plan *p, u32 collection, ir_ref predicate, ir_ref join_key, bool ordered, usize depth)
{
    lookup_result failure = {LOOKUP_ERROR};
    Check(depth <= 256 && collection < p->ir->collections.data.len, "invalid index lookup collection");
    ir_ref storage[INDEX_KEY_LIMIT];
    lookup_keys keys = {{storage, INDEX_KEY_LIMIT}, 0};
    if (join_key.table != IR_NONE) { Take(added, add_key_result, AddKey(p, &keys, join_key), ADD_KEY_OK); (void)added; }
    usize fixed = keys.used;
    Take(equalities, equalities_result, Equalities(p, predicate, collection, &keys, 0), EQUALITIES_OK); (void)equalities;
    /* Still inspect nested subqueries when the outer row is a primary-key lookup. */
    Take(nested, walk_expression_result, WalkExpression(p, predicate, depth + 1), WALK_EXPRESSION_OK); (void)nested;
    ir_ref_slice equality_keys = {keys.storage.ptr, keys.used};
    SliceFor(equality_keys, i) { if (Primary(p->ir, equality_keys.ptr[i])) return (lookup_result){LOOKUP_OK}; }
    if (!keys.used) return (lookup_result){LOOKUP_OK}; /* Unfiltered ordered scan uses the primary key. */
    /* Keep correlation key first; normalize other equality keys by decoded name
     * so reordering filter properties does not generate new physical indexes. */
    SliceFor(equality_keys, i) {
        if (i <= fixed) continue;
        ir_ref key = equality_keys.ptr[i];
        usize j = i;
        while (j > fixed && Before(Name(p->ir, key), Name(p->ir, equality_keys.ptr[j - 1]))) {
            equality_keys.ptr[j] = equality_keys.ptr[j - 1]; --j;
        }
        equality_keys.ptr[j] = key;
    }
    if (ordered) {
        ir_ref id = IdField(p->ir, collection);
        Check(id.table == IR_FIELD, "lookup collection has no primary key");
        Take(added, add_key_result, AddKey(p, &keys, id), ADD_KEY_OK); (void)added;
    }
    /* Deduplicate exact candidates immediately, before spending plan capacity. */
    index_candidate_slice existing = {p->candidates.ptr, p->candidates_used};
    SliceFor(existing, i) {
        index_candidate item = existing.ptr[i];
        if (item.collection != collection || item.keys.count != keys.used) continue;
        bool same = true;
        ir_ref_slice values = {keys.storage.ptr, keys.used};
        SliceFor(values, j) { if (!SameRef(values.ptr[j], p->keys.ptr[item.keys.start + j])) { same = false; break; } }
        if (same) return (lookup_result){LOOKUP_OK};
    }
    Check(p->candidates_used < p->candidates.len && keys.used <= p->keys.len - p->keys_used, "index plan capacity exceeded");
    ir_range range = {(u32)p->keys_used, (u32)keys.used};
    ir_ref_slice values = {keys.storage.ptr, keys.used};
    SliceFor(values, i) { p->keys.ptr[p->keys_used++] = values.ptr[i]; }
    p->candidates.ptr[p->candidates_used++] = (index_candidate){.collection = collection, .keys = range, .active = true};
    return (lookup_result){LOOKUP_OK};
}
static walk_expression_result WalkExpression(index_plan *p, ir_ref expression, usize depth)
{
    walk_expression_result failure = {WALK_EXPRESSION_ERROR};
    if (expression.table == IR_NONE) return (walk_expression_result){WALK_EXPRESSION_OK};
    const declaration_ir *ir = p->ir;
    Check(depth <= 256 && HasRef(ir, expression, IR_EXPRESSION), "invalid index expression traversal");
    ir_data data = ir->expressions.data.ptr[expression.index];
    switch ((ir_expression_kind)ir->expressions.kinds.ptr[expression.index]) {
    case IR_EXISTS: {
        Check(HasRef(ir, data.a, IR_COLLECTION), "invalid EXISTS index source");
        Take(lookup, lookup_result, Lookup(p, data.a.index, data.b, (ir_ref){0}, false, depth + 1), LOOKUP_OK); (void)lookup; break;
    }
    case IR_LOAD_BINDING:
        if (HasRef(ir, data.a, IR_BINDING) && ir->bindings.kinds.ptr[data.a.index] == IR_BINDING_LOCAL) {
            Take(value, walk_expression_result, WalkExpression(p, ir->bindings.data.ptr[data.a.index].b, depth + 1), WALK_EXPRESSION_OK); (void)value;
        }
        break;
    case IR_EQUAL: case IR_AND: {
        Take(right, walk_expression_result, WalkExpression(p, data.b, depth + 1), WALK_EXPRESSION_OK); (void)right;
        Take(left, walk_expression_result, WalkExpression(p, data.a, depth + 1), WALK_EXPRESSION_OK); (void)left; break;
    }
    case IR_RELATION_ACCESS:
        Check(HasRef(ir, data.b, IR_RELATIONSHIP), "invalid navigated relationship");
        if (ir->relationships.kinds.ptr[data.b.index] & IR_RELATION_REVERSE) {
            ir_data relationship = ir->relationships.data.ptr[data.b.index];
            Check(HasRef(ir, relationship.c, IR_RELATIONSHIP), "missing reverse relationship key");
            Take(lookup, lookup_result, Lookup(p, relationship.b.index, (ir_ref){0}, relationship.c, false, depth + 1), LOOKUP_OK); (void)lookup;
        }
        /* fall through */
    case IR_FIELD_ACCESS: case IR_PARSE_PARAMETER: {
        Take(value, walk_expression_result, WalkExpression(p, data.a, depth + 1), WALK_EXPRESSION_OK); (void)value; break;
    }
    default: break;
    }
    return (walk_expression_result){WALK_EXPRESSION_OK};
}
static walk_selections_result WalkSelections(index_plan *p, ir_range range, usize depth)
{
    walk_selections_result failure = {WALK_SELECTIONS_ERROR};
    const declaration_ir *ir = p->ir;
    Check(depth <= 256 && range.start <= ir->extra.len && range.count <= ir->extra.len - range.start, "invalid index selection range");
    ir_ref_slice selections = {range.count ? ir->extra.ptr + range.start : NULL, range.count};
    SliceFor(selections, i) {
        Check(HasRef(ir, selections.ptr[i], IR_SELECTION), "invalid index selection");
        ir_data selection = ir->selections.data.ptr[selections.ptr[i].index];
        Check(HasRef(ir, selection.a, IR_RELATIONSHIP), "invalid selected relationship for index");
        ir_data relationship = ir->relationships.data.ptr[selection.a.index];
        u32 flags = ir->relationships.kinds.ptr[selection.a.index];
        ir_ref join_key = flags & IR_RELATION_REVERSE ? relationship.c : IdField(ir, relationship.b.index);
        Check(HasRef(ir, join_key, IR_FIELD) || HasRef(ir, join_key, IR_RELATIONSHIP), "invalid relationship join key");
        Take(lookup, lookup_result, Lookup(p, relationship.b.index, selection.b, join_key, (flags & IR_RELATION_MANY) != 0, depth + 1), LOOKUP_OK); (void)lookup;
        Take(children, walk_selections_result, WalkSelections(p, selection.items, depth + 1), WALK_SELECTIONS_OK); (void)children;
    }
    return (walk_selections_result){WALK_SELECTIONS_OK};
}
#undef Check
#undef Take
static bool Covers(const index_plan *a, index_candidate broad, const index_plan *b, index_candidate narrow)
{
    if (broad.keys.count < narrow.keys.count || !Equal(Name(a->ir, (ir_ref){IR_COLLECTION, broad.collection}), Name(b->ir, (ir_ref){IR_COLLECTION, narrow.collection}))) return false;
    ir_ref_slice keys = {b->keys.ptr + narrow.keys.start, narrow.keys.count};
    SliceFor(keys, i) {
        if (!Equal(Name(a->ir, a->keys.ptr[broad.keys.start + i]), Name(b->ir, keys.ptr[i]))) return false;
    }
    return true;
}
static u64 HashPart(u64 hash, string_slice text)
{
    /* Length delimiters make sequences of physical names unambiguous. */
    u64 length = text.len;
    for (u32 i = 0; i < 8; ++i) { hash = (hash ^ (length & 255)) * UINT64_C(1099511628211); length >>= 8; }
    SliceFor(text, i) { hash = (hash ^ (u8)text.ptr[i]) * UINT64_C(1099511628211); }
    return hash;
}
static void PlanDestroy(index_plan *p) { free(p->candidates.ptr); free(p->keys.ptr); }
typedef enum plan_build_tag { PLAN_BUILD_OK, PLAN_BUILD_NO_SPACE, PLAN_BUILD_UNSUPPORTED } plan_build_tag;
typedef struct plan_build_result { plan_build_tag tag; } plan_build_result;
static plan_build_result PlanBuild(index_plan *p, const declaration_ir *ir)
{
    *p = (index_plan){.ir = ir};
    if (!ir) return (plan_build_result){PLAN_BUILD_OK};
    usize capacity = ir->queries.data.len;
    if (capacity > UINT32_MAX || ir->selections.data.len > UINT32_MAX - capacity) return (plan_build_result){PLAN_BUILD_NO_SPACE};
    capacity += ir->selections.data.len;
    if (ir->expressions.data.len >= UINT32_MAX - capacity) return (plan_build_result){PLAN_BUILD_NO_SPACE};
    capacity += ir->expressions.data.len + 1;
    if (capacity > UINT32_MAX / INDEX_KEY_LIMIT || capacity > SIZE_MAX / sizeof(index_candidate) || capacity > SIZE_MAX / INDEX_KEY_LIMIT / sizeof(ir_ref)) return (plan_build_result){PLAN_BUILD_NO_SPACE};
    p->candidates = (index_candidate_slice){calloc(capacity, sizeof(index_candidate)), capacity};
    p->keys = (ir_ref_slice){calloc(capacity * INDEX_KEY_LIMIT, sizeof(ir_ref)), capacity * INDEX_KEY_LIMIT};
    if (!p->candidates.ptr || !p->keys.ptr) return (plan_build_result){PLAN_BUILD_NO_SPACE};
    SliceFor(ir->queries.data, i) {
        ir_data query = ir->queries.data.ptr[i];
        p->offset = ir->queries.sources.ptr[i];
        if (!HasRef(ir, query.b, IR_COLLECTION)) { p->error = StringSlice("invalid query index source"); return (plan_build_result){PLAN_BUILD_UNSUPPORTED}; }
        if (Lookup(p, query.b.index, query.c, (ir_ref){0}, true, 0).tag != LOOKUP_OK || WalkSelections(p, query.items, 0).tag != WALK_SELECTIONS_OK)
            return (plan_build_result){PLAN_BUILD_UNSUPPORTED};
    }
    index_candidate_slice candidates = {p->candidates.ptr, p->candidates_used};
    SliceFor(candidates, i) {
        index_candidate *item = &candidates.ptr[i];
        item->signature = HashPart(UINT64_C(14695981039346656037), Name(ir, (ir_ref){IR_COLLECTION, item->collection}));
        ir_ref_slice keys = {p->keys.ptr + item->keys.start, item->keys.count};
        SliceFor(keys, k) { item->signature = HashPart(item->signature, Name(ir, keys.ptr[k])); }
        SliceFor(candidates, j) {
            if (i != j && candidates.ptr[j].keys.count > item->keys.count && Covers(p, candidates.ptr[j], p, *item)) item->active = false;
        }
    }
    return (plan_build_result){PLAN_BUILD_OK};
}
static bool Reserved(string_slice name)
{
    string_slice prefix = StringSlice("query_idx_");
    if (name.len < prefix.len) return false;
    SliceFor(prefix, i) { if (name.ptr[i] != prefix.ptr[i]) return false; }
    return true;
}
index_physical_result IndexPhysical(const declaration_ir *ir, physical_schema *schema)
{
    index_plan plan = {0};
    plan_build_result built = PlanBuild(&plan,ir);
    index_physical_result result = {.tag = INDEX_PHYSICAL_OK};
    if (built.tag != PLAN_BUILD_OK) {
        result = (index_physical_result){built.tag == PLAN_BUILD_NO_SPACE ? INDEX_PHYSICAL_NO_SPACE : INDEX_PHYSICAL_UNSUPPORTED, plan.error}; goto done_physical;
    }
    SliceFor(ir->collections.names,j) {
        if (Reserved(ir->strings.ptr[ir->collections.names.ptr[j]])) { result = (index_physical_result){INDEX_PHYSICAL_UNSUPPORTED,StringSlice("query_idx_ is reserved for indexes")}; goto done_physical; }
    }
    index_candidate_slice candidates = {plan.candidates.ptr,plan.candidates_used};
    SliceFor(candidates,i) {
        index_candidate item = candidates.ptr[i];
        if (!item.active) continue;
        char name_storage[32];
        i32 count = snprintf(name_storage,sizeof(name_storage),"query_idx_%016llx",(unsigned long long)item.signature);
        if (count < 0 || (usize)count >= sizeof(name_storage)) { result.tag = INDEX_PHYSICAL_UNSUPPORTED; goto done_physical; }
        string_slice name = {name_storage,(usize)count};
        if (PhysicalAdd(schema,(physical_record){.kind=PHYSICAL_INDEX,.owner=Name(ir,(ir_ref){IR_COLLECTION,item.collection}),.name=name}).tag != PHYSICAL_ADD_OK) { result.tag=INDEX_PHYSICAL_NO_SPACE; goto done_physical; }
        ir_ref_slice keys = {plan.keys.ptr+item.keys.start,item.keys.count};
        SliceFor(keys,j) {
            if (PhysicalAdd(schema,(physical_record){.kind=PHYSICAL_INDEX_KEY,.detail=(u32)j,.owner=name,.name=Name(ir,keys.ptr[j])}).tag != PHYSICAL_ADD_OK) { result.tag=INDEX_PHYSICAL_NO_SPACE; goto done_physical; }
        }
    }
done_physical:
    PlanDestroy(&plan); return result;
}
