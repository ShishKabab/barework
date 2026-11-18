#pragma once
#include "types.h"

typedef enum bump_allocate_tag {
    BUMP_ALLOCATE_OK, BUMP_ALLOCATE_INVALID_ARGUMENT,
    BUMP_ALLOCATE_NO_SPACE, BUMP_ALLOCATE_BACKING_UNAVAILABLE,
} bump_allocate_tag;
typedef struct bump_allocate_result {
    bump_allocate_tag tag;
    byte_slice allocation; /* Only valid on OK. */
} bump_allocate_result;

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum bump_init_tag {
    BUMP_INIT_OK, BUMP_INIT_INVALID_ARGUMENT
} bump_init_tag;
typedef struct bump_init_result {
    bump_init_tag tag;
} bump_init_result;

typedef enum bump_create_tag {
    BUMP_CREATE_OK, BUMP_CREATE_INVALID_ARGUMENT, BUMP_CREATE_OUT_OF_MEMORY, BUMP_CREATE_BUSY, BUMP_CREATE_SYSTEM_ERROR
} bump_create_tag;
typedef struct bump_create_result {
    bump_create_tag tag;
    i32 native_error;
} bump_create_result;

typedef enum bump_destroy_tag {
    BUMP_DESTROY_OK, BUMP_DESTROY_INVALID_ARGUMENT, BUMP_DESTROY_SYSTEM_ERROR
} bump_destroy_tag;
typedef struct bump_destroy_result {
    bump_destroy_tag tag;
    i32 native_error;
} bump_destroy_result;

