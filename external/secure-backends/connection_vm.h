#ifndef SECURE_CONNECTION_VM_H
#define SECURE_CONNECTION_VM_H
#include "connection_vm_results.h"
#include "connection_protocol.h"
#include "bump.h"

typedef struct connection_runtime connection_runtime;
typedef struct connection_vm connection_vm;
typedef struct connection_delivery {
    u32 type;
    u32 flags;
    usize consumed;
    const_byte_slice request;
    const_byte_slice output;
} connection_delivery;
/* Runtime/module is immutable after creation and may be shared by workers.
 * A VM belongs to the thread that opens it, until close; no migration. */
connection_runtime_create_result ConnectionRuntimeCreate(const_byte_slice module, connection_runtime **runtime);
void ConnectionRuntimeDestroy(connection_runtime *runtime);
connection_vm_open_result ConnectionVmOpen(connection_runtime *runtime, connection_vm **vm, connection_delivery *delivery);
connection_vm_step_result ConnectionVmStep(connection_vm *vm, u32 type, const_byte_slice input, connection_delivery *delivery);
connection_vm_close_result ConnectionVmClose(connection_vm *vm);
/* Diagnostic for bounded-memory/reset tests. Owner thread only. */
connection_vm_memory_size_result ConnectionVmMemorySize(connection_vm *vm);
/* Output bytes are valid until the next Step/Close. Copied request bytes remain
 * valid until a subsequent Step emits another HTTP request, or Close. In
 * particular they survive CONNECTION_RESPONSE on the owner while a business
 * job may borrow them before that response. No guest pointer escapes.
 * Structural validation is not an authentication or authorization decision. */
connection_message_validate_result ConnectionMessageValidate(const_byte_slice message);
#endif
