#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d "$PWD/build/.workflow-test-XXXXXX")
trap 'unlink "$scratch/test" 2>/dev/null || true; rmdir "$scratch"' EXIT
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -Iexternal/common -Iexternal/secure-backends -Isrc -Isrc/generated -isystem build/wasmtime/include \
    -isystem build/liburing/src/include -isystem external/liburing/src/include \
    tests/workflow.c src/workflow.c src/generated/app-routes.c src/generated/app-queries.c \
    external/secure-backends/query.c external/secure-backends/query_fake.c \
    external/secure-backends/business_vm.c external/secure-backends/wasm_reset.c \
    external/secure-backends/http_response_message.c external/common/os_thread_local.c \
    external/common/bump.c external/common/os_memory.c external/common/os.c build/liburing/src/liburing.a \
    -Lbuild/wasmtime/lib -lwasmtime -Wl,-rpath,"$PWD/build/wasmtime/lib" -o "$scratch/test"
timeout 30s "$scratch/test"
