#include "connection_vm.h"
#include "http.h"
#include "os.h"
#include "slice.h"
#include "business_protocol.h"
#include "wasm_pool.h"
#include "app-routes.h"
#include <wasmtime.h>
#include <stdlib.h>

struct connection_runtime { wasm_engine_t *engine; wasmtime_module_t *module; };
struct connection_vm {
    wasmtime_store_t *store;
    wasmtime_context_t *context;
    wasmtime_memory_t memory;
    wasmtime_func_t step;
    u32 control_offset;
    wasm_slice input;
    wasm_slice response_input;
    u64 owner;
    u32 allowed;
    bump_allocator copied_request;
    u8 output_storage[4096];
};

static void RuntimeError(wasmtime_error_t *error, wasm_trap_t *trap)
{
    if (error != NULL) wasmtime_error_delete(error);
    if (trap != NULL) wasm_trap_delete(trap);
}

static byte_slice Memory(connection_vm *vm)
{
    return (byte_slice){wasmtime_memory_data(vm->context, &vm->memory),
                       wasmtime_memory_data_size(vm->context, &vm->memory)};
}

static bool FieldIsValid(const_byte_slice message, u32 offset, u32 length)
{
    const_byte_slice field;
    if (length != 0 && offset < sizeof(http_request)) return false;
    return ConstByteSliceSubslice(message, offset, length, &field).tag == CONST_BYTE_SLICE_SUBSLICE_OK;
}

connection_message_validate_result ConnectionMessageValidate(const_byte_slice message)
{
    http_request request;
    if (message.len > CONNECTION_MESSAGE_LIMIT ||
        ByteSliceCopyRange((byte_slice){(u8 *)&request, sizeof(request)}, 0, message, 0, sizeof(request)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        request.MessageLength != message.len || request.VersionMajor != 1 || request.VersionMinor > 1 ||
        request.MethodLength == 0 || request.TargetLength == 0 || request.HeaderCount > 128 ||
        !FieldIsValid(message, request.MethodOffset, request.MethodLength) ||
        !FieldIsValid(message, request.TargetOffset, request.TargetLength)) return (connection_message_validate_result){.tag = CONNECTION_MESSAGE_VALIDATE_INVALID_MESSAGE};
    u32 offset = request.FirstHeaderOffset;
    for (u32 index = 0; index < request.HeaderCount; ++index) {
        http_header header;
        if (offset < sizeof(http_request) ||
            ByteSliceCopyRange((byte_slice){(u8 *)&header, sizeof(header)}, 0, message, offset, sizeof(header)).tag != BYTE_SLICE_COPY_RANGE_OK ||
            header.KeyLength == 0 || !FieldIsValid(message, header.KeyOffset, header.KeyLength) ||
            !FieldIsValid(message, header.ValueOffset, header.ValueLength)) return (connection_message_validate_result){.tag = CONNECTION_MESSAGE_VALIDATE_INVALID_MESSAGE};
        /* Parser emits descriptors in increasing order. Reject cycles/overlap. */
        if (header.NextHeaderOffset != 0 && header.NextHeaderOffset < offset + sizeof(header)) return (connection_message_validate_result){.tag = CONNECTION_MESSAGE_VALIDATE_INVALID_MESSAGE};
        offset = header.NextHeaderOffset;
    }
    return (connection_message_validate_result){.tag = offset == 0 ? CONNECTION_MESSAGE_VALIDATE_OK : CONNECTION_MESSAGE_VALIDATE_INVALID_MESSAGE};
}

connection_runtime_create_result ConnectionRuntimeCreate(const_byte_slice module, connection_runtime **output)
{
    if (output == NULL || module.ptr == NULL || module.len == 0) return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_INVALID_ARGUMENT};
    *output = NULL;
    connection_runtime *runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_OUT_OF_MEMORY};
    wasm_config_t *config = wasm_config_new();
    if (config == NULL) { free(runtime); return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_OUT_OF_MEMORY}; }
    wasmtime_config_consume_fuel_set(config, true);
    wasmtime_config_wasm_threads_set(config, false);
    wasmtime_config_parallel_compilation_set(config, false);
    wasmtime_config_memory_reservation_set(config, CONNECTION_MEMORY_LIMIT);
    if (WasmPoolConfigure(config, CONNECTION_MEMORY_LIMIT).tag != WASM_POOL_CONFIGURE_OK) {
        wasm_config_delete(config);
        free(runtime);
        return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_OUT_OF_MEMORY};
    }
    runtime->engine = wasm_engine_new_with_config(config);
    if (runtime->engine == NULL) { free(runtime); return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_OUT_OF_MEMORY}; }
    wasmtime_error_t *error = wasmtime_module_new(runtime->engine, module.ptr, module.len, &runtime->module);
    if (error != NULL) {
        RuntimeError(error, NULL);
        ConnectionRuntimeDestroy(runtime);
        return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_INVALID_MODULE};
    }
    wasm_importtype_vec_t imports;
    wasmtime_module_imports(runtime->module, &imports);
    usize count = imports.size;
    wasm_importtype_vec_delete(&imports);
    if (count != 0) { ConnectionRuntimeDestroy(runtime); return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_INVALID_MODULE}; }
    *output = runtime;
    return (connection_runtime_create_result){.tag = CONNECTION_RUNTIME_CREATE_OK};
}

