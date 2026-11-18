#ifndef COMMON_WASM_MEMORY_H
#define COMMON_WASM_MEMORY_H
#include "wasm_memory_results.h"

#include "types.h"

enum { WASM_MEMORY_PAGE_SIZE = 65536 };

/* Wasm32 only: one append-only allocator per instance, beginning at __heap_base.
 * The instance must have one owner/thread, with no other heap allocator, malloc,
 * sbrk, host memory growth, or direct memory.grow calls. Memory is never reused,
 * shrunk, or freed; discard the instance when its connection is done. A request
 * arena may reset its own used cursor within retained storage without rewinding
 * this backing allocator or reusing another allocation's range.
 * Returns a function-specific tagged result. Alignment must be a nonzero power of two.
 * Failed calls do not advance the allocation end or change the output range. */
wasm_memory_allocate_result WasmMemoryAllocate(usize size, usize alignment, byte_slice *allocation);
/* Only extends the exact original allocation, and only if its end equals the
 * global allocation end. An intervening allocation returns WASM_MEMORY_EXTEND_BUSY. No moves,
 * copying, gap reuse, or fallback blocks. Shrinking is invalid. */
wasm_memory_extend_result WasmMemoryExtend(byte_slice *allocation, usize new_length);

#endif
