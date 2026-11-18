#include "bump.h"

#if defined(__wasm32__)
#include "wasm_memory.h"
#else
#include "os.h"
#endif

#include <string.h>

bump_init_result BumpInit(bump_allocator *allocator, byte_slice storage)
{
    if (allocator == NULL || (storage.ptr == NULL && storage.len != 0)) {
        return (bump_init_result){.tag = BUMP_INIT_INVALID_ARGUMENT};
    }
    *allocator = (bump_allocator){.storage = storage, .limit = storage.len,
                                  .committed = storage.len};
    return (bump_init_result){.tag = BUMP_INIT_OK};
}

typedef enum bump_page_size_tag { BUMP_PAGE_SIZE_OK, BUMP_PAGE_SIZE_UNAVAILABLE } bump_page_size_tag;
typedef struct bump_page_size_result { bump_page_size_tag tag; usize value; } bump_page_size_result;
static bump_page_size_result BumpPageSize(void)
{
#if defined(__wasm32__)
    return (bump_page_size_result){.tag = BUMP_PAGE_SIZE_OK, .value = WASM_MEMORY_PAGE_SIZE};
#else
    os_memory_page_size_result queried = OsMemoryPageSize();
    if (queried.tag != OS_MEMORY_PAGE_SIZE_OK) return (bump_page_size_result){.tag = BUMP_PAGE_SIZE_UNAVAILABLE};
    return (bump_page_size_result){.tag = BUMP_PAGE_SIZE_OK, .value = queried.value};
#endif
}

bump_create_result BumpCreate(bump_allocator *allocator, usize limit)
{
    if (allocator == NULL || limit == 0 || limit > PTRDIFF_MAX)
        return (bump_create_result){.tag = BUMP_CREATE_INVALID_ARGUMENT};
    *allocator = (bump_allocator){0};
    byte_slice storage = {0};
#if defined(__wasm32__)
    usize initial = limit < WASM_MEMORY_PAGE_SIZE ? limit : WASM_MEMORY_PAGE_SIZE;
    wasm_memory_allocate_result result = WasmMemoryAllocate(initial, WASM_MEMORY_PAGE_SIZE, &storage);
    switch (result.tag) {
    case WASM_MEMORY_ALLOCATE_OK: break;
    case WASM_MEMORY_ALLOCATE_INVALID_ARGUMENT: return (bump_create_result){.tag = BUMP_CREATE_INVALID_ARGUMENT};
    case WASM_MEMORY_ALLOCATE_OUT_OF_MEMORY: return (bump_create_result){.tag = BUMP_CREATE_OUT_OF_MEMORY};
    case WASM_MEMORY_ALLOCATE_BUSY: return (bump_create_result){.tag = BUMP_CREATE_BUSY};
    }
#else
    os_memory_reserve_result result = OsMemoryReserve(limit, &storage);
    switch (result.tag) {
    case OS_MEMORY_RESERVE_OK: break;
    case OS_MEMORY_RESERVE_INVALID_ARGUMENT: return (bump_create_result){.tag = BUMP_CREATE_INVALID_ARGUMENT};
    case OS_MEMORY_RESERVE_OUT_OF_MEMORY: return (bump_create_result){.tag = BUMP_CREATE_OUT_OF_MEMORY};
    case OS_MEMORY_RESERVE_SYSTEM_ERROR: return (bump_create_result){.tag = BUMP_CREATE_SYSTEM_ERROR, .native_error = result.native_error};
    }
#endif
    allocator->storage = storage;
    allocator->limit = limit;
    allocator->owns_memory = 1;
#if defined(__wasm32__)
    allocator->committed = storage.len;
#endif
    return (bump_create_result){.tag = BUMP_CREATE_OK};
}