void ConnectionRuntimeDestroy(connection_runtime *runtime)
{
    if (runtime == NULL) return;
    if (runtime->module != NULL) wasmtime_module_delete(runtime->module);
    if (runtime->engine != NULL) wasm_engine_delete(runtime->engine);
    free(runtime);
}

typedef enum read_delivery_tag { READ_DELIVERY_OK, READ_DELIVERY_INVALID_GUEST, READ_DELIVERY_OUT_OF_MEMORY } read_delivery_tag;
typedef struct read_delivery_result { read_delivery_tag tag; } read_delivery_result;
static read_delivery_result ReadDelivery(connection_vm *vm, usize input_length, connection_delivery *delivery)
{
    byte_slice bytes = Memory(vm);
    const_byte_slice memory = {bytes.ptr, bytes.len};
    connection_control control;
    if (ByteSliceCopyRange((byte_slice){(u8 *)&control, sizeof(control)}, 0,
                           memory, vm->control_offset, sizeof(control)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        control.abi_version != CONNECTION_ABI_VERSION || control.struct_size != sizeof(control) ||
        control.output_type != CONNECTION_REQUEST || control.result.tag != CONNECTION_STEP_OK ||
        control.input.ptr != vm->input.ptr || control.input.len != vm->input.len ||
        control.response_input.ptr != vm->response_input.ptr || control.response_input.len != vm->response_input.len ||
        control.consumed != input_length || control.actions.len == 0 ||
        control.actions.len > CONNECTION_MAX_ACTIONS) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
    const_byte_slice actions;
    if (ConstByteSliceSubslice(memory, control.actions.ptr,
        (usize)control.actions.len * sizeof(connection_action), &actions).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
    *delivery = (connection_delivery){.type = CONNECTION_REQUEST, .consumed = control.consumed};
    SliceFor(control.actions, index) {
        connection_action action;
        if (ByteSliceCopyRange((byte_slice){(u8 *)&action, sizeof(action)}, 0, actions,
            (usize)index * sizeof(action), sizeof(action)).tag != BYTE_SLICE_COPY_RANGE_OK) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
        const_byte_slice data;
        if (ConstByteSliceSubslice(memory, action.data.ptr, action.data.len, &data).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
        switch (action.type) {
        case CONNECTION_NEED_INPUT:
            if (delivery->flags != 0 || data.len != 0) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
            delivery->flags |= CONNECTION_WANT_INPUT;
            break;
        case CONNECTION_ROUTED_REQUEST:
        case CONNECTION_HTTP_REQUEST: {
            const_byte_slice http = data;
            if (action.type == CONNECTION_ROUTED_REQUEST) {
                app_route_payload payload;
                route_request_read_result read = RouteRequestRead(data, &payload);
                if (read.tag != ROUTE_REQUEST_READ_OK) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
                http = read.http;
            }
            if (delivery->flags != 0 || ConnectionMessageValidate(http).tag != CONNECTION_MESSAGE_VALIDATE_OK) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
            BumpReset(&vm->copied_request);
            {
                bump_allocate_result allocated = BumpAllocate(&vm->copied_request, data.len, _Alignof(http_request));
                if (allocated.tag != BUMP_ALLOCATE_OK) return (read_delivery_result){.tag = READ_DELIVERY_OUT_OF_MEMORY};
                byte_slice copy = allocated.allocation;
                if (ByteSliceCopy(copy, 0, data).tag != BYTE_SLICE_COPY_OK) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
                delivery->request = (const_byte_slice){copy.ptr, copy.len};
            }
            delivery->flags |= CONNECTION_HAS_REQUEST;
            break;
        }
        case CONNECTION_SEND_BYTES:
            if ((delivery->flags & ~CONNECTION_HAS_REQUEST) != 0 || data.len == 0 ||
                ByteSliceCopy((byte_slice){vm->output_storage, sizeof(vm->output_storage)}, 0, data).tag != BYTE_SLICE_COPY_OK) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
            delivery->output = (const_byte_slice){vm->output_storage, data.len};
            delivery->flags |= CONNECTION_WANT_SEND;
            break;
        case CONNECTION_CLOSE_REQUEST:
            if (delivery->flags != 0 || data.len != 0) return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
            delivery->flags |= CONNECTION_WANT_CLOSE;
            break;
        default:
            return (read_delivery_result){.tag = READ_DELIVERY_INVALID_GUEST};
        }
    }
    vm->allowed = delivery->flags;
    return (read_delivery_result){.tag = READ_DELIVERY_OK};
}

typedef enum export_tag { EXPORT_OK, EXPORT_NOT_FOUND, EXPORT_WRONG_KIND } export_tag;
typedef struct export_result { export_tag tag; } export_result;
static export_result Export(connection_vm *vm, wasmtime_instance_t *instance, string_slice name,
                   wasmtime_extern_kind_t kind, wasmtime_extern_t *item)
{
    if (!wasmtime_instance_export_get(vm->context, instance, name.ptr, name.len, item)) return (export_result){.tag = EXPORT_NOT_FOUND};
    if (item->kind != kind) { wasmtime_extern_delete(item); return (export_result){.tag = EXPORT_WRONG_KIND}; }
    return (export_result){.tag = EXPORT_OK};
}

connection_vm_open_result ConnectionVmOpen(connection_runtime *runtime, connection_vm **output, connection_delivery *delivery)
{
    if (runtime == NULL || output == NULL || delivery == NULL) return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_INVALID_ARGUMENT};
    *output = NULL;
    connection_vm *vm = calloc(1, sizeof(*vm));
    if (vm == NULL) return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_OUT_OF_MEMORY};
    vm->owner = ThreadCurrentId();
    vm->store = wasmtime_store_new(runtime->engine, NULL, NULL);
    if (vm->store == NULL) { free(vm); return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_OUT_OF_MEMORY}; }
    vm->context = wasmtime_store_context(vm->store);
    wasmtime_store_limiter(vm->store, CONNECTION_MEMORY_LIMIT, 64, 1, 1, 1);
    bump_create_result arena = BumpCreate(&vm->copied_request, CONNECTION_DISPATCH_LIMIT);
    if (arena.tag != BUMP_CREATE_OK) {
        ConnectionVmClose(vm);
        switch (arena.tag) {
        case BUMP_CREATE_OUT_OF_MEMORY: return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_OUT_OF_MEMORY};
        case BUMP_CREATE_SYSTEM_ERROR: return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_SYSTEM_ERROR, .native_error = arena.native_error};
        case BUMP_CREATE_INVALID_ARGUMENT:
        case BUMP_CREATE_BUSY:
        case BUMP_CREATE_OK: return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_INVALID_ARGUMENT};
        }
    }
    wasmtime_error_t *failure = wasmtime_context_set_fuel(vm->context, 100000000);
    if (failure != NULL) { RuntimeError(failure, NULL); ConnectionVmClose(vm); return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_INVALID_GUEST}; }
    wasmtime_instance_t instance;
    wasm_trap_t *trap = NULL;
    failure = wasmtime_instance_new(vm->context, runtime->module, NULL, 0, &instance, &trap);
    if (failure != NULL || trap != NULL) { RuntimeError(failure, trap); ConnectionVmClose(vm); return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_INVALID_GUEST}; }
    wasmtime_extern_t item;
    if (Export(vm, &instance, StringSlice("memory"), WASMTIME_EXTERN_MEMORY, &item).tag != EXPORT_OK) goto invalid;
    vm->memory = item.of.memory;
    wasmtime_extern_delete(&item);
    if (Export(vm, &instance, StringSlice("ConnectionStep"), WASMTIME_EXTERN_FUNC, &item).tag != EXPORT_OK) goto invalid;
    vm->step = item.of.func;
    wasmtime_extern_delete(&item);
    if (Export(vm, &instance, StringSlice("ConnectionInit"), WASMTIME_EXTERN_FUNC, &item).tag != EXPORT_OK) goto invalid;
    wasmtime_func_t init = item.of.func;
    wasmtime_extern_delete(&item);
    wasmtime_val_t argument = {.kind = WASMTIME_I32, .of.i32 = CONNECTION_ABI_VERSION};
    wasmtime_val_t result;
    failure = wasmtime_func_call(vm->context, &init, &argument, 1, &result, 1, &trap);
    if (failure != NULL || trap != NULL) { RuntimeError(failure, trap); goto invalid; }
    if (result.kind != WASMTIME_I32 || result.of.i32 == 0) goto invalid;
    byte_slice bytes = Memory(vm);
    connection_init_result initialized;
    if (ByteSliceCopyRange((byte_slice){(u8 *)&initialized, sizeof(initialized)}, 0,
        (const_byte_slice){bytes.ptr, bytes.len}, (u32)result.of.i32, sizeof(initialized)).tag != BYTE_SLICE_COPY_RANGE_OK)
        goto invalid;
    if (initialized.tag == CONNECTION_INIT_NO_SPACE) {
        ConnectionVmClose(vm);
        return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_OUT_OF_MEMORY};
    }
    if (initialized.tag != CONNECTION_INIT_OK) goto invalid;
    vm->control_offset = initialized.control_offset;
    connection_control control;
    if (ByteSliceCopyRange((byte_slice){(u8 *)&control, sizeof(control)}, 0,
        (const_byte_slice){bytes.ptr, bytes.len}, vm->control_offset, sizeof(control)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        control.input.len != CONNECTION_INPUT_SIZE) goto invalid;
    byte_slice window;
    if (ByteSliceSubslice(bytes, control.input.ptr, control.input.len, &window).tag != BYTE_SLICE_SUBSLICE_OK) goto invalid;
    vm->input = control.input;
    if (control.response_input.len != BUSINESS_RESPONSE_LIMIT ||
        ByteSliceSubslice(bytes, control.response_input.ptr, control.response_input.len, &window).tag != BYTE_SLICE_SUBSLICE_OK) goto invalid;
    vm->response_input = control.response_input;
    read_delivery_result read = ReadDelivery(vm, 0, delivery);
    if (read.tag == READ_DELIVERY_OUT_OF_MEMORY) {
        ConnectionVmClose(vm);
        return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_OUT_OF_MEMORY};
    }
    if (read.tag != READ_DELIVERY_OK || delivery->flags != CONNECTION_WANT_INPUT) goto invalid;
    *output = vm;
    return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_OK};
