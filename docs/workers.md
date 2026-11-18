# Workers and benchmarks

Build with `./news-platform/build.sh`, then run:

```bash
news-platform/build/news-platform                # one pinned worker per available physical P-core
news-platform/build/news-platform --workers 0    # inline, no worker threads
news-platform/build/news-platform --workers 1
news-platform/build/news-platform --workers 8
news-platform/build/news-platform --workers 4 --business-dispatch main
news-platform/build/news-platform --workers 2 --business-workers 4
```

The build needs a native C compiler, Clang with wasm32 support, a WASI C sysroot,
its compiler-rt builtins archive, and `xxd`. `build-guest.sh` accepts `WASI_CC`,
`WASI_SYSROOT`, and `WASI_BUILTINS`; defaults use the previously unpacked toolchain
under `build/wasi-toolchain/usr`, as in `tests/wasm-memory.sh`. WASI libc supplies
memory primitives at link time, but the resulting guest has no WASI imports.
`prepare-wasmtime.sh` verifies and extracts the pinned upstream Wasmtime 50.0.1
Linux x86-64 C API archive under `build/wasmtime`, downloading only when absent.
The matching vendored source and licenses remain untouched. The host executable
loads `build/wasmtime/lib/libwasmtime.so` through a relative runtime search path.

All three guests are embedded in the host binary and compiled once at startup. Native
code links only the runtime adapter and structural message validator, not the
HTTP parser or response policy. Those are compiled into the guest.

Project-generated C files and headers live in `src/generated/` and are intended
for version control. This includes route, business, and broker sources and the three
`*_guest_bytes.h` headers emitted by `build-guest.sh`. The latter are published
after all guest builds succeed. The `.wasm` binaries remain under `build/`.
Rebuild with `build.sh` to refresh the generated artifacts together.

All three runtimes use Wasmtime's pooling allocator. Each engine owns a separate pool
of 1024 core-instance, memory, and table slots, matching the server connection
limit through `WASM_POOL_CAPACITY` in `wasm_pool.h`. Each module may define one
memory and one table; limits remain 2 MiB per memory and 64 table elements.
These are virtual reservations, not eagerly allocated physical memory. Upstream
guard-page, reset/decommit, and keep-resident defaults are retained. Pool sizing
is independent of worker count: connection stores can remain live while workers
run other jobs, and at most one business job per connection can be active.

Connection stores last until connection teardown. Compatible business instances
are retained in thread-local pools, with one idle instance per runtime per thread:
Each guest call is synchronous, but active business instances survive query yields
and cannot enter the idle cache. The host resets all guest state after copying a
successful response and receiving broker completion. Traps,
invalid output and reset failures destroy the instance. Unsupported modules use
fresh instances. Wasmtime resets backing slots whenever stores are destroyed.
Modules whose minimum memory/table requirements exceed pool limits are rejected
at module compilation, before instance creation. Runtime store limits remain in
place for growth. Tests dirty globals, tables, data segments, zero memory and
grown pages across normal and trapping requests to verify reuse isolation.
The connection test also fills all 1024 slots, checks exhaustion, and verifies
that closing one instance makes its slot available again.

The main thread owns the listener, connections, and the only io_uring queue.
The worker count is additional to the main thread. Each worker is pinned to one
logical CPU selected from a physical P-core; each connection and its Wasm
instance stay on one worker. Default sizing counts only available physical
P-cores, deduplicating package/core IDs rather than counting SMT siblings.
On the current host this means six workers: six P-cores and eight E-cores are
available, with 20 logical CPUs. The selected P-core CPUs are 0,2,4,6,8,10.
Explicit counts still work; workers beyond the selected core count share those
same CPUs round-robin. They do not spill onto E-cores or additional SMT siblings.
The main I/O thread and inline mode retain the calling thread's affinity.

Selection respects the calling thread's affinity/cpuset. If P/E classification
is unavailable, startup reports a fallback to all available physical cores.
If known E-cores are available but no P-cores are allowed, worker startup fails;
inline mode remains usable. Affinity is applied with pthread creation attributes
before a worker's callback runs; affinity failure aborts pool creation and joins
any workers already created. No steady-state synchronization or wakeup is added.
Missing topology IDs fall back to individual logical CPUs and are reported as
unresolved. This respects CPU affinity/cpusets, but does not convert cgroup CPU
quota into a worker count. Use an explicit count when needed.

