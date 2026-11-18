# Barework

A native C host with isolated connection, broker, and business WebAssembly
instances. Applications supply declarations and handwritten C route handlers.
Generated routing, query interfaces, continuations, and broker policy connect them.

## Build an application

```bash
./build.sh /absolute/path/to/application
```

The application contains `app.cfg` and `src/business/*.c`. The build generates
checked-in C sources and headers in its `src/generated/` and writes its executable
to `build/server`. Run that executable to listen on localhost:8080.
`build/app-compiler` in the application exposes the build/version commands.

Infrastructure sources are in `src/`, reusable primitives and backend components
in `external/common/` and `external/secure-backends/`, and preserved upstream
sources and licenses in the other `external/` directories. `docs/` describes the
compiler, architecture, and runtime. Historical benchmark examples refer to the
original news-platform application.

Requires Linux x86-64 with io_uring, a C compiler, Clang with wasm32 support,
Bash, make, curl, tar, xxd, and a WASI sysroot/compiler-rt archive. Wasmtime is
pinned and prepared automatically. Set `WASI_SYSROOT` and `WASI_BUILTINS`, or
prepare the local toolchain with `tests/wasm-memory.sh`. Dependency caches live
in this repository's ignored `build/` and are shared by consuming applications.
The built executable currently uses the cached Wasmtime shared library through
an application-local symlink; distributing binaries requires packaging that library.

## Tests

```bash
./tests/run.sh
```

Tests create and remove an isolated application workspace. The original
news-platform routes live in `tests/fixtures/news-platform/` as regression data;
they are not runtime defaults. The suite covers all ten dispatch configurations,
compiler diagnostics, request isolation, continuations, and malformed messages.

The current backend provides synthetic reads only. There is no authentication,
real database execution, mutation support, or TLS. Each deployment pins one
infrastructure revision and its generated ABI; cross-version guests are unsupported.
