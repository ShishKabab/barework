#pragma once
#include "types.h"

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum index_arena_create_tag {
    INDEX_ARENA_CREATE_OK, INDEX_ARENA_CREATE_INVALID_ARGUMENT, INDEX_ARENA_CREATE_OVERFLOW, INDEX_ARENA_CREATE_OUT_OF_MEMORY, INDEX_ARENA_CREATE_SYSTEM_ERROR
} index_arena_create_tag;
typedef struct index_arena_create_result {
    index_arena_create_tag tag;
    i32 native_error;
} index_arena_create_result;

typedef enum index_arena_get_tag {
    INDEX_ARENA_GET_OK, INDEX_ARENA_GET_INVALID_ARGUMENT, INDEX_ARENA_GET_STALE_ID
} index_arena_get_tag;
typedef struct index_arena_get_result {
    index_arena_get_tag tag;
} index_arena_get_result;

typedef enum index_arena_acquire_tag {
    INDEX_ARENA_ACQUIRE_OK, INDEX_ARENA_ACQUIRE_INVALID_ARGUMENT, INDEX_ARENA_ACQUIRE_NO_SPACE
} index_arena_acquire_tag;
typedef struct index_arena_acquire_result {
    index_arena_acquire_tag tag;
} index_arena_acquire_result;

typedef enum index_arena_release_tag {
    INDEX_ARENA_RELEASE_OK, INDEX_ARENA_RELEASE_INVALID_ARGUMENT, INDEX_ARENA_RELEASE_STALE_ID
} index_arena_release_tag;
typedef struct index_arena_release_result {
    index_arena_release_tag tag;
} index_arena_release_result;

typedef enum index_arena_destroy_tag {
    INDEX_ARENA_DESTROY_OK, INDEX_ARENA_DESTROY_INVALID_ARGUMENT, INDEX_ARENA_DESTROY_BUSY, INDEX_ARENA_DESTROY_SYSTEM_ERROR
} index_arena_destroy_tag;
typedef struct index_arena_destroy_result {
    index_arena_destroy_tag tag;
    i32 native_error;
} index_arena_destroy_result;