`OsCpuInfo` reports available logical CPUs, cores, P/E/unknown classifications,
and unresolved topology IDs, plus owned slices of representative core CPU IDs
and P-core CPU IDs. Release these with `OsCpuInfoDestroy`. `ThreadStart` takes a
borrowed CPU-ID slice (empty means inherit); `WorkerPoolCreate` takes a borrowed
placement slice and copies each selected CPU into its thread's affinity at
creation. On Intel hybrid systems, classification uses the
kernel's `cpu_core/cpus` and `cpu_atom/cpus` PMU lists ([kernel documentation](https://android.googlesource.com/kernel/common/+/d15bb1f6dabe1/tools/perf/Documentation/intel-hybrid.txt)).
Unavailable classifications remain unknown; frequencies are not used to guess
core types. Threaded startup prints detected counts and the worker CPU list.
Topology is a startup snapshot; hotplug does not resize a running pool.

## Ownership and protocol

`external/common/worker.h` implements generic jobs with a callback, message type,
context, borrowed input slice, and correlation ID. It has no HTTP dependency.
Each worker has two bounded SPSC buffers: controller-to-worker jobs and
worker-to-controller results. Workers continuously poll their incoming queues and
publish results directly: no eventfd notifications, blocking waits, `pause`, or
scheduler yields. The main thread polls both io_uring completions with `IoPoll`
and every worker's result queue, including when no socket I/O completes. Both
workers and the main loop spin while idle. No I/O queue,
work-stealing queue, or shared result queue is accessed by multiple threads.

The controller reserves a result slot when submitting each job. Until the result
is received, that credit remains occupied, even after execution finishes. A full
worker returns `WORKER_SUBMIT_WOULD_BLOCK` without accepting the job. Both byte buffers are sized
for complete records at the credit limit, so SPSC's partial-transfer API never
splits a job/result in this protocol. Workers never block waiting for result
space. Pool destruction requires all results received;
it stops and joins the threads before freeing their storage.

Connections are assigned round-robin and remain on that worker until teardown.
A host indexable arena provides stable connection records, recording worker index
and a generation-tagged ID. Socket completions and worker messages carry that ID.
Slots are recycled only after `CONNECTION_CLOSED`; stale generations are rejected.
Each connection has one pending socket operation or worker job at a time.

`CONNECTION_OPEN` creates one Wasmtime store/instance on the owner worker.
`CONNECTION_RECEIVE`, `CONNECTION_EOF`, and `CONNECTION_SEND_COMPLETE` advance it.
The guest returns a `CONNECTION_REQUEST` action batch: request more input, expose
a parsed HTTP request, send bytes, or close. `CONNECTION_CLOSE` destroys the
instance on its owner and replies with `CONNECTION_CLOSED`. Numeric namespaces
separate `CONNECTION_*`, `BROKER_*`, `SIGNING_*`, and `BUSINESS_*`.

Valid bodyless requests are matched by generated C inside the connection guest.
Matched requests first create an isolated broker instance for admission, then
borrow a reset business instance (or create one). `BusinessInit` provides an
input window for the relocatable route dispatch envelope. `BusinessHandle`
returns a query action or a final `BUSINESS_RESULT` descriptor. The host validates and copies the result
before resetting or discarding the instance, then passes `CONNECTION_RESPONSE` to the
connection guest. That guest serializes HTTP and applies HEAD/persistence rules.
Business failures become `CONNECTION_BUSINESS_FAILED`: HTTP 500 then close.
Handwritten route handlers build the response: home returns `Hello, world!`,
empty returns 204, and profile/timeline return JSON from typed synthetic queries.
The profile-summary example performs two queries and returns its retained first result.
The routes are declared in `app.cfg`; 404/405 handling and invalid HTTP framing stay in
the connection guest; request bodies and TLS remain unimplemented.

