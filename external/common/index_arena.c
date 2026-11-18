#include "index_arena.h"
#include "slice.h"
#include <string.h>

typedef struct index_slot { u32 generation; u32 occupied; } index_slot;

static index_slot *IndexSlot(index_arena *arena, u32 index)
{
    byte_slice bytes;
    if (index >= arena->capacity || ByteSliceSubslice(arena->storage,
        (usize)index * arena->stride, arena->stride, &bytes).tag != BYTE_SLICE_SUBSLICE_OK) return NULL;
    return (index_slot *)bytes.ptr;
}

index_arena_create_result IndexArenaCreate(index_arena *arena, u32 capacity, usize size, usize alignment)
{
    os_memory_page_size_result queried = OsMemoryPageSize();
    if (queried.tag != OS_MEMORY_PAGE_SIZE_OK)
        return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_SYSTEM_ERROR, .native_error = queried.native_error};
    if (arena == NULL || capacity == 0 || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0 || alignment > queried.value)
        return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_INVALID_ARGUMENT};
    *arena = (index_arena){0};
    if (alignment < _Alignof(index_slot)) alignment = _Alignof(index_slot);
    usize offset = (sizeof(index_slot) + alignment - 1) & ~(alignment - 1);
    if (size > SIZE_MAX - offset - (alignment - 1)) return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_OVERFLOW};
    usize stride = (offset + size + alignment - 1) & ~(alignment - 1);
    if (capacity > SIZE_MAX / stride) return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_OVERFLOW};
    os_memory_reserve_result reserved = OsMemoryReserve(stride * capacity, &arena->storage);
    switch (reserved.tag) {
    case OS_MEMORY_RESERVE_OK: break;
    case OS_MEMORY_RESERVE_INVALID_ARGUMENT: return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_INVALID_ARGUMENT};
    case OS_MEMORY_RESERVE_OUT_OF_MEMORY: return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_OUT_OF_MEMORY};
    case OS_MEMORY_RESERVE_SYSTEM_ERROR: return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_SYSTEM_ERROR, .native_error = reserved.native_error};
    }
    os_memory_commit_result committed = OsMemoryCommit(arena->storage);
    if (committed.tag != OS_MEMORY_COMMIT_OK) {
        OsMemoryFree(&arena->storage);
        return (index_arena_create_result){.tag = committed.tag == OS_MEMORY_COMMIT_INVALID_ARGUMENT ?
            INDEX_ARENA_CREATE_INVALID_ARGUMENT : INDEX_ARENA_CREATE_SYSTEM_ERROR, .native_error = committed.native_error};
    }
    arena->stride = stride;
    arena->payload_offset = offset;
    arena->payload_size = size;
    arena->capacity = capacity;
    return (index_arena_create_result){.tag = INDEX_ARENA_CREATE_OK};
}

index_arena_get_result IndexArenaGet(index_arena *arena, indexed_id id, byte_slice *item)
{
    if (arena == NULL || item == NULL) return (index_arena_get_result){.tag = INDEX_ARENA_GET_INVALID_ARGUMENT};
    index_slot *slot = IndexSlot(arena, id.index);
    if (slot == NULL || !slot->occupied || slot->generation != id.generation) return (index_arena_get_result){.tag = INDEX_ARENA_GET_STALE_ID};
    byte_slice_subslice_result bounds = ByteSliceSubslice(arena->storage,
        (usize)id.index * arena->stride + arena->payload_offset, arena->payload_size, item);
    return (index_arena_get_result){.tag = bounds.tag == BYTE_SLICE_SUBSLICE_OK ?
        INDEX_ARENA_GET_OK : INDEX_ARENA_GET_INVALID_ARGUMENT};
}

index_arena_acquire_result IndexArenaAcquire(index_arena *arena, indexed_id *id, byte_slice *item)
{
    if (arena == NULL || id == NULL || item == NULL) return (index_arena_acquire_result){.tag = INDEX_ARENA_ACQUIRE_INVALID_ARGUMENT};
    if (arena->live == arena->capacity) return (index_arena_acquire_result){.tag = INDEX_ARENA_ACQUIRE_NO_SPACE};
    for (u32 scanned = 0; scanned < arena->capacity; ++scanned) {
        u32 index = arena->next;
        arena->next = (index + 1) % arena->capacity;
        index_slot *slot = IndexSlot(arena, index);
        if (slot->occupied || slot->generation == UINT32_MAX) continue;
        ++slot->generation;
        slot->occupied = 1;
        ++arena->live;
        *id = (indexed_id){index, slot->generation};
        if (IndexArenaGet(arena, *id, item).tag != INDEX_ARENA_GET_OK) {
            slot->occupied = 0;
            --arena->live;
            return (index_arena_acquire_result){.tag = INDEX_ARENA_ACQUIRE_INVALID_ARGUMENT};
        }
        /* Slot initialization is a bounded low-level storage operation. */
        memset(item->ptr, 0, item->len);
        return (index_arena_acquire_result){.tag = INDEX_ARENA_ACQUIRE_OK};
    }
    return (index_arena_acquire_result){.tag = INDEX_ARENA_ACQUIRE_NO_SPACE};
}

index_arena_release_result IndexArenaRelease(index_arena *arena, indexed_id id)
{
    byte_slice item;
    switch (IndexArenaGet(arena, id, &item).tag) {
    case INDEX_ARENA_GET_OK: break;
    case INDEX_ARENA_GET_INVALID_ARGUMENT: return (index_arena_release_result){.tag = INDEX_ARENA_RELEASE_INVALID_ARGUMENT};
    case INDEX_ARENA_GET_STALE_ID: return (index_arena_release_result){.tag = INDEX_ARENA_RELEASE_STALE_ID};
    }
    IndexSlot(arena, id.index)->occupied = 0;
    --arena->live;
    return (index_arena_release_result){.tag = INDEX_ARENA_RELEASE_OK};
}

index_arena_destroy_result IndexArenaDestroy(index_arena *arena)
{
    if (arena == NULL) return (index_arena_destroy_result){.tag = INDEX_ARENA_DESTROY_INVALID_ARGUMENT};
    if (arena->live != 0) return (index_arena_destroy_result){.tag = INDEX_ARENA_DESTROY_BUSY};
    os_memory_free_result released = OsMemoryFree(&arena->storage);
    switch (released.tag) {
    case OS_MEMORY_FREE_OK: *arena = (index_arena){0}; return (index_arena_destroy_result){.tag = INDEX_ARENA_DESTROY_OK};
    case OS_MEMORY_FREE_INVALID_ARGUMENT: return (index_arena_destroy_result){.tag = INDEX_ARENA_DESTROY_INVALID_ARGUMENT};
    case OS_MEMORY_FREE_SYSTEM_ERROR: return (index_arena_destroy_result){.tag = INDEX_ARENA_DESTROY_SYSTEM_ERROR, .native_error = released.native_error};
    }
    __builtin_unreachable();
}
