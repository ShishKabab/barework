# Application compiler

From `news-platform/`, build with `./build.sh`, then run:

```bash
./build/app-compiler build
./build/app-compiler version
```

These are the only two commands. Both read `app.cfg` in the current project
directory; an optional final argument selects a different project directory.
`build.sh` compiles the native tools; `app-compiler build` compiles the application.
Successful application builds produce `build/app-ast.txt`, `build/app-ir.txt`,
`build/schema`, `build/migration.sql`, and `src/generated/app-routes.{h,c}`. Errors report
a diagnostic and exit unsuccessfully. The server embeds the generated routes
in its connection guest; it does not interpret configuration at runtime.
Sequential route queries also generate broker policy and business continuations;
handwritten C handlers implement response construction (see below).

## Build and version lifecycle

Every configuration requires exactly one top-level `version(integer)` call,
with a value from 1 to 4294967295. The first version is 1. With recorded history,
the working version must be the latest recorded version or its successor.

`build` lowers resolved IR into a desired physical schema, then diffs that against
the latest recorded `schema`. With no history it creates the initial schema.
With the same version, unchanged physical definitions produce an empty migration.
It permits edits while the working version still equals the latest recorded
version; increment `version()` before recording those edits. New application
versions create fresh procedure names even when their queries are unchanged.
Tables, columns, and index keys compare by physical names and definitions, not IR
IDs. Procedure bodies compare their generated SQL and argument signatures.

`version` performs the same build and records its exact input bytes, resulting
physical schema, and migration:

```text
versions/0001/app.cfg
versions/0001/schema
versions/0001/migration.sql
versions/0002/app.cfg
versions/0002/schema
versions/0002/migration.sql
```

Recording identical source at the latest recorded version succeeds without
rewriting any recorded file. Different source at that version fails. Version
numbers cannot skip or go backwards. Numbered history directories must be complete
and contiguous; directory names are padded to at least four digits. Legacy
snapshots without `schema` are rejected and require an explicit upgrade; the
compiler does not guess historical database state by recompiling old sources.

Both commands take a project lock in `build/.app-compiler.lock`. They generate into
owned staging directories and clean them on success, ordinary failure, or handled
interrupts. A new version directory is published atomically with a no-overwrite
rename after syncing all three files; existing recorded migrations are never regenerated
in place. Power loss/SIGKILL can leave an unpublished hidden staging directory;
it is ignored as history. Build files are replaced individually only after all
generation succeeds. A filesystem failure during replacement is reported and
requires a rebuild; no partial version directory is published.

Commit recorded versions to Git. A future production deployment must consume a
recorded directory and its frozen migration, not development outputs. Deployment
and database-side applied-version tracking are not implemented here. After a
version is recorded, the next build compares against it, so its development
migration becomes empty while the recorded migration remains unchanged.

### Physical schemas and migration planning

The physical model owns tables, columns (including primary-key identity and
nullability), B-tree indexes with ordered keys, versioned procedure bodies and
signatures, and procedure-to-table dependencies. It is independent of IR storage.
The resulting schema includes retained historical procedures and indexes, even
when their originating queries no longer appear in the current application.

Supported differences are new tables, nullable-column additions, and column/table
removals without retained-procedure dependencies. Removal drops affected indexes
first. Required columns on existing tables need a default/backfill policy and are
rejected. Type, nullability, and identity changes are also rejected for now.
Renames are never inferred. Constraints not yet lowered into SQL do not change the
physical schema or gain enforcement through this diff.

Older versioned procedures remain callable. Their table dependencies conservatively
protect every column of referenced tables, including whole-row JSON projections.
Removing such a table or column fails until explicit version retirement exists.
During same-version development, changed/removed procedures are dropped before
schema changes, then new definitions are created. There is no `CASCADE`.
Adding columns can affect the whole-row JSON returned by retained procedures;
the current protocol does not freeze each old response's field set.