invalid:
    ConnectionVmClose(vm);
    return (connection_vm_open_result){.tag = CONNECTION_VM_OPEN_INVALID_GUEST};
}

connection_vm_step_result ConnectionVmStep(connection_vm *vm, u32 type, const_byte_slice input, connection_delivery *delivery)
{
    if (vm == NULL || delivery == NULL || (input.ptr == NULL && input.len != 0)) return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_INVALID_ARGUMENT};
    if (vm->owner != ThreadCurrentId()) return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_WRONG_THREAD};
    bool business = type == CONNECTION_RESPONSE || type == CONNECTION_BUSINESS_FAILED;
    wasm_slice destination = business ? vm->response_input : vm->input;
    if ((type == CONNECTION_RECEIVE && (vm->allowed != CONNECTION_WANT_INPUT || input.len == 0)) ||
        (type == CONNECTION_EOF && (vm->allowed != CONNECTION_WANT_INPUT || input.len != 0)) ||
        (type == CONNECTION_SEND_COMPLETE && (!(vm->allowed & CONNECTION_WANT_SEND) || input.len != 0)) ||
        (business && (vm->allowed != CONNECTION_HAS_REQUEST ||
            (type == CONNECTION_RESPONSE ? input.len == 0 : input.len != 0))) ||
        (type != CONNECTION_RECEIVE && type != CONNECTION_EOF && type != CONNECTION_SEND_COMPLETE && !business) ||
        input.len > destination.len) return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_INVALID_ARGUMENT};
    byte_slice memory = Memory(vm), window;
    if (ByteSliceSubslice(memory, destination.ptr, destination.len, &window).tag != BYTE_SLICE_SUBSLICE_OK ||
        ByteSliceCopy(window, 0, input).tag != BYTE_SLICE_COPY_OK) return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_INVALID_GUEST};
    u32 length = (u32)input.len;
    byte_slice control;
    if (ByteSliceSubslice(memory, vm->control_offset, sizeof(connection_control), &control).tag != BYTE_SLICE_SUBSLICE_OK ||
        ByteSliceCopy(control, offsetof(connection_control, input_type), BytesOf(type)).tag != BYTE_SLICE_COPY_OK ||
        ByteSliceCopy(control, offsetof(connection_control, input_length), BytesOf(length)).tag != BYTE_SLICE_COPY_OK) return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_INVALID_GUEST};
    wasmtime_error_t *failure = wasmtime_context_set_fuel(vm->context, 100000000);
    if (failure != NULL) { RuntimeError(failure, NULL); return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_INVALID_GUEST}; }
    wasm_trap_t *trap = NULL;
    failure = wasmtime_func_call(vm->context, &vm->step, NULL, 0, NULL, 0, &trap);
    if (failure != NULL || trap != NULL) { RuntimeError(failure, trap); return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_INVALID_GUEST}; }
    switch (ReadDelivery(vm, input.len, delivery).tag) {
    case READ_DELIVERY_OK: return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_OK};
    case READ_DELIVERY_INVALID_GUEST: return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_INVALID_GUEST};
    case READ_DELIVERY_OUT_OF_MEMORY: return (connection_vm_step_result){.tag = CONNECTION_VM_STEP_OUT_OF_MEMORY};
    }
    __builtin_unreachable();
}

