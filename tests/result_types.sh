#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
compiler=${CC:-cc}
flags=(-std=c11 -Wall -Wextra -Wpedantic -Werror -Werror=enum-compare -Werror=enum-conversion -Iexternal/common -fsyntax-only -x c -)
# Positive control: avoid accepting a missing compiler/header as a test success.
"$compiler" "${flags[@]}" <<'C'
#include "slice.h"
byte_slice_copy_result Copy(byte_slice to, const_byte_slice from) { return ByteSliceCopy(to, 0, from); }
C
if "$compiler" "${flags[@]}" >build/result-struct-mismatch.log 2>&1 <<'C'
#include "slice.h"
byte_slice_copy_result Copy(byte_slice to, const_byte_slice from) { return ByteSliceCopyRange(to, 0, from, 0, from.len); }
C
then
    echo 'Different function result structs must not be interchangeable.' >&2
    exit 1
fi
if "$compiler" "${flags[@]}" >build/result-tag-mismatch.log 2>&1 <<'C'
#include "slice.h"
byte_slice_copy_result Copy(void) { return (byte_slice_copy_result){.tag = BYTE_SLICE_COPY_RANGE_OK}; }
C
then
    echo 'Different function result tags must not be interchangeable.' >&2
    exit 1
fi
echo 'PASS: compiler rejects cross-function result structs and tags'
