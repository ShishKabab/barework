#include "wasm_memory.h"


#if !defined(__wasm32__)
#error "wasm_memory.c requires a Wasm32 target"
#endif

extern u8 __heap_base;

static usize heap_start;
static usize allocation_end;
static u64 memory_end;

typedef enum wasm_memory_initialize_tag { WASM_MEMORY_INITIALIZE_OK, WASM_MEMORY_INITIALIZE_EXTERNAL_GROWTH } wasm_memory_initialize_tag;
typedef struct wasm_memory_initialize_result { wasm_memory_initialize_tag tag; } wasm_memory_initialize_result;
typedef enum wasm_memory_ensure_tag { WASM_MEMORY_ENSURE_OK, WASM_MEMORY_ENSURE_OUT_OF_MEMORY } wasm_memory_ensure_tag;
typedef struct wasm_memory_ensure_result { wasm_memory_ensure_tag tag; } wasm_memory_ensure_result;
static wasm_memory_initialize_result WasmMemoryInitialize(void)
{
    u64 current_end = (u64)__builtin_wasm_memory_size(0) * WASM_MEMORY_PAGE_SIZE;
    if (heap_start == 0) {
        heap_start = (usize)(uintptr_t)&__heap_base;
        allocation_end = heap_start;
        memory_end = current_end;
    }
    /* Catch growth outside this allocator rather than claiming unknown bytes. */
    return (wasm_memory_initialize_result){.tag = current_end == memory_end ? WASM_MEMORY_INITIALIZE_OK : WASM_MEMORY_INITIALIZE_EXTERNAL_GROWTH};
}

static wasm_memory_ensure_result WasmMemoryEnsure(usize end)
{
    if ((u64)end <= memory_end) {
        return (wasm_memory_ensure_result){.tag = WASM_MEMORY_ENSURE_OK};
    }
    u64 pages = ((u64)end + WASM_MEMORY_PAGE_SIZE - 1) / WASM_MEMORY_PAGE_SIZE;
    usize extra = (usize)(pages - memory_end / WASM_MEMORY_PAGE_SIZE);
    if (__builtin_wasm_memory_grow(0, extra) == SIZE_MAX) {
        return (wasm_memory_ensure_result){.tag = WASM_MEMORY_ENSURE_OUT_OF_MEMORY};
    }
    memory_end = pages * WASM_MEMORY_PAGE_SIZE;
    return (wasm_memory_ensure_result){.tag = WASM_MEMORY_ENSURE_OK};
}

wasm_memory_allocate_result WasmMemoryAllocate(usize size, usize alignment, byte_slice *allocation)
{
    if (allocation == NULL || size == 0 || size > PTRDIFF_MAX || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        return (wasm_memory_allocate_result){.tag = WASM_MEMORY_ALLOCATE_INVALID_ARGUMENT};
    }
    if (WasmMemoryInitialize().tag != WASM_MEMORY_INITIALIZE_OK) return (wasm_memory_allocate_result){.tag = WASM_MEMORY_ALLOCATE_BUSY};
    usize padding = (0 - allocation_end) & (alignment - 1);
    if (padding > SIZE_MAX - allocation_end ||
        size > SIZE_MAX - allocation_end - padding) {
        return (wasm_memory_allocate_result){.tag = WASM_MEMORY_ALLOCATE_OUT_OF_MEMORY};
    }
    usize start = allocation_end + padding;
    usize end = start + size;
    if (WasmMemoryEnsure(end).tag != WASM_MEMORY_ENSURE_OK) return (wasm_memory_allocate_result){.tag = WASM_MEMORY_ALLOCATE_OUT_OF_MEMORY};
    allocation_end = end;
    *allocation = (byte_slice){(u8 *)(uintptr_t)start, size};
    return (wasm_memory_allocate_result){.tag = WASM_MEMORY_ALLOCATE_OK};
}

wasm_memory_extend_result WasmMemoryExtend(byte_slice *allocation, usize new_length)
{
    if (allocation == NULL || allocation->ptr == NULL || allocation->len == 0 ||
        new_length < allocation->len || new_length > PTRDIFF_MAX) {
        return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_INVALID_ARGUMENT};
    }
    if (WasmMemoryInitialize().tag != WASM_MEMORY_INITIALIZE_OK) return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_BUSY};
    usize start = (usize)(uintptr_t)allocation->ptr;
    if (start < heap_start || start > allocation_end ||
        allocation->len > allocation_end - start) {
        return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_INVALID_ARGUMENT};
    }
    if (new_length == allocation->len) {
        return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_OK};
    }
    if (allocation->len != allocation_end - start) {
        return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_BUSY};
    }
    if (new_length > SIZE_MAX - start) {
        return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_OUT_OF_MEMORY};
    }
    if (WasmMemoryEnsure(start + new_length).tag != WASM_MEMORY_ENSURE_OK) return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_OUT_OF_MEMORY};
    allocation_end = start + new_length;
    allocation->len = new_length;
    return (wasm_memory_extend_result){.tag = WASM_MEMORY_EXTEND_OK};
}
