#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Pinned upstream C API binary matching external/wasmtime.version.
version=50.0.1
checksum=e2c0c26e1aef52f82f0b9918ad6f34cc30f4b557aee3e261cb127a9462be8f4d
root=build/wasmtime
mkdir -p "$root"
if [[ ! -f "$root/c-api.tar.xz" ]]; then
    [[ $(uname -s) == Linux && $(uname -m) == x86_64 ]]
    curl --fail --location --retry 3 "https://github.com/bytecodealliance/wasmtime/releases/download/v$version/wasmtime-v$version-x86_64-linux-c-api.tar.xz" -o "$root/c-api.tar.xz"
fi
printf '%s  %s\n' "$checksum" "$root/c-api.tar.xz" | sha256sum --check --status
if [[ ! -f "$root/include/wasmtime.h" || ! -f "$root/lib/libwasmtime.so" ]]; then
    tar -xJf "$root/c-api.tar.xz" --strip-components=1 -C "$root"
fi