`schema` uses the versioned `physical-schema 1` format: an application version,
followed by records with kind, detail, and three decimal byte lengths, then their
owner/name/value bytes. It is portable, length-delimited, and validated on load;
it does not serialize C structs or parse SQL to recover structure. It is a trusted
compiler artifact containing generated SQL bodies, and should be committed with
the other version files rather than edited manually. Unsupported format versions,
broken references, duplicate identities, and truncated snapshots are rejected.
The compiler diffs recorded state, not a live database; migrations must be applied
in order, and out-of-band database changes are outside this workflow.

## Current syntax

Files contain `version(integer)` and semicolon-terminated `collection(...)`, `route(...)`, and
`query(...)` calls. Supported expressions and statements cover `app.cfg`:

- Single- or double-quoted strings, nonnegative decimal numbers, booleans, null,
  and identifiers.
- Objects, arrays, nested calls, and dotted member access.
- Parenthesized callback parameters followed by `=>` and a statement block.
- `let name = expression;`, `return expression;`, `return;`, and expression statements.
- Trailing commas, whitespace, `//` comments, and `/* ... */` comments.

Braces in expression position mean an object. Callback bodies require
`() => { ... }` or named parameters such as `(context) => { ... }`. Grouped
expressions, operators, indexing, assignment expressions, and other JavaScript
syntax are not implemented. This is a declaration language, not a JavaScript
interpreter. Identifiers use ASCII letters, digits, `_`, and `$`, with no initial
digit. Escapes are validated but not decoded; literals remain source spans.
For example, `'true'` remains a string, distinct from `true`.

Fields, relationships, constraints, routes, and query options are represented
syntactically in the AST. The separate IR resolution pass described below
validates names, argument counts, types, references, and scopes. Top-level calls are limited to the three
declaration names; nested helpers remain ordinary calls. Parsing stops at the
first error. A nesting limit of 128 bounds recursive parsing's stack usage.

## Storage and lifetime

