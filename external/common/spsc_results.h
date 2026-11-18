#pragma once
#include "types.h"

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum spsc_init_tag {
    SPSC_INIT_OK, SPSC_INIT_INVALID_ARGUMENT, SPSC_INIT_NOT_SUPPORTED
} spsc_init_tag;
typedef struct spsc_init_result {
    spsc_init_tag tag;
} spsc_init_result;

typedef enum spsc_write_tag {
    SPSC_WRITE_OK, SPSC_WRITE_INVALID_ARGUMENT
} spsc_write_tag;
typedef struct spsc_write_result {
    spsc_write_tag tag;
    usize value; /* Only valid for SPSC_WRITE_OK. */
} spsc_write_result;

typedef enum spsc_read_tag {
    SPSC_READ_OK, SPSC_READ_INVALID_ARGUMENT
} spsc_read_tag;
typedef struct spsc_read_result {
    spsc_read_tag tag;
    usize value; /* Only valid for SPSC_READ_OK. */
} spsc_read_result;

