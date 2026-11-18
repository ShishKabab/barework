# Secure WebAssembly Server Architecture

Technical design notes and decision record | 8 November 2026

A small native C transport host orchestrates isolated WebAssembly compartments for TLS, HTTP parsing, request business logic, and resource authorization. An offline compiler produces typed operations, asynchronous state machines, SQL, and independent access-control policies from a shared workflow definition.

This is a design exploration, not a validated implementation. Security properties depend on the host ABI, isolation configuration, parser correctness, and broker enforcement. Performance figures referenced below are historical external benchmarks or explicit estimates; none constitute a measured result for this proposed server.

Current implementation: one HTTP connection guest per connection, pinned to its
worker, with a resettable request arena. TLS will run in that same instance;
TLS, signing, and broker protocols are not implemented yet. Generated route
matching runs in the connection guest and dispatches tagged captures to a
business stub; route callback execution remains unimplemented. See
[workers and connection protocol](workers.md) for the implemented boundary.

# 1 Goals and guiding principles

- Serve many thousands of concurrent, largely I/O-bound requests on a few CPU cores with predictable p99 latency and low overhead.

- Keep the native privileged surface minimal: accept connections, manage asynchronous I/O, schedule WASM calls, and mediate strictly bounded cross-compartment capabilities.

- Place parsing and business logic inside WASM rather than trusting memory-unsafe C with application-level inputs.

- Use short-lived instances, explicit ownership, request-local memory, and destructive teardown as security boundaries.

- Compile application and authorization workflows offline into efficient code with minimal runtime interpretation.

- Keep PostgreSQL as a mature execution engine while exploring a clearer, typed, potentially procedural query language.

- Operate behind Cloudflare in production; HTTP/1.1 at the origin is acceptable initially, while browsers may use HTTP/2 or HTTP/3 to Cloudflare.

# 2 Proposed components and their lifetimes

| **Compartment**      | **Lifetime**                                    | **Authority and responsibilities**                                                                                    |
|----------------------|-------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------|
| Native C host        | Process / worker                                | TCP, io_uring, scheduling, instance pooling, transfer buffers, validated host calls; no HTTP semantics by design.     |
| Signing WASM         | Long-lived or protected signer pool             | Holds certificate private key, performs narrowly restricted handshake signing; no network or filesystem access.       |
| Connection WASM      | Per connection, pinned to one worker            | HTTP parsing and connection state; later TLS handshake/records and ephemeral secrets in the same instance. No DB/signing capability yet. |
| Authorization broker | Shared service with request-scoped policy state | Authentication, typed capability creation, operation guards, SQL execution and external-resource mediation.           |
| Business WASM        | Per request                                     | Generated async state machine, domain transformations, rendering and allowed broker calls; no unrestricted resources. |

Conceptual request path: socket → connection instance (TLS then HTTP) → broker establishes request workflow/capability → business instance ↔ broker ↔ database or external services → response through the connection instance. The signing instance is called only for authorized TLS handshake signing. Every cross-instance interaction is mediated by the host; arbitrary direct imports are not granted.

The broker itself is a sensitive trust domain. If it is long-lived, one broker memory-corruption vulnerability could affect several users even while business instances remain isolated. A small native capability table plus short-lived policy-evaluation instances is an alternative to benchmark and threat-model.

# 3 Networking and asynchronous execution

## 3.1 Native event loop

The C host initially exposes a minimal completion-oriented interface. io_uring is the preferred experiment, but epoll provides a simpler baseline. TLS integration is not automatically asynchronous merely because sockets use io_uring; the host must correctly advance TLS read/write state and handle backpressure, partial operations, cancellation, and shutdown.

Deliver completion events to guest code in batches, and accept batches of I/O commands or capability calls. This reduces host–guest transitions. Generation-tagged request/connection IDs prevent late completions from being delivered to recycled objects.

## 3.2 WASM scheduling options

