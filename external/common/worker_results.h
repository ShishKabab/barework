#pragma once
#include "types.h"

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum worker_pool_create_tag {
    WORKER_POOL_CREATE_OK, WORKER_POOL_CREATE_INVALID_ARGUMENT, WORKER_POOL_CREATE_OUT_OF_MEMORY, WORKER_POOL_CREATE_OVERFLOW, WORKER_POOL_CREATE_NOT_SUPPORTED, WORKER_POOL_CREATE_SYSTEM_ERROR
} worker_pool_create_tag;
typedef struct worker_pool_create_result {
    worker_pool_create_tag tag;
    i32 native_error;
} worker_pool_create_result;

typedef enum worker_submit_tag {
    WORKER_SUBMIT_OK, WORKER_SUBMIT_INVALID_ARGUMENT, WORKER_SUBMIT_WOULD_BLOCK, WORKER_SUBMIT_BROKEN
} worker_submit_tag;
typedef struct worker_submit_result {
    worker_submit_tag tag;
} worker_submit_result;

typedef enum worker_receive_tag {
    WORKER_RECEIVE_OK, WORKER_RECEIVE_INVALID_ARGUMENT, WORKER_RECEIVE_EMPTY, WORKER_RECEIVE_BROKEN
} worker_receive_tag;
typedef struct worker_receive_result {
    worker_receive_tag tag;
} worker_receive_result;

typedef enum worker_pool_destroy_tag {
    WORKER_POOL_DESTROY_OK, WORKER_POOL_DESTROY_BUSY, WORKER_POOL_DESTROY_SYSTEM_ERROR
} worker_pool_destroy_tag;
typedef struct worker_pool_destroy_result {
    worker_pool_destroy_tag tag;
    i32 native_error;
} worker_pool_destroy_result;

