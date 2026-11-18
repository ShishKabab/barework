#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build
# Supply a WASI sysroot and compiler-rt archive, or use the locally unpacked
# wasi-libc + libclang-rt-18-dev-wasm32 packages. Nothing is downloaded here.
sysroot=${WASI_SYSROOT:-"$PWD/build/wasi-toolchain/usr"}
builtins=${WASI_BUILTINS:-"$sysroot/lib/llvm-18/lib/clang/18/lib/wasi/libclang_rt.builtins-wasm32.a"}
if [[ ! -f "$sysroot/lib/wasm32-wasi/libc.a" || ! -f "$builtins" ]]; then
    echo 'Set WASI_SYSROOT and WASI_BUILTINS to a WASI C toolchain.' >&2
    exit 1
fi
"${WASI_CC:-clang}" --target=wasm32-wasi --sysroot="$sysroot" \
    -std=c11 -O2 -fno-inline -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror -nostdlib \
    -Iexternal/common -Iexternal/secure-backends tests/wasm_memory.c \
    external/common/wasm_memory.c external/common/bump.c external/secure-backends/http.c \
    "$sysroot/lib/wasm32-wasi/libc.a" "$builtins" \
    -Wl,--no-entry,--export=TestMemory,--initial-memory=262144,--max-memory=4194304 \
    -o build/wasm-memory-test.wasm
# With inlining disabled, verify no linked libc heap allocator or other code
# introduces another growth site alongside WasmMemoryEnsure.
growth_sites=$("${WASM_OBJDUMP:-llvm-objdump}" -d build/wasm-memory-test.wasm | grep -c 'memory.grow')
test "$growth_sites" -eq 1
for trial in 1 2; do
    result=$("${WASMTIME:-wasmtime}" run --invoke TestMemory build/wasm-memory-test.wasm)
    if [[ "$result" != 0 ]]; then
        echo "Wasm memory test failed at source line $result (instance $trial)." >&2
        exit 1
    fi
done
echo 'PASS: Wasm growth, contiguous arenas, blocked growth, exhaustion, no reuse'