The selected connection model is one Wasm instance per connection, pinned to its worker until teardown, handling HTTP and later TLS. Generation-tagged entries in a host indexable arena record worker ownership. Calls are synchronous steps on that worker; instances survive between calls while I/O is pending. Business instances now execute generated stackless continuations, with a separate request-scoped broker instance and queued synthetic query results.

Wasmtime traditional asynchronous host calls support synchronous-looking guest code but require exclusive use of a store for a suspended call and can reserve substantial per-call stack address space. Explicit stackless state machines remain attractive for 10,000+ concurrent requests. Newer Component Model asynchronous primitives may be evaluated separately.

## 3.3 Bounded streaming

Large upload bodies should flow through bounded buffers, obeying upstream/downstream backpressure. Authorized native piping between an incoming body stream and a configured storage destination can avoid copying every byte into business WASM. That is guest-bypass streaming, not necessarily true zero-copy: TLS record processing and socket buffers may still copy data.

A pipe handle must be bound to the owning request, permitted destination, size limit, cancellation state, and HTTP-body boundary. A slow destination must cause the host to stop consuming client data. No pending I/O may access an arena or guest memory after that memory is reset or reassigned.

# 4 HTTP and Cloudflare

For HTTPS, ALPN negotiates the application protocol. Offer only http/1.1 from the origin TLS endpoint, so compatible browsers and Cloudflare negotiate HTTP/1.1. Local plain HTTP ordinarily uses HTTP/1.1 without ALPN. A development certificate trusted by the browser is required for warning-free local HTTPS.

Cloudflare supports browser-side HTTP/2 and HTTP/3, and can connect to an HTTP/1.1-only origin. It may use HTTP/2 to origins that advertise it; HTTP/3 to origin was not supported in Cloudflare documentation checked during this discussion. HTTP/1.1 origin links can require more TCP/TLS connections because they lack HTTP/2 stream multiplexing.

Parsing remains guest-owned. The connection guest keeps buffered/pipelined input outside the resettable request arena and exposes one completed request at a time. The host validates and copies it before any next guest call can start another request. Network-byte consumption is distinct from HTTP-parser consumption, allowing later TLS record buffering. The current guest rejects request bodies and upgrades and keeps one active HTTP/1.1 request per connection; body streaming/chunked decoding remains future work.

# 5 Memory ownership and isolation

The initial idea used one WASM instance per worker and one arena per request, backed by reusable pages that were zeroed on release. This makes allocation predictable but provides no isolation between live request arenas: bounds checks in WASM apply to the entire linear memory, not individual C allocations. Use-after-free, adjacent-object overwrite, stale pointers, and persistent-state corruption remain possible.

Each connection instance has its own linear memory, globals, and runtime state, discarded at connection teardown. Within it, the request arena retains backing storage and resets only its used cursor when starting the next request, after the preceding message was copied out. Connection state, future TLS state, and buffered input live outside that arena. The global Wasm backing allocator remains append-only and the sole caller of memory.grow. An arena reset does not isolate requests on the same connection or erase their bytes.

Never let raw guest pointers survive asynchronous buffer ownership changes. Use checked offset/length pairs and instance/request ownership metadata at the host boundary. Options include copying into host-owned I/O buffers or explicitly pinning guest memory until completion. The latter saves copies but makes lifecycle correctness more demanding.

Virtual reservation is not resident RAM. Wasmtime can reserve several GiB of address space per 32-bit linear memory to accelerate bounds checking, while actual physical consumption follows touched pages. Many thousands of instances may exhaust virtual address space or pool slots, so maximum memory, reservation, guard, and pool settings need measurement.

# 6 Threat model and capabilities