`--business-dispatch local` (default) pins the workflow to the connection worker,
with each phase submitted as a separate job through main. `--business-dispatch main` sends parsed
requests through main, which chooses the worker with the fewest outstanding
jobs, rotating ties. Every continuation stays on that initially selected worker.
This is a controller-owned pending count, not an extra
shared atomic or a guarantee of instantaneous worker idleness. Main routes the
business result back to the pinned connection owner for serialization. No
connection job executes while another worker uses its copied request. Each
connection record owns business output storage, retained through the resume job.

`--business-workers Y` selects main dispatch with a separate business pool;
`--workers X` controls connection workers. Without this option main dispatch
shares the connection pool. Business CPU placement starts after the X connection
workers in the allowed P-core list, wrapping only when X+Y exceeds it. Thus
2+4 uses six distinct P-cores on the current machine. All threads still use SPSC
queues and spin. With X=0 connection work runs on main; a separate business pool
can still be used. With no pools, a bounded main-owned job queue advances one
phase per request per event-loop visit, allowing I/O and other requests between phases.

`src/workflow.c` owns broker/business sessions and host-only tickets containing
the generation-tagged connection ID, request serial, and phase counter. Stale
tickets and calls from another thread are rejected. Broker policy independently
checks route, operation, and sequence and derives the numeric ID from its own
immutable request copy. Query results are checked against generated shapes before
resuming business code. Every query is a separate queued synthetic-backend phase;
there is no database I/O or authentication yet. Broker instances are always
destroyed at completion. Owner-thread cancellation discards both live instances.
The server currently discovers disconnects through its existing socket operations;
it does not monitor a socket concurrently with an active business workflow.

The guest retains partial/pipelined input in fixed scratch storage outside its
request arena. Network consumption is distinct from HTTP parsing consumption.
It emits at most one parsed request per call. Before the next call, the worker
validates and copies that contiguous message and send bytes into host storage.
No guest pointer reaches socket I/O. Host copies remain valid until the next job;
partial sends finish before sending `CONNECTION_SEND_COMPLETE`. This call can
start a buffered next request, resetting the request arena cursor at that point.
No separate reset acknowledgement or host mutation of allocator internals occurs.

The server allocates 1024 job credits per worker, matching its total connection
limit. Its one-job-per-connection rule guarantees submission space even if all
connections select one worker. It applies backpressure by not issuing another
receive while parsing is pending. Inline mode invokes the same Wasm connection handler
on the main thread without creating a pool or worker queues. It uses the same
nonblocking I/O loop.

## Guest ABI and limits

`connection_protocol.h` defines the v4 little-endian ABI using fixed-width `u32`
fields and `{u32 ptr, u32 len}` slices. Byte slices use linear-memory offsets;
action-slice lengths count elements. HTTP message offsets remain root-relative.
`ConnectionInit(version)` returns an offset to a `connection_init_result`: its
own tag identifies success, repeated initialization, unsupported version, or
allocation failure. Only success supplies the control-block offset.
`ConnectionStep()` reads an input command from that block and publishes its own
`connection_step_result` tag together with a `CONNECTION_REQUEST` action
batch. The initial implementation permits three actions per batch, a 4096-byte
input window, 128 KiB parsed HTTP limit, 256 KiB dispatch limit, 64 KiB wire
headers, and 128 headers. The request arena has a 384 KiB limit to hold the
parsed message and its dispatch copy without relocation. `CONNECTION_ROUTED_REQUEST`
carries the tagged envelope described in [declarations.md](declarations.md).
`CONNECTION_HTTP_REQUEST` retains raw HTTP diagnostics alongside immediate error
responses. The native adapter validates and copies the routed envelope before
business dispatch; the business guest validates it again before using its tag.
It also has a separate 3072-byte business-response window so incoming business
results cannot overwrite buffered pipelined network input. Serialized responses
are bounded to 4096 bytes; oversized/invalid business output yields 500/close.

