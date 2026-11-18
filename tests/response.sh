#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d)
Cleanup() {
    for name in response response-sanitized; do
        if [[ -f "$task_tmp/$name" ]]; then unlink "$task_tmp/$name"; fi
    done
    rmdir "$task_tmp"
}
trap Cleanup EXIT
flags=(-std=c11 -Wall -Wextra -Wpedantic -Werror -Werror=enum-compare -Werror=enum-conversion -Iexternal/common -Iexternal/secure-backends -Isrc)
sources=(tests/response.c src/http_response.c external/secure-backends/http_response_message.c external/secure-backends/http_response_builder.c external/common/bump.c external/common/os_memory.c)
"${CC:-cc}" "${flags[@]}" -O2 "${sources[@]}" -o "$task_tmp/response"
"$task_tmp/response"
"${CC:-cc}" "${flags[@]}" -O1 -g -fsanitize=address,undefined "${sources[@]}" -o "$task_tmp/response-sanitized"
"$task_tmp/response-sanitized"