| **Threat**               | **Containment or residual risk**                                                                                                              |
|--------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| Guest buffer overflow    | WASM confines accesses to guest linear memory, but cannot protect adjacent C objects inside that memory.                                      |
| Cross-request corruption | Per-request instance separation strongly reduces direct contamination; long-lived broker and TLS state remain shared within their own scopes. |
| Host boundary abuse      | Host validates offsets, lengths, handle generation, owning request/instance, permission, and pending I/O lifetime.                            |
| Unbounded CPU or memory  | Enforce maximum guest memory, operation counts, time/fuel/epoch budgets, query timeouts, result limits, and body sizes.                       |
| Authorization bypass     | Broker does not accept business assertions about roles, ownership, or workflow state; it uses trusted session and query evidence.             |
| SQL injection and SSRF   | No arbitrary SQL or network destinations from business WASM. Precompiled parameterized operations and allowlisted destinations only.          |
| Workflow misuse          | Limit operation order, repeated effects, parameter provenance, resource IDs, budgets, and transactional conditions.                           |
| Fatal guest trap         | Discard the affected connection instance and close its connection. Treat a shared broker trap as potentially affecting the worker or broker state. |

The HTTP parser selects a request type, but its result is not an authentication claim. The broker must validate session credentials and bind the chosen route and parameters to an immutable, request-scoped policy context. Business code cannot freely select a different workflow or mint capabilities.

# 7 TLS compartment and signing

BoringSSL, AWS-LC, rustls, and OpenSSL were considered. rustls has a memory-safe TLS protocol core and a C FFI. BoringSSL has large-scale Google deployment and extensive fuzzing; vendoring reduces its API/ABI stability concern, although upstream security patches still require tracking. An experimental BoringSSL-to-WASM build requires verification of platform support and cryptographic throughput, particularly loss of native assembly accelerations.

TLS may run as one WASM instance per connection, with no filesystem or arbitrary socket imports. That prevents direct access to other connection instances but does not prevent exfiltration of secrets already present in the compromised instance: its legitimate connection output is an exfiltration channel.

A separate signing WASM compartment retains the certificate private key and exposes only narrow signing operations for handshake authentication. The TLS instance retains ephemeral traffic secrets. Ticket-encryption keys and other shared long-term secrets must likewise stay out of per-connection TLS instances, or ticket support may be disabled initially.

The signer must not behave as an unrestricted signature oracle. Rate limits, key scope, handshake context validation, and fixed-format output all matter. Even without direct network access, a compromised signer can potentially encode information in outputs; network restriction alone is not a formal non-exfiltration guarantee.

# 8 Shared workflow definition and offline compiler

A single source language defines request types, typed parameters, query templates, guarded branches, bounded loops, allowed external effects, transactions, and resource budgets. Compilation happens offline, and runtime code is specialized. The same definition generates independent business execution and broker enforcement machines; their mutable states are never shared.

| **Generated artifact** | **Purpose**                                                                                                        |
|------------------------|--------------------------------------------------------------------------------------------------------------------|
| Business state machine | Explicit ready/wait/result transitions, request-local state layout, typed callbacks and response rendering points. |
| Broker policy machine  | Allowed operation IDs, trusted guards, parameter provenance, resource scope, counts, limits and terminal states.   |
| SQL and bindings       | Prepared parameterized PostgreSQL statements, typed result decoders, and metadata for compile-time checks.         |
| Security manifest      | Auditable list of reachable tables, columns, mutations, services, guards, budgets and workflow versions.           |

Each generated pair should embed a workflow version/hash, and incompatible business and broker artifacts must not run together. The broker treats calls from business WASM as adversarial even when generated C APIs make invalid calls difficult in normal source code.

# 9 Example workflow semantics

workflow publish_document(document_id: DocumentId from route) {

document = documents.get(document_id)

require document.owner_id == session.user_id

require document.status == Draft

documents.publish(document.id)

return document

}

This is proposed syntax, not an implemented compiler. A broker-issued resource handle can bind the exact document found during an authorized read. Publication should still use an atomic conditional update or transaction to prevent time-of-check/time-of-use races. A compromised business instance must not be allowed to substitute a new document ID merely because it matches the raw integer type.