connection_vm_close_result ConnectionVmClose(connection_vm *vm)
{
    if (vm == NULL) return (connection_vm_close_result){.tag = CONNECTION_VM_CLOSE_OK};
    if (vm->owner != ThreadCurrentId()) return (connection_vm_close_result){.tag = CONNECTION_VM_CLOSE_WRONG_THREAD};
    bump_destroy_result released = BumpDestroy(&vm->copied_request);
    if (released.tag == BUMP_DESTROY_INVALID_ARGUMENT)
        return (connection_vm_close_result){.tag = CONNECTION_VM_CLOSE_INVALID_STATE};
    if (released.tag == BUMP_DESTROY_SYSTEM_ERROR)
        return (connection_vm_close_result){.tag = CONNECTION_VM_CLOSE_SYSTEM_ERROR, .native_error = released.native_error};
    wasmtime_store_delete(vm->store);
    free(vm);
    return (connection_vm_close_result){.tag = CONNECTION_VM_CLOSE_OK};
}

connection_vm_memory_size_result ConnectionVmMemorySize(connection_vm *vm)
{
    if (vm == NULL) return (connection_vm_memory_size_result){.tag = CONNECTION_VM_MEMORY_SIZE_INVALID_ARGUMENT};
    if (vm->owner != ThreadCurrentId()) return (connection_vm_memory_size_result){.tag = CONNECTION_VM_MEMORY_SIZE_WRONG_THREAD};
    return (connection_vm_memory_size_result){.tag = CONNECTION_VM_MEMORY_SIZE_OK,
        .value = wasmtime_memory_data_size(vm->context, &vm->memory)};
}
