#pragma once
#include "types.h"

typedef enum byte_range_resolve_tag {
    BYTE_RANGE_RESOLVE_OK, BYTE_RANGE_RESOLVE_INVALID_ARGUMENT, BYTE_RANGE_RESOLVE_OUT_OF_RANGE
} byte_range_resolve_tag;
typedef struct byte_range_resolve_result {
    byte_range_resolve_tag tag;
    byte_slice bytes; /* Borrowed; meaningful only on OK. */
} byte_range_resolve_result;

typedef enum const_byte_range_resolve_tag {
    CONST_BYTE_RANGE_RESOLVE_OK, CONST_BYTE_RANGE_RESOLVE_INVALID_ARGUMENT, CONST_BYTE_RANGE_RESOLVE_OUT_OF_RANGE
} const_byte_range_resolve_tag;
typedef struct const_byte_range_resolve_result {
    const_byte_range_resolve_tag tag;
    const_byte_slice bytes; /* Borrowed; meaningful only on OK. */
} const_byte_range_resolve_result;

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum byte_slice_subslice_tag {
    BYTE_SLICE_SUBSLICE_OK, BYTE_SLICE_SUBSLICE_INVALID_ARGUMENT, BYTE_SLICE_SUBSLICE_OUT_OF_RANGE
} byte_slice_subslice_tag;
typedef struct byte_slice_subslice_result {
    byte_slice_subslice_tag tag;
} byte_slice_subslice_result;

typedef enum const_byte_slice_subslice_tag {
    CONST_BYTE_SLICE_SUBSLICE_OK, CONST_BYTE_SLICE_SUBSLICE_INVALID_ARGUMENT, CONST_BYTE_SLICE_SUBSLICE_OUT_OF_RANGE
} const_byte_slice_subslice_tag;
typedef struct const_byte_slice_subslice_result {
    const_byte_slice_subslice_tag tag;
} const_byte_slice_subslice_result;

typedef enum byte_slice_copy_range_tag {
    BYTE_SLICE_COPY_RANGE_OK, BYTE_SLICE_COPY_RANGE_INVALID_ARGUMENT, BYTE_SLICE_COPY_RANGE_OUT_OF_RANGE
} byte_slice_copy_range_tag;
typedef struct byte_slice_copy_range_result {
    byte_slice_copy_range_tag tag;
} byte_slice_copy_range_result;

typedef enum byte_slice_copy_tag {
    BYTE_SLICE_COPY_OK, BYTE_SLICE_COPY_INVALID_ARGUMENT, BYTE_SLICE_COPY_OUT_OF_RANGE
} byte_slice_copy_tag;
typedef struct byte_slice_copy_result {
    byte_slice_copy_tag tag;
} byte_slice_copy_result;