Branches over user choices can be application-controlled but must remain within the broker’s permitted transition graph. Security guards involving roles, ownership, mutable database facts, or financial limits must be evaluated from trusted broker state or enforced atomically in the database. Loops require maximum iterations and scoped resource sets.

Transactional workflows and asynchronous application workflows should be distinguished. A database transaction may contain several relational operations. An external email, upload, or long-lived network operation generally needs a separate async state-machine boundary and failure/retry semantics; it must not silently hold database locks while waiting on remote systems.

# 10 Query language direction

The final direction is open to a new offline-compiled query language, including ORM-like features, but avoiding the expressiveness collapse common in complex ORM queries. The motivating complaint is SQL’s awkward reading order: selected expressions precede the source tables and often require jumping around among joins, grouping, and nested clauses.

Convex’s “It’s not you, it’s SQL” argues for typed documents, ordinary procedural transactional code, explicit index usage, and predictable OLTP performance. Its model does not provide general SQL-style joins and aggregations directly in the query language; JavaScript composes complex operations. The proposed stack would retain PostgreSQL’s relational optimizer and rich SQL execution features instead of recreating them in guest loops.

Research references include PRQL for top-to-bottom pipelines compiling to SQL; Kusto for sequence-oriented tabular transformations; EdgeQL for nested shapes and compositional relational expressions; and Malloy for rich aggregations and nested outputs. These are design inspirations rather than settled dependencies.

from users as user

join orders on orders.user_id == user.id

filter orders.status == Completed

group by user.id, user.name {

order_count = count(orders.id)

}

filter order_count \>= 5

sort order_count desc

A query expression should stay symbolic until compilation. For example, nested “three most recent orders per user” must compile to a lateral/window-based relational plan rather than a database request for each user. Complex features such as recursive CTEs, windows, unions, lateral joins, aggregates, transactions, and nested result shapes are tests for language adequacy.

The compiler IR should separate relational values from scalar values, ordering, provenance, effects, and control flow. Procedural source syntax does not require procedural database execution; a read-check-update workflow can sometimes be fused into one conditional UPDATE ... RETURNING statement. Unsupported or opaque SQL remains a possible escape hatch, with explicit broker policy and limited static guarantees.

Performance contracts may state required indexes, maximum result rows, query timeouts, and plan expectations. PostgreSQL can change plans even when an index exists: a compiler can verify schema and test plans but cannot promise index usage without stronger database-level control.

# 11 Performance evidence and caveats

| **Source / result**                                                                     | **Interpretation**                                                                                                   |
|-----------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| Fastly Lucet 2019: about 30 µs instantiate, 5 µs switch, 23 µs teardown for Hello World | Historical microbenchmark; demonstrates feasibility of fresh per-request isolation, not this stack’s cost.           |
| Wasmtime 1.0: about 2 ms to 5 µs instance startup for SpiderMonkey.wasm                 | Pooling, lazy initialization and copy-on-write can radically reduce initialization; not guaranteed for every module. |
| Fermyon/Arm: 200–900 µs cold starts; very high Hello World throughput                   | Vendor-reported platform workload; not directly comparable with bare module instantiation.                           |
| Cloudflare Workers: reusable V8 isolates with multiple concurrent requests              | Different trade-off: efficient multiplexing and caching but request-state leakage/CPU interference concerns.         |
| Hypothesis: 50k req/s × 10 µs additional CPU = 0.5 CPU core                             | Arithmetic illustration only, excludes teardown, initialization, I/O, memory reset and contention.                   |

The primary unknown is not isolated module startup but maintaining thousands of \*live, suspended\* instances across few cores. Benchmark peak RSS, virtual address space, CPU overhead, pool saturation, tail latency, queue depth, and process behavior under overload. Compare copy-on-write/madvise resets with retained memory and explicit clearing, since VM operations and interprocessor interrupts may matter under high parallelism.

# 12 Decisions, alternatives and unresolved issues

