#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d "$PWD/build/.query-test-XXXXXX")
trap 'unlink "$scratch/test" 2>/dev/null || true; rmdir "$scratch"' EXIT
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
    -Iexternal/common -Iexternal/secure-backends tests/query.c external/secure-backends/query.c \
    external/secure-backends/query_fake.c -o "$scratch/test"
"$scratch/test"