`business_protocol.h` defines the deployment-local business ABI. Init, Continue,
and Handle exports return
offsets to their own tagged results. Each store has a 2 MiB memory limit,
10 million fuel units per request, and no imports. The response is a contiguous root
plus offset-linked HTTP headers and optional body. Empty bodies use offset=0,
length=0. Host and connection guest validate descriptor bounds, header syntax,
cycles, status/body restrictions, and reject business-supplied framing headers.
The connection owns Content-Length and Connection; 204/304 omit Content-Length,
empty 200 sends Content-Length: 0, and HEAD preserves the representation length
while sending no body. Stores are destroyed after traps or bad output.

The shared `http_response` descriptor in `http_response_message.h` contains
status, linked `http_header` descriptors, and a root-relative `byte_range` body.
`HttpResponseBuild` validates status, headers, body restrictions and size before
one bump allocation, then copies slice inputs into a contiguous message. Empty
bodies use `{0, 0}`; duplicate headers and their order are preserved. The business
guest's arena owns the response until the host copies it and resets the instance.
`HttpResponseValidate` checks the shared descriptor at boundaries;
`HttpResponseSerialize` in the connection produces wire bytes and adds framing.
`http_wire_response` names the serialized immediate-response/policy result.
Test-only headers and fixtures live under `tests/`; `tests/response.sh` exercises
the builder and serializer with strict compilation and address/undefined sanitizers.

For reset, `wasm_reset.c` conservatively admits one unshared memory32, one
fixed-size funcref table (at most 64 elements) without element segments, and
exactly one mutable i32 Wasm global exported as `__stack_pointer`. Memory and
table exports are required. Function signatures must be numeric; imports, start
functions, tags, passive data/element segments and unknown forms disqualify reuse.
GC and typed function references are disabled. Nonmatching modules still run via
fresh-instance fallback. The actual C guest must pass admission in the test suite.

The host snapshots initial linear memory immediately after instantiation, before
BusinessInit. It retains only the prefix through the last nonzero byte. After
each successfully completed request it clears the entire current memory, restores that prefix,
restores the stack pointer, and resets all table entries to null. This covers C
statics, constants, stack and heap, not just arena-used bytes. The current memory
size is retained, so this intentionally differs from a fresh instance's
memory.size. Reset allocator globals initialize from the retained size on the next
BusinessInit; memory.grow is still owned exclusively by the guest allocator.
The guest allocator is append-only during each request, with no guest free list.

TLS keys use the common OS interface. Cache destructors run on their owning
threads at exit; join all other runtime users before BusinessRuntimeDestroy,
which also clears the calling thread's cache. No cache mutex or worker-to-worker
handoff is needed. Connection instances and their arena-reset rules are unchanged.

Every connection store has a 2 MiB linear-memory limit and 100 million fuel units per call,
including initialization. Modules with imports are rejected; there is no WASI,
shared memory, socket capability, or guest callback. Traps and invalid ABI data
close the affected connection and destroy its instance on the owner worker.
The adapter reacquires the memory view after every call, checks slice bounds,
validates the request's root/field/header-link ranges, and copies outputs before
another call. It also rejects wrong-thread access and illegal input sequencing.
This validates memory safety at the boundary, not authentication or authorization.

Native fallible interfaces use separate result structs and enums for each
function. For example, `TcpListen` returns `tcp_listen_result`; only
`TCP_LISTEN_OK` carries a socket value. `IoPoll` returns `io_poll_result`, with
distinct `IO_POLL_OK`, `IO_POLL_EMPTY`, and failure tags. Socket completions have
their own tags, so an EOF byte count cannot be mistaken for a polling failure.
There is no shared numeric error or global result-tag convention. Native errno
values remain diagnostic payloads of system-error cases. Callers translate
between function-specific outcomes explicitly; the build rejects mixing enums.

The generic worker envelope carries a borrowed `outcome` byte slice. Connection
jobs publish a `connection_job_result` there, stored in their worker-owned
context until the next job. The host checks its size and copies it before
inspecting the tag. This leaves other job types free to define their own result
structs. Slice, memory, SPSC, parser initialization/reset/finish, and runtime
interfaces follow the same function-specific convention.

