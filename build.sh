#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
infra=$PWD
app=$(cd "${1:?Usage: build.sh APPLICATION_DIRECTORY}" && pwd)
mkdir -p "$app/build"
ln -sfn "$infra/build/wasmtime" "$app/build/wasmtime"
ln -sfn "$infra/build/app-compiler" "$app/build/app-compiler"
./prepare-wasmtime.sh
mkdir -p build
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror \
    -Iexternal/common -Iexternal/secure-backends \
    src/app_compiler.c external/secure-backends/declaration/lexer.c \
    external/secure-backends/declaration/parser.c external/secure-backends/declaration/ir.c \
    external/secure-backends/declaration/ir_dump.c external/secure-backends/declaration/sql.c \
    external/secure-backends/declaration/query_sql.c external/secure-backends/declaration/index_sql.c \
    external/secure-backends/declaration/physical_schema.c external/secure-backends/declaration/routes.c \
    external/secure-backends/declaration/workflow.c -o build/app-compiler
./build/app-compiler build "$app"
./build-guest.sh "$app"
mkdir -p build/liburing
(
    cd build/liburing
    configure_tmp=$(mktemp -d "$PWD/.configure.XXXXXX")
    trap 'rm -rf -- "$configure_tmp"' EXIT
    export TMPDIR="$configure_tmp"
    # Upstream leaves the base file of its hard-coded /tmp BPF probe behind.
    # Redirect that probe into our owned temporary directory without editing it.
    mktemp() {
        if [[ ${1-} == /tmp/test_bpf_XXXXXX ]]; then
            command mktemp "$TMPDIR/test_bpf_XXXXXX"
        else
            command mktemp "$@"
        fi
    }
    export -f mktemp
    if ! bash ../../external/liburing/configure --cc="${CC:-cc}" --use-libc >configure.log 2>&1; then
        cat configure.log >&2
        exit 1
    fi
)
make -s -C build/liburing/src liburing.a
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -pthread \
    -Iexternal/common -Iexternal/secure-backends -I"$app/src/generated" -isystem build/wasmtime/include \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    src/main.c src/connection_job.c src/workflow.c "$app/src/generated/app-queries.c" external/secure-backends/query.c external/secure-backends/query_fake.c external/common/worker.c external/common/index_arena.c \
    external/common/os.c external/common/os_cpu.c external/common/os_memory.c \
    external/common/bump.c external/common/spsc.c external/secure-backends/connection_vm.c external/secure-backends/business_vm.c external/secure-backends/wasm_reset.c external/common/os_thread_local.c external/secure-backends/http_response_message.c \
    build/liburing/src/liburing.a -Lbuild/wasmtime/lib -lwasmtime \
    -Wl,-rpath,'$ORIGIN/wasmtime/lib' -o "$app/build/server"