bump_destroy_result BumpDestroy(bump_allocator *allocator)
{
    if (allocator == NULL) return (bump_destroy_result){.tag = BUMP_DESTROY_INVALID_ARGUMENT};
#if !defined(__wasm32__)
    if (allocator->owns_memory) {
        os_memory_free_result result = OsMemoryFree(&allocator->storage);
        switch (result.tag) {
        case OS_MEMORY_FREE_OK: break;
        case OS_MEMORY_FREE_INVALID_ARGUMENT: return (bump_destroy_result){.tag = BUMP_DESTROY_INVALID_ARGUMENT};
        case OS_MEMORY_FREE_SYSTEM_ERROR: return (bump_destroy_result){.tag = BUMP_DESTROY_SYSTEM_ERROR, .native_error = result.native_error};
        }
    }
#endif
    *allocator = (bump_allocator){0};
    return (bump_destroy_result){.tag = BUMP_DESTROY_OK};
}

typedef enum bump_ensure_tag { BUMP_ENSURE_OK, BUMP_ENSURE_UNAVAILABLE } bump_ensure_tag;
typedef struct bump_ensure_result { bump_ensure_tag tag; } bump_ensure_result;
static bump_ensure_result BumpEnsure(bump_allocator *allocator, usize required)
{
    if (required <= allocator->committed) {
        return (bump_ensure_result){.tag = BUMP_ENSURE_OK};
    }
    if (!allocator->owns_memory) {
        return (bump_ensure_result){.tag = BUMP_ENSURE_UNAVAILABLE};
    }
    bump_page_size_result queried = BumpPageSize();
    if (queried.tag != BUMP_PAGE_SIZE_OK) return (bump_ensure_result){.tag = BUMP_ENSURE_UNAVAILABLE};
    usize page_size = queried.value;
    if (page_size == 0 || required > SIZE_MAX - (page_size - 1)) {
        return (bump_ensure_result){.tag = BUMP_ENSURE_UNAVAILABLE};
    }
    usize end = ((required + page_size - 1) / page_size) * page_size;
#if defined(__wasm32__)
    if (end > allocator->limit) {
        end = allocator->limit;
    }
    wasm_memory_extend_result result = WasmMemoryExtend(&allocator->storage, end);
#else
    byte_slice pages = {allocator->storage.ptr + allocator->committed,
                        end - allocator->committed};
    os_memory_commit_result result = OsMemoryCommit(pages);
#endif
#if defined(__wasm32__)
    i32 succeeded = result.tag == WASM_MEMORY_EXTEND_OK;
#else
    i32 succeeded = result.tag == OS_MEMORY_COMMIT_OK;
#endif
    if (succeeded) allocator->committed = end;
    return (bump_ensure_result){.tag = succeeded ? BUMP_ENSURE_OK : BUMP_ENSURE_UNAVAILABLE};
}

void BumpReset(bump_allocator *allocator)
{
    allocator->used = 0;
}

bump_allocate_result BumpAllocate(bump_allocator *allocator, usize size, usize alignment)
{
    if (allocator == NULL || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0 || allocator->storage.ptr == NULL ||
        allocator->used > allocator->limit || allocator->used > allocator->committed) {
        return (bump_allocate_result){.tag = BUMP_ALLOCATE_INVALID_ARGUMENT};
    }
    uintptr_t address = (uintptr_t)(allocator->storage.ptr + allocator->used);
    usize padding = (usize)((0 - address) & (alignment - 1));
    usize remaining = allocator->limit - allocator->used;
    if (padding > remaining || size > remaining - padding) {
        return (bump_allocate_result){.tag = BUMP_ALLOCATE_NO_SPACE};
    }
    if (BumpEnsure(allocator, allocator->used + padding + size).tag != BUMP_ENSURE_OK) {
        return (bump_allocate_result){.tag = BUMP_ALLOCATE_BACKING_UNAVAILABLE};
    }
    byte_slice result = {allocator->storage.ptr + allocator->used + padding, size};
    memset(allocator->storage.ptr + allocator->used, 0, padding + size);
    allocator->used += padding + size;
    return (bump_allocate_result){.tag = BUMP_ALLOCATE_OK, .allocation = result};
}
