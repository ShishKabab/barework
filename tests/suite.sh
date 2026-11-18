#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

cd "$(dirname "$0")/.."
source tests/server.sh
./build.sh
./tests/result_types.sh
./tests/routes.sh
./tests/response.sh
./tests/workflow.sh
./tests/workflow_codegen.sh
./tests/query.sh
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -Iexternal/common \
    tests/slice.c -o build/slice-test
./build/slice-test
echo 'PASS: slice copy bounds, overlap, empty slices, overflow, relative byte ranges'
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror \
    -Iexternal/common -Iexternal/secure-backends tests/http.c \
    external/common/bump.c external/common/os_memory.c \
    external/secure-backends/http.c -o build/http-test
timeout 15s ./build/http-test
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -Iexternal/common \
    tests/memory.c external/common/bump.c external/common/os_memory.c -o build/memory-test
timeout 15s ./build/memory-test
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -pthread -Iexternal/common \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/os.c external/common/os.c build/liburing/src/liburing.a -o build/os-test
timeout 15s ./build/os-test
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -pthread -Iexternal/common \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/spsc.c external/common/spsc.c external/common/os.c \
    build/liburing/src/liburing.a -o build/spsc-test
timeout 30s ./build/spsc-test
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -pthread \
    -Iexternal/common \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/worker.c \
    external/common/worker.c external/common/spsc.c external/common/os.c \
    external/common/os_cpu.c external/common/os_memory.c external/common/bump.c \
    build/liburing/src/liburing.a -o build/worker-test
timeout 30s ./build/worker-test
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -pthread \
    -Iexternal/common -Iexternal/secure-backends -Isrc -Isrc/generated -isystem build/wasmtime/include \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/connection.c src/connection_job.c src/workflow.c src/generated/app-queries.c external/secure-backends/query.c external/secure-backends/query_fake.c external/secure-backends/connection_vm.c external/secure-backends/business_vm.c external/secure-backends/wasm_reset.c external/common/os_thread_local.c external/secure-backends/http_response_message.c \
    external/common/worker.c external/common/spsc.c external/common/index_arena.c \
    external/common/os.c external/common/os_memory.c external/common/bump.c \
    build/liburing/src/liburing.a -Lbuild/wasmtime/lib -lwasmtime \
    -Wl,-rpath,'$ORIGIN/wasmtime/lib' -o build/connection-test
timeout 40s ./build/connection-test
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iexternal/common \
    -Iexternal/secure-backends -Isrc -Isrc/generated -isystem build/wasmtime/include \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    -pthread tests/business.c src/generated/app-routes.c external/common/bump.c external/common/os_memory.c src/http_response.c external/secure-backends/business_vm.c external/secure-backends/wasm_reset.c external/common/os_thread_local.c \
    external/secure-backends/http_response_message.c external/common/os.c build/liburing/src/liburing.a -Lbuild/wasmtime/lib -lwasmtime \
    -Wl,-rpath,'$ORIGIN/wasmtime/lib' -o build/business-test
timeout 30s ./build/business-test
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iexternal/common \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    -Iexternal/secure-backends -isystem build/wasmtime/include tests/business_reuse.c \
    external/secure-backends/business_vm.c external/secure-backends/http_response_message.c \
    external/secure-backends/wasm_reset.c external/common/os_thread_local.c \
    external/common/os.c build/liburing/src/liburing.a -Lbuild/wasmtime/lib -lwasmtime -Wl,-rpath,'$ORIGIN/wasmtime/lib' -o build/business-reuse-test
timeout 30s ./build/business-reuse-test
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iexternal/common \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/thread_local.c external/common/os_thread_local.c external/common/os.c \
    build/liburing/src/liburing.a -o build/thread-local-test
timeout 10s ./build/thread-local-test
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -pthread \
    -Iexternal/common -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/echo_server.c external/common/os.c build/liburing/src/liburing.a -o build/echo-server
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -pthread \
    -Iexternal/common -isystem build/liburing/src/include \
    -isystem external/liburing/src/include tests/echo.c external/common/os.c \
    build/liburing/src/liburing.a -o build/echo-test

if (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    echo 'Test port 8080 is already in use.' >&2
    exit 1
fi

server_log=build/echo-server.log
./build/echo-server >"$server_log" 2>&1 &
server_pid=$!
Cleanup() {
    status=$?
    trap - EXIT
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    if ((status != 0)); then
        cat "$server_log" >&2
    fi
    exit "$status"
}
trap Cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

timeout 30s ./build/echo-test
kill -0 "$server_pid"

# Startup on an occupied port must fail promptly with a useful diagnostic.
status=0
timeout 5s ./build/news-platform >build/echo-duplicate.log 2>&1 || status=$?
test "$status" -eq 1
grep -q 'TCP listen failed' build/echo-duplicate.log
echo 'PASS: occupied-port startup failure'

kill "$server_pid"
wait "$server_pid" 2>/dev/null || true
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -pthread \
    -Iexternal/common -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/http_server.c external/common/os.c build/liburing/src/liburing.a -o build/http-server-test
for workers in default 0 1 2 main-0 main-1 main-2 split-1-1 split-2-2 split-0-2; do
    server_log="build/http-server-$workers.log"
    arguments=()
    case "$workers" in
        default) ;;
        main-*) arguments=(--workers "${workers#main-}" --business-dispatch main) ;;
        split-*) split=${workers#split-}; arguments=(--workers "${split%-*}" --business-workers "${split#*-}") ;;
        *) arguments=(--workers "$workers") ;;
    esac
    StartHttpServer "${arguments[@]}"
    timeout 45s ./build/http-server-test
    kill -0 "$server_pid"
    echo "PASS: HTTP server workers=$workers"
    kill "$server_pid"
    wait "$server_pid" 2>/dev/null || true
done
