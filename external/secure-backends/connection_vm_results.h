#pragma once
#include "types.h"

typedef enum connection_vm_memory_size_tag { CONNECTION_VM_MEMORY_SIZE_OK, CONNECTION_VM_MEMORY_SIZE_INVALID_ARGUMENT, CONNECTION_VM_MEMORY_SIZE_WRONG_THREAD } connection_vm_memory_size_tag;
typedef struct connection_vm_memory_size_result { connection_vm_memory_size_tag tag; usize value; } connection_vm_memory_size_result;

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum connection_runtime_create_tag {
    CONNECTION_RUNTIME_CREATE_OK, CONNECTION_RUNTIME_CREATE_INVALID_ARGUMENT, CONNECTION_RUNTIME_CREATE_OUT_OF_MEMORY, CONNECTION_RUNTIME_CREATE_INVALID_MODULE
} connection_runtime_create_tag;
typedef struct connection_runtime_create_result {
    connection_runtime_create_tag tag;
} connection_runtime_create_result;

typedef enum connection_vm_open_tag {
    CONNECTION_VM_OPEN_OK, CONNECTION_VM_OPEN_INVALID_ARGUMENT, CONNECTION_VM_OPEN_OUT_OF_MEMORY, CONNECTION_VM_OPEN_INVALID_GUEST, CONNECTION_VM_OPEN_SYSTEM_ERROR
} connection_vm_open_tag;
typedef struct connection_vm_open_result {
    connection_vm_open_tag tag;
    i32 native_error;
} connection_vm_open_result;

typedef enum connection_vm_step_tag {
    CONNECTION_VM_STEP_OK, CONNECTION_VM_STEP_INVALID_ARGUMENT, CONNECTION_VM_STEP_WRONG_THREAD, CONNECTION_VM_STEP_INVALID_GUEST, CONNECTION_VM_STEP_OUT_OF_MEMORY
} connection_vm_step_tag;
typedef struct connection_vm_step_result {
    connection_vm_step_tag tag;
} connection_vm_step_result;

typedef enum connection_vm_close_tag {
    CONNECTION_VM_CLOSE_OK, CONNECTION_VM_CLOSE_WRONG_THREAD, CONNECTION_VM_CLOSE_INVALID_STATE, CONNECTION_VM_CLOSE_SYSTEM_ERROR
} connection_vm_close_tag;
typedef struct connection_vm_close_result {
    connection_vm_close_tag tag;
    i32 native_error;
} connection_vm_close_result;

typedef enum connection_message_validate_tag {
    CONNECTION_MESSAGE_VALIDATE_OK, CONNECTION_MESSAGE_VALIDATE_INVALID_MESSAGE
} connection_message_validate_tag;
typedef struct connection_message_validate_result {
    connection_message_validate_tag tag;
} connection_message_validate_result;

