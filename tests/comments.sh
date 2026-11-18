#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
scratch=$(mktemp -d "$PWD/build/.comments-XXXXXX")
trap 'unlink "$scratch/test" 2>/dev/null || true; rmdir "$scratch"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -Iexternal/common -Iexternal/secure-backends tests/comments.c \
    external/secure-backends/declaration/lexer.c -o "$scratch/test"
"$scratch/test"
