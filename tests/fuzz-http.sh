#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build/http-corpus
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror=enum-compare -Werror=enum-conversion -Werror \
    -fsanitize=fuzzer,address,undefined -Iexternal/common -Iexternal/secure-backends \
    tests/http_fuzz.c external/common/bump.c external/common/os_memory.c \
    external/secure-backends/http.c \
    -o build/http-fuzz
printf 'GET / HTTP/1.1\r\nHost: example.test\r\n\r\n' >build/http-corpus/get
printf 'POST / HTTP/1.1\r\nX: \ta b \t\r\nX:two\r\nContent-Length: 4\r\n\r\nDATA' >build/http-corpus/post
printf 'OPTIONS * HTTP/1.0\r\nEmpty:\r\nRaw:\200\377\r\n\r\n' >build/http-corpus/options
./build/http-fuzz -runs="${HTTP_FUZZ_RUNS:-20000}" -max_len=4096 \
    -artifact_prefix=build/ build/http-corpus