The design follows Mitchell Hashimoto's descriptions of the Zig
[tokenizer](https://mitchellh.com/zig/tokenizer) and
[parser](https://mitchellh.com/zig/parser): source offsets, separate field arrays,
index-based nodes, and extra storage for lists instead of allocated tree objects.

`declaration/lexer.h` exposes an allocation-free next-token operation over a byte
slice without a terminating sentinel. Tokens contain a tag and start/end source
offsets; EOF is an ordinary token. Failures carry function-specific result tags
and source locations.

`declaration/parser.h` exposes `DeclarationParse` and `DeclarationAstDestroy`.
The AST borrows the source and owns separate columns:

- Token tags (`u8`), starts (`u32`), and ends (`u32`).
- Node tags (`u8`), main-token indices (`u32`), and child ranges (two `u32`s).
- Extra data (`u32`), holding ordered child-node indices.

Root index zero is the file node. Links are indices. Leaves have empty child
ranges; lists live in extra data. Child meanings for each kind are documented
beside `declaration_node_data`. Calls have a callee and an arguments node;
callbacks have parameters and a body. Collection, route, and query nodes retain
the call layout.

The parser counts tokens, allocates columns once, and fills them with a second
lexer pass. Node/scratch/extra capacities are bounded from the token count.
There is no allocation per token, node, or string. A scratch stack collects
children while nested constructs append their own lists. Final slice lengths
expose only used entries; unused capacity is retained until destruction.

Keep source bytes alive and unchanged until after destroying the AST. Failed
parses release allocations and leave an empty AST. The command-line walker uses
an explicit stack so long member/call chains do not cause recursive printing.

## Resolved application IR

Resolve the file and write the dump to `build/app-ir.txt`:

```bash
./build/app-compiler build
```

`DeclarationResolve` builds an owned `declaration_ir` with independent string and
literal lifetimes. Resolution errors report the original source location and stop before
producing a partial dump. No request execution or database access is implemented.

The resolver registers collection and route names first, builds all fields and
relationships, then resolves constraints, callback bodies, and queries. Collection
references may point forward in the file. Collection and route names have separate
namespaces. Query names are scoped to their route, or to file scope for top-level
queries. Local bindings are resolved in declaration order; a callback parameter
and a local cannot have the same name.

The IR has separate tables for collections, fields, relationships, routes,
queries, constraints, bindings, expressions, selections, and statements. Each
table stores names, source offsets, operation kinds, types, and payloads in
separate columns. A reference is a table tag plus a `u32` index, preventing a
collection reference from being interpreted as a field reference. Variable-length
lists contain these resolved references in extra data. `ir.h` documents each
payload layout. Strings are decoded and interned once; symbol access in the IR
uses resolved references, not runtime string lookup.

Schema rules in this implementation:

- Every collection has an implicit nominal `id` type tied to that collection.
- Field types are `string`, `boolean`, `number`, and `timestamp`; `optional`
  requires a boolean value.
- `singleChildOf` and `multipleChildOf` create a required forward reference.
  `alias` renames that reference. The reverse name is the child's collection
  name, with zero-or-one and many cardinality respectively. Duplicate members
  and ambiguous generated reverse names are rejected.
- `connects(a, b)` creates two required references and their many-valued reverses.
  It does not implicitly add a composite uniqueness constraint.
- `unique` resolves its target members. `constraint` is filtered uniqueness and
  requires both target members and a filter. `checkUpdate` resolves a callback
  that must end with a boolean return. Checks are descriptions for later execution,
  not an implemented transactional enforcement mechanism.

Query filters become typed equality expressions combined with `and`. A
relationship key in a filter compares its target's nominal ID; comparing a
many-valued relationship directly is rejected. Unknown fields, mismatched ID
types, duplicate keys/options, and unknown helpers produce resolution errors.
Direct text route parameters compared with a non-string scalar field become
explicit `parse_parameter_checked` expressions. Conversion failure must become an
invalid-parameter outcome when execution is implemented; it must never become an
unchecked cast. Comparing null requires an optional field.

Selections resolve relationship IDs, target collection scopes, nested filters,
and nested selections. Their cardinality and optionality are explicit. Query
results are record sets; this pass does not infer a single result from ID filters.

Callback context members become operations: `context.route.id` resolves to a
specific route-parameter binding; `context.new.user.id` resolves to a new-record
operation, relationship ID, and field ID. `exists` resolves its collection and
predicate and has boolean result type. Ordinary bindings become load operations.
Routes retain the complete path string plus ordered literal/parameter bindings,
and resolved body statements. Full path spelling, including a trailing slash,
is retained and used by the generated matcher described below.

### Generated route matching and dispatch

`build` and `version` generate `src/generated/app-routes.h` and `src/generated/app-routes.c`.
All project-generated C sources and headers belong in `src/generated/` and should
be committed with the source that produces them. Compiler dumps, development SQL,
schema output, binaries, and toolchain caches remain in `build/`. `build.sh` builds the compiler, generates these
files, then builds the guests and native host together. Each deployment runs
one application and guest ABI; route tags are local to that build, with no
per-request application version or cross-version dispatch support. Recorded
schema history remains unchanged by this runtime contract.

Business route interfaces will follow the same layout: generated headers under
`src/generated/`, with handwritten implementations outside it (for example,
`src/business/route_profile.c`). The compiler must never overwrite handwritten
handlers. Order them by execution flow: `RouteProfile_Start`, then
`RouteProfile_GetProfile_Result`. Leaf handlers return the final response;
there is no separate finish handler. This per-route business interface is planned;
the current business guest still contains the response stub.

Matching is case-sensitive over the complete raw path, excluding the query
string. A parameter consumes one nonempty slash-delimited segment. Encoded bytes
remain encoded, including `%2F`; there is no decoding or path normalization.
Trailing slashes are significant. Route paths cannot contain query or fragment
delimiters. Route and capture names must be identifiers; generated names use
prefixes so C keywords cannot collide with generated identifiers.

Among complete matches, a literal wins over a parameter at the first differing
segment, from left to right. Thus `/profile/new` wins over `/profile/:id`.
If a literal branch fails later, parameter alternatives remain eligible:
`/a/:middle/end` wins over `/:first/b/:last` for `/a/b/end`, while the latter
matches `/a/b/fallback`. Declaration order has no effect. Identical matching
patterns, including patterns differing only in parameter names, fail compilation
with both source offsets. The compiler orders complete candidate blocks by this
precedence; runtime code uses branches and slice comparisons, with no route table
interpreter or regex engine.

Each route with parameters has a capture struct containing named `byte_range` fields.
`app_route_payload` contains a fixed-width route tag and a union of these structs;
inspect the tag before accessing a variant. `RouteMatch` accepts the parsed HTTP
message and request bump arena. It allocates only after finding a complete match,
returning a function-specific result for match, no match, invalid input, or no
space. Failed candidates allocate nothing.

Generated code uses named route tags, ordinary readable C string literals, and
explicit candidate blocks. Shared helpers in `route.h` handle path extraction,
envelope copying, and bounds validation; checked narrowing and object-byte views
stay at those boundaries rather than being repeated in each generated route.

The allocation contains a `route_request` root, tagged capture payload, and a
copy of the HTTP message. Capture `index` and `len` values refer to bytes relative
to the complete dispatch message. The nested HTTP message retains its own root
and relative offsets. `RouteRequestRead` checks the layout, tag, capture bounds,
and path containment before exposing the borrowed HTTP slice. Resolve capture
ranges against the complete message with `ConstByteRangeResolve`. Native pointers
never cross guest boundaries. Both structures use the deployment's little-endian
ABI, not a portable serialization format.

The connection guest handles unmatched paths as 404 and unsupported methods as
405, preserving HEAD and keep-alive behavior. Matched requests travel through
the existing scheduler to the business stub, which validates the envelope and
reads its route tag. The stub returns 204 for the declared `empty` route and
the existing greeting for other routes. Route selection does not authorize any
operation; broker policies and route-body execution remain future work.

Run `tests/routes.sh` after building the compiler for generated-C tests, including
precedence under reordered declarations, fallback, relocation, malformed payloads,
allocation failure, duplicate diagnostics, and empty route sets. `tests/run.sh`
also exercises connection/business Wasm dispatch and pipelining.

The dump lists every table entry with its ID, optional diagnostic name, source
byte offset, type, and resolved references. Expression operands `a` and `b` follow
the operation layouts in `ir_expression_kind`. Initial capacities are conservative
bounds based on AST size and path segments; exposed slice lengths contain only
used entries. No unit or integration test files were added. Verification uses
the proposal and direct command invocations in a temporary project that is
removed afterwards.

## PostgreSQL migrations

Generate the initial schema from the resolved IR:

```bash
./build.sh
./build/app-compiler build
```

`DeclarationSql` consumes the current and optional previous IR. With no previous
version it emits one `CREATE TABLE` per collection in declaration order. Otherwise
it emits the supported physical-schema differences. Query-derived indexes follow the table
statements, then query procedure definitions. Unsupported schema changes and query expressions are rejected
before writing. The development migration is `build/migration.sql`; `version`
freezes a copy in `versions/XXXX/migration.sql`. Neither command connects to a database.

| IR member | PostgreSQL column |
| --- | --- |
| Implicit collection ID | `bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY` |
| `string` | `text` |
| `boolean` | `boolean` |
| `number` | `double precision` |
| `timestamp` | `timestamp with time zone` |
| Forward relationship | `bigint`, named by the resolved alias or target name |

Required columns have `NOT NULL`; optional fields permit null. Reverse
relationships do not create columns. Names retain their original spelling and
case, including `$session` and camelCase names. All identifiers are quoted, with
embedded quotes doubled. The generator rejects invalid UTF-8, NUL, empty names,
identifiers over PostgreSQL's default 63-byte limit, and reserved system-column
names. It validates the schema before emitting any SQL; output I/O failure is
reported separately and may leave partial output in the caller's stream.

Table creation includes inline primary keys and nullability. Foreign keys,
single-child uniqueness, declared unique/filtered
constraints, update checks, and database-side migration execution remain unimplemented.
In particular, the file establishes storage but does not enforce all IR rules yet.
Identity integers are the initial storage policy for nominal collection IDs; the
IR continues to distinguish IDs belonging to different collections.

SQL spelling follows PostgreSQL's [CREATE TABLE syntax](https://www.postgresql.org/docs/current/sql-createtable.html)
and [quoted-identifier rules](https://www.postgresql.org/docs/current/sql-syntax-lexical.html).

### Query-derived indexes

The compiler derives nonunique B-tree indexes from resolved query filters and
relationship lookups, including nested selections. Correlation keys come first,
followed by required scalar equality keys in name order. Array-producing lookups
append `id` to support the generated ordering. Primary-key lookups and unfiltered
ordered scans use the existing primary-key index. Optional/null-safe comparisons
are conservatively excluded from equality keys. Declared constraints do not
participate in this pass.

Identical candidates are deduplicated; an index covered by another candidate's
leading columns is omitted. The current application generates `profile(user)`,
`email(user, isPrimary, id)`, and `timelineEvent(timeline, id)`. This is a structural
heuristic, not a cost-based optimizer: actual planner choices and performance
still require representative data and `EXPLAIN (ANALYZE, BUFFERS)`.

Index names use a stable fingerprint of the table and ordered column names,
independent of query names, IR IDs, and application versions. The `query_idx_`
namespace is reserved for the compiler. Migrations compare definitions with all
indexes in the recorded physical schema, including historical ones. Matching or
covering indexes are reused regardless of name. An index survives query removal
and is dropped only when its table or a key column is removed. A query that
disappears and later returns therefore does not recreate its existing index.
New indexes use plain `CREATE INDEX`; there is no name-only `IF NOT EXISTS`
fallback. Conflicting names with different definitions are rejected.
Index creation is ordinary transactional creation, not `CONCURRENTLY`.

See PostgreSQL's [multicolumn indexes](https://www.postgresql.org/docs/current/indexes-multicolumn.html)
and [CREATE INDEX](https://www.postgresql.org/docs/current/sql-createindex.html).

## Versioned query procedures

Each resolved query becomes a PostgreSQL procedure. A route-scoped query uses
`<route>_<query>_v<version>`; a file-scoped query uses `<query>_v<version>`. Names
are quoted and preserve case. The compiler rejects names exceeding 63 bytes and
collisions caused by combining scope and query names; it never truncates them.
For the proposal, the migration contains:

```sql
CREATE PROCEDURE "profile_getProfile_v1"(IN "p_id" text, OUT "result" jsonb)
-- ...
CREATE PROCEDURE "timeline_getTimeline_v1"(IN "p_id" text, OUT "result" jsonb)
-- ...
```

Only route parameters referenced by the query or its nested selections become
inputs. Their order follows the route's path declaration, and their names gain a
`p_` prefix. Inputs are text because the route provides text; IR checked conversions
become SQL casts to bigint, boolean, double precision, or timestamp with time zone.
Invalid conversions raise PostgreSQL errors. Parameters are referenced with `$1`,
`$2`, etc.; no runtime input is interpolated into SQL text. Scalar local bindings
are inlined from their resolved initializer expressions. Unsupported dependencies,
such as using another query result as a scalar, stop generation.

Procedures use `LANGUAGE SQL`, `SECURITY INVOKER`, and `BEGIN ATOMIC` bodies,
requiring PostgreSQL 14 or newer. The parsed body avoids placing user-authored
literals inside an outer dollar-quoted string. Identifiers and literals are escaped
separately, and built-in JSON functions are qualified with `pg_catalog`.

The result is one JSONB array of records, ordered by primary key; an empty root
result is `[]`. Scalar columns and forward relationship keys are included. A
selected relationship replaces its key with the nested object or array. To-many
selections return arrays (including `[]`); to-one selections return an object or
`null`. Duplicate rows on a to-one relationship raise a cardinality error rather
than being silently discarded. This matters until single-child constraints are
implemented. Nested selections and their filters compile to correlated subqueries
inside the same SQL statement, so the caller makes one database call. Optional
comparisons use null-safe equality; required scalar comparisons use ordinary `=`.
`exists` lowers to a SQL `EXISTS` predicate.

For example, supply the input and an output placeholder:

```sql
CALL "profile_getProfile_v1"('123', NULL);
CALL "timeline_getTimeline_v1"('456', NULL);
```

The output row has a `result` column containing the JSONB value. Calls execute with
the caller's database privileges; this step does not implement broker authorization
or native runtime bindings.

A new application version emits every active query under its new suffix and
retains old procedures for older application versions. A version-only bump therefore
has a nonempty migration when queries exist. During same-version development, the
compiler compares generated procedure bodies and signatures with the recorded baseline:
unchanged queries emit nothing, changed queries are dropped and recreated, and
removed queries are dropped. These development deltas target the recorded baseline,
not arbitrary intermediate database states. The `version` command still refuses
to alter a recorded version, and recorded SQL files remain untouched.

PostgreSQL documents [SQL procedure bodies](https://www.postgresql.org/docs/current/sql-createprocedure.html)
and [CALL output parameters](https://www.postgresql.org/docs/current/sql-call.html).

## Generated workflows and handwritten handlers

`app-compiler build` writes all project-generated `.c/.h` files to
`src/generated/`: `app-routes`, `app-queries`, `app-business`, `app-broker`, and
one `route_<name>.h` per route. These files belong in version control. Route
implementations remain handwritten in `src/business/route_<name>.c`; generation
never writes them. Functions appear in execution order: `RouteProfile_Start`,
then `RouteProfile_GetProfile_Result`. Leaf handlers return response descriptors;
there is no separate finish handler. The host's broker-completion handshake is
an internal lifecycle check, not a business handler.

Names derive from route and query declarations. For example, `profile` and
`getProfile` produce `BROKER_OPERATION_PROFILE_GET_PROFILE`,
`profile_get_profile_result`, and `RouteProfile_GetProfile_Result`. Generated
state retains immutable query-message slices in the business request arena.
Cross-instance records contain root-relative byte ranges and explicit row counts,
never pointers. Pointers in handler contexts and shape metadata remain local to
one instance. Optional scalars have explicit presence tags.

The current workflow compiler supports at most eight sequential named reads per
route, each with a root `id == context.route.<capture>` filter. Unsupported
statements, filters, types, and normalized C-name collisions fail compilation.
Nested selections define typed result shapes; the fake backend supplies synthetic
records rather than executing SQL predicates. Handwritten handlers may finish
at the final result or on a query failure; the broker rejects skipped successful
operations, duplicate operations, wrong-route operations, and out-of-order results.
The broker derives query arguments from its own request copy; business code
cannot supply SQL or replace those IDs.

For the synthetic backend, ID `0` returns an empty array, `2` omits optional
values, `13` returns a backend failure (HTTP 503), and other valid IDs return
populated records. Invalid numeric captures return HTTP 400. Profile and timeline
return JSON; `/profile-summary/:id` exercises two queries while retaining the first
result. Authentication, sessions, real database execution, writes, arbitrary
control flow, and query-result-dependent parameters remain future increments.
These synthetic anonymous reads are not a production authorization mechanism.

The host and all guests share one deployment's generated interfaces and ABI.
Recorded schema versions govern deployment migrations; runtime workflows do not
negotiate or support concurrent application versions.