| **Status**   | **Decision or question**                                                                                                      |
|--------------|-------------------------------------------------------------------------------------------------------------------------------|
| Selected     | Separate signer, connection (TLS + HTTP), broker, and business security domains. |
| Implemented  | Per-connection HTTP instance pinned to a worker, indexable ownership arena, namespaced action protocol, resettable request arena. TLS is future work in the same instance. |
| Preferred    | Offline shared workflow definition compiles SQL, typed bindings, broker policy, and explicit async state machines.            |
| Preferred    | HTTP/1.1 origin behind Cloudflare initially, no general network access from guest modules.                                    |
| To benchmark | io_uring versus epoll; native rustls versus BoringSSL-in-WASM cryptographic throughput.                                       |
| To design    | Whether broker is a long-lived WASM instance, per-request policy instance, or native handle table with isolated evaluators.   |
| To design    | Cross-instance ABI, streaming buffers, handle ownership, lifecycle of parser output, and unconsumed HTTP bytes.               |
| To design    | Signing interface, key/ticket lifecycle, handshake verification and resistance to signature-oracle misuse.                    |
| To design    | Query language syntax, relational IR, SQL escape hatch, cost contracts, transactions and compiler proof boundaries.           |

# 13 Suggested implementation and verification sequence

1\. Build a native C host and minimal WASM guest with event/command batches, generation handles, structured errors and cancellation. Keep an epoll baseline alongside io_uring.

2\. Compile one small HTTP/1.1 parser guest with strict message framing and fuzz it. Test keep-alive, chunked bodies, pipelining leftovers, malformed headers and early disconnects.

3\. Evaluate Wasmtime pooling for per-connection instances, then run 1k–20k mostly suspended connections, measuring RSS, virtual reservation, arena resets and p99 latency. The initial implementation uses one bounded store per connection without pooling.

4\. Prototype TLS as native rustls and BoringSSL-in-WASM, measuring handshake and bulk cryptography costs. Add isolated signer only after defining its narrowly scoped contract.

5\. Implement host-owned request capabilities and one handwritten workflow policy that restricts parameter provenance, operation order and resource scope.

6\. Build the offline compiler for a narrow subset: typed inputs, simple reads, guarded writes, bounded branches, generated C state machine and broker transition table.

7\. Expand query IR through a corpus of 20 representative queries and workflows, including joins, grouping, nested shapes, window functions and atomic mutations.

8\. Perform adversarial tests: compromised parser output, fabricated business handles, stale I/O completions, trap recovery, cross-connection TLS isolation, and broker escalation.

# 14 Sources for further study

Wasmtime security model — https://docs.wasmtime.dev/security.html

Wasmtime fast instantiation — https://docs.wasmtime.dev/examples-fast-instantiation.html

Bytecode Alliance Wasmtime 1.0 performance — https://bytecodealliance.org/articles/wasmtime-10-performance

Fastly Lucet performance and lifecycle — https://www.fastly.com/it/blog/lucet-performance-and-lifecycle

Fastly Compute sandbox lifecycle — https://www.fastly.com/documentation/guides/compute/developer-guides/sandbox-lifecycle/

Cloudflare HTTP/2 to origin — https://developers.cloudflare.com/speed/optimization/protocol/http2-to-origin/

Cloudflare HTTP/3 — https://developers.cloudflare.com/speed/optimization/protocol/http3/

BoringSSL project — https://github.com/google/boringssl

rustls security policy — https://github.com/rustls/rustls/security/policy

Cloudflare Keyless SSL — https://developers.cloudflare.com/ssl/keyless-ssl/

WebAssembly Component Model canonical ABI — https://component-model.bytecodealliance.org/advanced/canonical-abi.html

Convex SQL critique — https://stack.convex.dev/not-sql

PRQL documentation — https://prql-lang.org/book/

EdgeQL documentation — https://docs.geldata.com/reference/edgeql

Malloy documentation — https://docs.malloydata.dev/
