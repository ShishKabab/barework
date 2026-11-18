#ifndef COMMON_BUMP_H
#define COMMON_BUMP_H
#include "bump_results.h"

#include "types.h"

typedef struct bump_allocator {
    byte_slice storage;
    usize used;
    usize limit;
    usize committed;
    i32 owns_memory;
} bump_allocator;

/* Caller owns storage. Init/Create require a fresh or destroyed allocator;
 * destroy an owning allocator before reinitializing it. All old allocations
 * must be dead before destroy or reinitialization. */
bump_init_result BumpInit(bump_allocator *allocator, byte_slice storage);
/* Owns a contiguous arena with an exact allocation limit. Host: reserves the
 * whole range, committing pages on demand. Wasm: allocates an initial prefix
 * from the global allocator, extending it only while it remains the last
 * allocation. Failure never moves existing allocations or changes used.
 * Returns a function-specific tagged result. Do not copy a live owning allocator.
 * Do not read storage beyond committed; storage.len may include reserved pages. */
bump_create_result BumpCreate(bump_allocator *allocator, usize limit);
/* Invalidates every allocation. Frees host reservations; Wasm only forgets the
 * handle and never rewinds the global allocator. Borrowed storage is not freed. */
bump_destroy_result BumpDestroy(bump_allocator *allocator);
/* Invalidates allocations without clearing bytes or releasing backing storage.
 * All previous references must be dead. Does not rewind the Wasm allocator. */
void BumpReset(bump_allocator *allocator);
/* Alignment must be a nonzero power of two, size must be nonzero. Failures do
 * not advance used. OK carries the allocation; bytes and padding are zeroed. */
bump_allocate_result BumpAllocate(bump_allocator *allocator, usize size, usize alignment);

#endif
