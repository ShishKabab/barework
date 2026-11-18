#ifndef COMMON_INDEX_ARENA_H
#define COMMON_INDEX_ARENA_H
#include "index_arena_results.h"
#include "os.h"

typedef struct indexed_id { u32 index; u32 generation; } indexed_id;
typedef struct index_arena {
    byte_slice storage;
    usize stride;
    usize payload_offset;
    usize payload_size;
    u32 capacity;
    u32 next;
    u32 live;
} index_arena;
/* Controller-owned, fixed-capacity stable slots backed by host virtual memory.
 * IDs are invalidated on release. Release only after I/O and worker references
 * are gone. Generation exhaustion retires a slot instead of wrapping. */
index_arena_create_result IndexArenaCreate(index_arena *arena, u32 capacity, usize size, usize alignment);
index_arena_acquire_result IndexArenaAcquire(index_arena *arena, indexed_id *id, byte_slice *item);
index_arena_get_result IndexArenaGet(index_arena *arena, indexed_id id, byte_slice *item);
index_arena_release_result IndexArenaRelease(index_arena *arena, indexed_id id);
index_arena_destroy_result IndexArenaDestroy(index_arena *arena);
static inline u64 IndexedIdPack(indexed_id id) { return ((u64)id.generation << 32) | id.index; }
static inline indexed_id IndexedIdUnpack(u64 id) { return (indexed_id){(u32)id, (u32)(id >> 32)}; }
#endif
