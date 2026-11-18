#pragma once
#include "types.h"

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum wasm_memory_allocate_tag {
    WASM_MEMORY_ALLOCATE_OK, WASM_MEMORY_ALLOCATE_INVALID_ARGUMENT, WASM_MEMORY_ALLOCATE_OUT_OF_MEMORY, WASM_MEMORY_ALLOCATE_BUSY
} wasm_memory_allocate_tag;
typedef struct wasm_memory_allocate_result {
    wasm_memory_allocate_tag tag;
} wasm_memory_allocate_result;

typedef enum wasm_memory_extend_tag {
    WASM_MEMORY_EXTEND_OK, WASM_MEMORY_EXTEND_INVALID_ARGUMENT, WASM_MEMORY_EXTEND_OUT_OF_MEMORY, WASM_MEMORY_EXTEND_BUSY
} wasm_memory_extend_tag;
typedef struct wasm_memory_extend_result {
    wasm_memory_extend_tag tag;
} wasm_memory_extend_result;