HTTP/1.1 connections persist by default. HTTP/1.0 requires `Connection: keep-alive`;
`close` takes precedence, including across multiple Connection fields and
comma-separated options. Responses remain ordered when requests are pipelined.
HEAD has no response body. There is no fixed request-count cap per connection.
The request arena retains backing storage across requests; it is not recreated
or cleared at reset. Connection state survives until instance destruction. Unsupported bodies,
upgrades, malformed requests, and framing errors receive an error and close;
their leftover bytes are never processed as another request. Idle timeouts are
not implemented yet. These rules follow the framing requirements in
[RFC 9112 section 9.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.3).

## Verification and measurement

```bash
news-platform/tests/run.sh
news-platform/tests/benchmark.sh 10000 32 0 1 2 4 default
news-platform/tests/benchmark.sh 100000 1000 0 1 2 4 default
REQUESTS_PER_CONNECTION=10 news-platform/tests/benchmark.sh 300000 32 0 1 2
news-platform/tests/benchmark-business.sh 300000 32
```

The suite exercises the HTTP server in default, zero-, one-, and two-worker modes,
shared main dispatch and separate connection/business pools,
plus worker queue saturation, ordering, byte-ring wrap, polling, parser cancellation,
EOF, inline equivalence, and CPU affinity restriction.
Worker tests verify the actual mask and running CPU inside callbacks, multiple
workers sharing a selected core, rejection of CPUs outside a restricted caller
mask, unchanged parent affinity, and cleanup after partial pool creation.
It also checks repeated requests, HTTP/1.0 persistence, close precedence, HEAD
and mixed-status pipelines, fragmented follow-up requests, and pipelines larger
than a receive buffer.
It also checks Wasm worker ownership, stale IDs, cancellation, traps/fuel, ABI
bounds, disallowed imports, copied pipelined messages, and a stable memory-size
plateau across 2,000 requests after arena growth.
Compile-failure tests verify that one function's result struct or tag cannot
accidentally be substituted for another's.

The benchmark accepts requests per run, client concurrency (1–1024), then worker
counts. Defaults are 10000 requests, concurrency 32, and counts `0 1 2 default`.
`REQUESTS_PER_CONNECTION` defaults to 1; set it to 10 for ten sequential requests
per connection. Total requests must be divisible by that setting. The client
requests `Connection: close` on the last request, and validates every response by
Content-Length before sending the next request (no benchmark pipelining).
It builds once, starts/stops a server per count, warms up 64 connections, and reports
CSV: worker setting, requests, concurrency, seconds, requests/second. `default`'s
actual count is in `build/benchmark-default.log`. It refuses an occupied test port.
Every response is checked for HTTP status, body, and framing by the existing C
HTTP test client. This measures connection setup, I/O, parsing, worker
handoffs, and response handling together, not isolated parser speed.

Client and server run on the same host and compete for CPU. Short runs are smoke
tests, not reliable performance conclusions. Repeat longer runs, vary concurrency,
and compare under the same affinity and load. No-thread mode may win for small
requests whose parsing costs less than a thread handoff.

The client uses one blocking thread per concurrent request stream. Threads sleep
on individual start events until the timed run begins; startup does not spin.
At high concurrency, client scheduling and connection churn can limit measured
throughput. The server supports 1024 simultaneous connections. The client needs
approximately twice the concurrency in file descriptors (start events + sockets).

`tests/benchmark-business.sh` compares inline execution, local business execution,
main dispatch to a shared pool, and separate connection/business pools. Defaults
are 300000 requests, concurrency 32, ten requests per connection and three rounds.
Each measurement has a separate 20000-request warmup; configuration order rotates
between rounds. `ROUNDS` and `REQUESTS_PER_CONNECTION` can override the defaults.
The matrix compares shared/local pools of 1, 2, 4, and 6 workers with 1+1, 1+3,
2+2, 3+1, 1+5, 2+4, 3+3, 4+2, and 5+1 split pools. Counts exclude the main thread.
Server logs, raw CSV, settings, and median/min/max summaries are saved under
`build/benchmarks/business-<UTC timestamp>/`. For an even round count the summary
uses the lower middle observation. All runs use the same guests and validate
both test headers, body, status and framing on every response.
