#include "business_vm.h"
#include "slice.h"
#include "wasm_pool.h"
#include "wasm_reset.h"
#include "os.h"
#include "broker_protocol.h"
#include <wasmtime.h>
#include <stdlib.h>

typedef struct business_instance {
    wasmtime_store_t *store;
    wasmtime_context_t *context;
    wasmtime_memory_t memory;
    wasmtime_func_t init, handle, resume;
    bool has_resume;
    wasmtime_global_t stack;
    wasmtime_val_t initial_stack;
    wasmtime_table_t table;
    usize table_size, initial_size;
    byte_slice image; /* Initial nonzero prefix; the remaining memory is zero. */
} business_instance;
typedef struct business_thread_pool { business_instance *idle; } business_thread_pool;
struct business_runtime {
    wasm_engine_t *engine;
    wasmtime_module_t *module;
    os_thread_local *threads;
    bool can_reset, broker;
};
static void FailureDelete(wasmtime_error_t *error, wasm_trap_t *trap)
{
    if (error) wasmtime_error_delete(error);
    if (trap) wasm_trap_delete(trap);
}
static void InstanceDestroy(business_instance *instance)
{
    if (!instance) return;
    free(instance->image.ptr);
    if (instance->store) wasmtime_store_delete(instance->store);
    free(instance);
}
static void ThreadPoolDestroy(void *context)
{
    business_thread_pool *pool = context;
    InstanceDestroy(pool->idle);
    free(pool);
}
void BusinessRuntimeDestroy(business_runtime *runtime)
{
    if (!runtime) return;
    /* Other users must have exited/joined, releasing their TLS pools first. */
    OsThreadLocalDestroy(runtime->threads);
    if (runtime->module) wasmtime_module_delete(runtime->module);
    if (runtime->engine) wasm_engine_delete(runtime->engine);
    free(runtime);
}
bool BusinessRuntimeCanReset(const business_runtime *runtime) { return runtime && runtime->can_reset; }
business_runtime_create_result BusinessRuntimeCreate(const_byte_slice module, business_runtime **output)
{
    if (!output || !module.ptr || !module.len) return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_INVALID_ARGUMENT};
    *output = NULL;
    business_runtime *runtime = calloc(1, sizeof(*runtime));
    if (!runtime) return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_OUT_OF_MEMORY};
    wasm_config_t *config = wasm_config_new();
    if (!config) { free(runtime); return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_OUT_OF_MEMORY}; }
    wasmtime_config_consume_fuel_set(config, true);
    wasmtime_config_wasm_threads_set(config, false);
    wasmtime_config_wasm_gc_set(config, false);
    wasmtime_config_wasm_function_references_set(config, false);
    wasmtime_config_parallel_compilation_set(config, false);
    wasmtime_config_memory_reservation_set(config, BUSINESS_MEMORY_LIMIT);
    if (WasmPoolConfigure(config, BUSINESS_MEMORY_LIMIT).tag != WASM_POOL_CONFIGURE_OK) {
        wasm_config_delete(config);
        free(runtime);
        return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_OUT_OF_MEMORY};
    }
    runtime->engine = wasm_engine_new_with_config(config);
    if (!runtime->engine) { free(runtime); return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_OUT_OF_MEMORY}; }
    wasmtime_error_t *error = wasmtime_module_new(runtime->engine, module.ptr, module.len, &runtime->module);
    if (error) { FailureDelete(error, NULL); BusinessRuntimeDestroy(runtime); return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_INVALID_MODULE}; }
    wasm_importtype_vec_t imports;
    wasmtime_module_imports(runtime->module, &imports);
    usize count = imports.size;
    wasm_importtype_vec_delete(&imports);
    if (count) { BusinessRuntimeDestroy(runtime); return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_INVALID_MODULE}; }
    runtime->can_reset = WasmResetCompatible(module);
    if (OsThreadLocalCreate(ThreadPoolDestroy, &runtime->threads).tag != OS_THREAD_LOCAL_CREATE_OK) {
        BusinessRuntimeDestroy(runtime);
        return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_OUT_OF_MEMORY};
    }
    *output = runtime;
    return (business_runtime_create_result){BUSINESS_RUNTIME_CREATE_OK};
}
static byte_slice Memory(business_instance *instance)
{
    return (byte_slice){wasmtime_memory_data(instance->context, &instance->memory), wasmtime_memory_data_size(instance->context, &instance->memory)};
}
typedef enum business_export_tag { BUSINESS_EXPORT_OK, BUSINESS_EXPORT_INVALID } business_export_tag;
typedef struct business_export_result { business_export_tag tag; } business_export_result;
static business_export_result Export(wasmtime_context_t *context, wasmtime_instance_t *instance,
    string_slice name, wasmtime_extern_kind_t kind, wasmtime_extern_t *item)
{
    if (!wasmtime_instance_export_get(context, instance, name.ptr, name.len, item)) return (business_export_result){BUSINESS_EXPORT_INVALID};
    if (item->kind != kind) { wasmtime_extern_delete(item); return (business_export_result){BUSINESS_EXPORT_INVALID}; }
    return (business_export_result){BUSINESS_EXPORT_OK};
}
typedef enum instance_create_tag { INSTANCE_CREATE_OK, INSTANCE_CREATE_NO_SPACE, INSTANCE_CREATE_INVALID } instance_create_tag;
typedef struct instance_create_result { instance_create_tag tag; business_instance *instance; } instance_create_result;
static instance_create_result InstanceCreate(business_runtime *runtime)
{
    business_instance *vm = calloc(1, sizeof(*vm));
    if (!vm) return (instance_create_result){.tag = INSTANCE_CREATE_NO_SPACE};
    vm->store = wasmtime_store_new(runtime->engine, NULL, NULL);
    if (!vm->store) { free(vm); return (instance_create_result){.tag = INSTANCE_CREATE_NO_SPACE}; }
    vm->context = wasmtime_store_context(vm->store);
    wasmtime_store_limiter(vm->store, BUSINESS_MEMORY_LIMIT, 64, 1, 1, 1);
    wasmtime_error_t *error = wasmtime_context_set_fuel(vm->context, 10000000);
    wasm_trap_t *trap = NULL;
    if (error) goto invalid;
    wasmtime_instance_t instance;
    error = wasmtime_instance_new(vm->context, runtime->module, NULL, 0, &instance, &trap);
    if (error || trap) goto invalid;
    wasmtime_extern_t item;
    if (Export(vm->context, &instance, StringSlice("memory"), WASMTIME_EXTERN_MEMORY, &item).tag != BUSINESS_EXPORT_OK) goto invalid;
    vm->memory = item.of.memory;
    wasmtime_extern_delete(&item);
    if (Export(vm->context, &instance, runtime->broker ? StringSlice("BrokerInit") : StringSlice("BusinessInit"), WASMTIME_EXTERN_FUNC, &item).tag != BUSINESS_EXPORT_OK) goto invalid;
    vm->init = item.of.func;
    wasmtime_extern_delete(&item);
    if (Export(vm->context, &instance, runtime->broker ? StringSlice("BrokerHandle") : StringSlice("BusinessHandle"), WASMTIME_EXTERN_FUNC, &item).tag != BUSINESS_EXPORT_OK) goto invalid;
    vm->handle = item.of.func;
    wasmtime_extern_delete(&item);
    if (Export(vm->context, &instance, runtime->broker ? StringSlice("BrokerContinue") : StringSlice("BusinessContinue"), WASMTIME_EXTERN_FUNC, &item).tag == BUSINESS_EXPORT_OK) {
        vm->resume = item.of.func;
        vm->has_resume = true;
        wasmtime_extern_delete(&item);
    }
    if (runtime->can_reset) {
        if (Export(vm->context, &instance, StringSlice("__stack_pointer"), WASMTIME_EXTERN_GLOBAL, &item).tag != BUSINESS_EXPORT_OK) goto invalid;
        vm->stack = item.of.global;
        wasmtime_extern_delete(&item);
        wasmtime_global_get(vm->context, &vm->stack, &vm->initial_stack);
        if (vm->initial_stack.kind != WASMTIME_I32) goto invalid;
        if (Export(vm->context, &instance, StringSlice("__indirect_function_table"), WASMTIME_EXTERN_TABLE, &item).tag != BUSINESS_EXPORT_OK) goto invalid;
        vm->table = item.of.table;
        wasmtime_extern_delete(&item);
        vm->table_size = wasmtime_table_size(vm->context, &vm->table);
        if (vm->table_size > 64) goto invalid;
        byte_slice memory = Memory(vm);
        vm->initial_size = memory.len;
        usize length = memory.len;
        while (length && !memory.ptr[length - 1]) --length;
        if (length) {
            vm->image = (byte_slice){malloc(length), length};
            if (!vm->image.ptr) { InstanceDestroy(vm); return (instance_create_result){.tag = INSTANCE_CREATE_NO_SPACE}; }
            if (ByteSliceCopyRange(vm->image, 0, (const_byte_slice){memory.ptr, memory.len}, 0, length).tag != BYTE_SLICE_COPY_RANGE_OK) goto invalid;
        }
    }
    return (instance_create_result){.tag = INSTANCE_CREATE_OK, .instance = vm};
invalid:
    FailureDelete(error, trap);
    InstanceDestroy(vm);
    return (instance_create_result){.tag = INSTANCE_CREATE_INVALID};
}
typedef enum instance_reset_tag { INSTANCE_RESET_OK, INSTANCE_RESET_INVALID } instance_reset_tag;
typedef struct instance_reset_result { instance_reset_tag tag; } instance_reset_result;
static instance_reset_result InstanceReset(business_instance *vm)
{
    byte_slice memory = Memory(vm);
    if (memory.len < vm->initial_size || memory.len > BUSINESS_MEMORY_LIMIT ||
        wasmtime_table_size(vm->context, &vm->table) != vm->table_size ||
        ByteSliceZero(memory).tag != BYTE_SLICE_ZERO_OK ||
        ByteSliceCopy(memory, 0, (const_byte_slice){vm->image.ptr, vm->image.len}).tag != BYTE_SLICE_COPY_OK)
        return (instance_reset_result){INSTANCE_RESET_INVALID};
    wasmtime_error_t *error = wasmtime_global_set(vm->context, &vm->stack, &vm->initial_stack);
    if (error) { FailureDelete(error, NULL); return (instance_reset_result){INSTANCE_RESET_INVALID}; }
    wasmtime_val_t null = {.kind = WASMTIME_FUNCREF};
    wasmtime_funcref_set_null(&null.of.funcref);
    for (usize index = 0; index < vm->table_size; ++index) {
        error = wasmtime_table_set(vm->context, &vm->table, index, &null);
        if (error) { FailureDelete(error, NULL); return (instance_reset_result){INSTANCE_RESET_INVALID}; }
    }
    return (instance_reset_result){INSTANCE_RESET_OK};
}
business_run_result BusinessRun(business_runtime *runtime, const_byte_slice request, byte_slice output)
{
    if (!runtime || !request.ptr || request.len < sizeof(http_request) || request.len > CONNECTION_DISPATCH_LIMIT ||
        (!output.ptr && output.len)) return (business_run_result){.tag = BUSINESS_RUN_INVALID_ARGUMENT};
    business_thread_pool *pool = OsThreadLocalGet(runtime->threads);
    if (!pool) {
        pool = calloc(1, sizeof(*pool));
        if (!pool) return (business_run_result){.tag = BUSINESS_RUN_OUT_OF_MEMORY};
        if (OsThreadLocalSet(runtime->threads, pool).tag != OS_THREAD_LOCAL_SET_OK) {
            free(pool);
            return (business_run_result){.tag = BUSINESS_RUN_OUT_OF_MEMORY};
        }
    }
    business_instance *vm = pool->idle;
    bool reused = vm != NULL;
    pool->idle = NULL;
    if (!vm) {
        instance_create_result created = InstanceCreate(runtime);
        switch (created.tag) {
        case INSTANCE_CREATE_OK: vm = created.instance; break;
        case INSTANCE_CREATE_NO_SPACE: return (business_run_result){.tag = BUSINESS_RUN_OUT_OF_MEMORY};
        case INSTANCE_CREATE_INVALID: return (business_run_result){.tag = BUSINESS_RUN_INVALID_GUEST};
        }
    }
    business_run_result result = {.tag = BUSINESS_RUN_INVALID_GUEST};
    wasmtime_context_t *context = vm->context;
    /* Fresh instances already received their budget before the start function. */
    wasmtime_error_t *error = reused ? wasmtime_context_set_fuel(context, 10000000) : NULL;
    wasm_trap_t *trap = NULL;
    if (error) goto done;
    wasmtime_val_t args[2] = {{.kind = WASMTIME_I32, .of.i32 = BUSINESS_ABI_VERSION},
        {.kind = WASMTIME_I32, .of.i32 = (i32)request.len}}, returned;
    error = wasmtime_func_call(context, &vm->init, args, 2, &returned, 1, &trap);
    if (error || trap || returned.kind != WASMTIME_I32) goto done;
    byte_slice bytes = Memory(vm), input;
    business_init_result initialized;
    if (ByteSliceCopyRange((byte_slice){(u8 *)&initialized, sizeof(initialized)}, 0,
        (const_byte_slice){bytes.ptr, bytes.len}, (u32)returned.of.i32, sizeof(initialized)).tag != BYTE_SLICE_COPY_RANGE_OK) goto done;
    if (initialized.tag == BUSINESS_INIT_NO_SPACE) { result.tag = BUSINESS_RUN_OUT_OF_MEMORY; goto done; }
    if (initialized.tag != BUSINESS_INIT_OK || initialized.input.len != request.len ||
        ByteSliceSubslice(bytes, initialized.input.ptr, initialized.input.len, &input).tag != BYTE_SLICE_SUBSLICE_OK ||
        ByteSliceCopy(input, 0, request).tag != BYTE_SLICE_COPY_OK) goto done;
    error = wasmtime_func_call(context, &vm->handle, NULL, 0, &returned, 1, &trap);
    if (error || trap || returned.kind != WASMTIME_I32) goto done;
    bytes = Memory(vm);
    business_handle_result handled;
    const_byte_slice response;
    if (ByteSliceCopyRange((byte_slice){(u8 *)&handled, sizeof(handled)}, 0,
        (const_byte_slice){bytes.ptr, bytes.len}, (u32)returned.of.i32, sizeof(handled)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        handled.tag != BUSINESS_HANDLE_OK || handled.type != BUSINESS_RESULT ||
        ConstByteSliceSubslice((const_byte_slice){bytes.ptr, bytes.len}, handled.output.ptr, handled.output.len, &response).tag != CONST_BYTE_SLICE_SUBSLICE_OK ||
        HttpResponseValidate(response).tag != HTTP_RESPONSE_VALIDATE_OK) goto done;
    if (ByteSliceCopy(output, 0, response).tag != BYTE_SLICE_COPY_OK) { result.tag = BUSINESS_RUN_OUTPUT_TOO_SMALL; goto done; }
    result = (business_run_result){.tag = BUSINESS_RUN_OK, .response = {output.ptr, response.len}};
done:
    FailureDelete(error, trap);
    if (result.tag == BUSINESS_RUN_OK && runtime->can_reset && InstanceReset(vm).tag == INSTANCE_RESET_OK) pool->idle = vm;
    else InstanceDestroy(vm);
    return result;
}

struct business_session {
    business_runtime *runtime;
    business_instance *vm;
    u64 owner;
    bool started, terminal, poisoned;
};
broker_runtime_create_result BrokerRuntimeCreate(const_byte_slice module, business_runtime **output)
{
    business_runtime_create_result created = BusinessRuntimeCreate(module, output);
    switch (created.tag) {
    case BUSINESS_RUNTIME_CREATE_OK:
        (*output)->broker = true;
        (*output)->can_reset = false;
        return (broker_runtime_create_result){BROKER_RUNTIME_CREATE_OK};
    case BUSINESS_RUNTIME_CREATE_OUT_OF_MEMORY: return (broker_runtime_create_result){BROKER_RUNTIME_CREATE_NO_SPACE};
    case BUSINESS_RUNTIME_CREATE_INVALID_ARGUMENT:
    case BUSINESS_RUNTIME_CREATE_INVALID_MODULE: return (broker_runtime_create_result){BROKER_RUNTIME_CREATE_INVALID};
    }
    __builtin_unreachable();
}
/* External ABI adapter: invoke an input-window export, validate and copy before
 * any subsequent guest call. This is shared by begin and continuation events. */
typedef enum session_input_tag { SESSION_INPUT_OK, SESSION_INPUT_INVALID, SESSION_INPUT_NO_SPACE } session_input_tag;
typedef struct session_input_result { session_input_tag tag; } session_input_result;
static session_input_result SessionInput(business_instance *vm, wasmtime_func_t *function, u32 event, const_byte_slice input)
{
    if ((!input.ptr && input.len) || input.len > CONNECTION_DISPATCH_LIMIT) return (session_input_result){SESSION_INPUT_INVALID};
    wasmtime_val_t arguments[2] = {{.kind = WASMTIME_I32, .of.i32 = (i32)event}, {.kind = WASMTIME_I32, .of.i32 = (i32)input.len}}, returned;
    wasm_trap_t *trap = NULL;
    wasmtime_error_t *error = wasmtime_func_call(vm->context, function, arguments, 2, &returned, 1, &trap);
    if (error || trap || returned.kind != WASMTIME_I32) { FailureDelete(error, trap); return (session_input_result){SESSION_INPUT_INVALID}; }
    byte_slice memory = Memory(vm), window;
    business_init_result ready;
    if (ByteSliceCopyRange(MutableBytesOf(ready), 0, (const_byte_slice){memory.ptr, memory.len}, (u32)returned.of.i32, sizeof(ready)).tag != BYTE_SLICE_COPY_RANGE_OK)
        return (session_input_result){SESSION_INPUT_INVALID};
    if (ready.tag == BUSINESS_INIT_NO_SPACE) return (session_input_result){SESSION_INPUT_NO_SPACE};
    if (ready.tag != BUSINESS_INIT_OK || ready.input.len != input.len ||
        ByteSliceSubslice(memory, ready.input.ptr, ready.input.len, &window).tag != BYTE_SLICE_SUBSLICE_OK ||
        ByteSliceCopy(window, 0, input).tag != BYTE_SLICE_COPY_OK) return (session_input_result){SESSION_INPUT_INVALID};
    return (session_input_result){SESSION_INPUT_OK};
}
business_session_open_result BusinessSessionOpen(business_runtime *runtime, const_byte_slice request)
{
    if (!runtime || !request.ptr || request.len < sizeof(http_request) || request.len > CONNECTION_DISPATCH_LIMIT)
        return (business_session_open_result){.tag = BUSINESS_SESSION_OPEN_INVALID};
    business_thread_pool *pool = OsThreadLocalGet(runtime->threads);
    if (!pool) {
        pool = calloc(1, sizeof(*pool));
        if (!pool) return (business_session_open_result){.tag = BUSINESS_SESSION_OPEN_NO_SPACE};
        if (OsThreadLocalSet(runtime->threads, pool).tag != OS_THREAD_LOCAL_SET_OK) {
            free(pool); return (business_session_open_result){.tag = BUSINESS_SESSION_OPEN_NO_SPACE};
        }
    }
    business_session *session = calloc(1, sizeof(*session));
    if (!session) return (business_session_open_result){.tag = BUSINESS_SESSION_OPEN_NO_SPACE};
    business_instance *vm = pool->idle;
    pool->idle = NULL;
    bool reused = vm != NULL;
    if (!vm) {
        instance_create_result created = InstanceCreate(runtime);
        if (created.tag != INSTANCE_CREATE_OK) {
            free(session);
            return (business_session_open_result){.tag = created.tag == INSTANCE_CREATE_NO_SPACE ? BUSINESS_SESSION_OPEN_NO_SPACE : BUSINESS_SESSION_OPEN_INVALID};
        }
        vm = created.instance;
    }
    if (reused) {
        wasmtime_error_t *error = wasmtime_context_set_fuel(vm->context, 10000000);
        if (error) { FailureDelete(error, NULL); InstanceDestroy(vm); free(session); return (business_session_open_result){.tag = BUSINESS_SESSION_OPEN_INVALID}; }
    }
    session_input_result input = SessionInput(vm, &vm->init, BUSINESS_ABI_VERSION, request);
    if (input.tag != SESSION_INPUT_OK) {
        InstanceDestroy(vm); free(session);
        return (business_session_open_result){.tag = input.tag == SESSION_INPUT_NO_SPACE ? BUSINESS_SESSION_OPEN_NO_SPACE : BUSINESS_SESSION_OPEN_INVALID};
    }
    *session = (business_session){.runtime = runtime, .vm = vm, .owner = ThreadCurrentId()};
    return (business_session_open_result){BUSINESS_SESSION_OPEN_OK, session};
}
business_session_step_result BusinessSessionStep(business_session *session, u32 event, const_byte_slice input, byte_slice output)
{
    business_session_step_result result = {.tag = BUSINESS_SESSION_STEP_INVALID};
    if (!session) return result;
    if (session->owner != ThreadCurrentId()) return (business_session_step_result){.tag = BUSINESS_SESSION_STEP_WRONG_THREAD};
    if (session->terminal || session->poisoned || (!output.ptr && output.len)) return result;
    business_instance *vm = session->vm;
    if (!session->started) {
        u32 expected = session->runtime->broker ? BROKER_REQUEST : BUSINESS_REQUEST;
        if (event != expected || input.len) goto invalid;
        session->started = true;
    } else {
        if (!vm->has_resume) goto invalid;
        session_input_result ready = SessionInput(vm, &vm->resume, event, input);
        if (ready.tag != SESSION_INPUT_OK) {
            if (ready.tag == SESSION_INPUT_NO_SPACE) result.tag = BUSINESS_SESSION_STEP_NO_SPACE;
            goto invalid;
        }
    }
    wasm_trap_t *trap = NULL;
    wasmtime_val_t returned;
    wasmtime_error_t *error = wasmtime_func_call(vm->context, &vm->handle, NULL, 0, &returned, 1, &trap);
    if (error || trap || returned.kind != WASMTIME_I32) { FailureDelete(error, trap); goto invalid; }
    byte_slice memory = Memory(vm);
    const_byte_slice bytes = {memory.ptr, memory.len}, payload;
    business_handle_result handled;
    if (ByteSliceCopyRange(MutableBytesOf(handled), 0, bytes, (u32)returned.of.i32, sizeof(handled)).tag != BYTE_SLICE_COPY_RANGE_OK ||
        handled.tag != BUSINESS_HANDLE_OK || ConstByteSliceSubslice(bytes, handled.output.ptr, handled.output.len, &payload).tag != CONST_BYTE_SLICE_SUBSLICE_OK) goto invalid;
    if (session->runtime->broker) {
        switch (handled.type) {
        case BROKER_ADMITTED:
        case BROKER_FINISHED:
        case BROKER_REJECTED:
            if (payload.len) goto invalid;
            break;
        case BROKER_EXECUTE_QUERY: if (payload.len != sizeof(query_arguments)) goto invalid; break;
        case BROKER_QUERY_RESULT: if (payload.len < sizeof(query_result) || payload.len > QUERY_MESSAGE_LIMIT) goto invalid; break;
        default: goto invalid;
        }
        if (handled.type == BROKER_FINISHED || handled.type == BROKER_REJECTED) session->terminal = true;
    } else {
        switch (handled.type) {
        case BUSINESS_RESULT:
            if (HttpResponseValidate(payload).tag != HTTP_RESPONSE_VALIDATE_OK) goto invalid;
            session->terminal = true;
            break;
        case BUSINESS_QUERY_REQUEST: if (payload.len != sizeof(query_call)) goto invalid; break;
        case BUSINESS_FAILED: if (payload.len) goto invalid; session->terminal = true; break;
        default: goto invalid;
        }
    }
    if (ByteSliceCopy(output, 0, payload).tag != BYTE_SLICE_COPY_OK) { result.tag = BUSINESS_SESSION_STEP_NO_SPACE; goto invalid; }
    return (business_session_step_result){BUSINESS_SESSION_STEP_OK, handled.type, {output.ptr, payload.len}};
invalid:
    session->poisoned = true;
    return result;
}
business_session_close_result BusinessSessionClose(business_session *session, bool reuse)
{
    if (!session) return (business_session_close_result){BUSINESS_SESSION_CLOSE_OK};
    if (session->owner != ThreadCurrentId()) return (business_session_close_result){BUSINESS_SESSION_CLOSE_WRONG_THREAD};
    business_runtime *runtime = session->runtime;
    business_thread_pool *pool = OsThreadLocalGet(runtime->threads);
    if (reuse && session->terminal && !session->poisoned && runtime->can_reset && pool && !pool->idle &&
        InstanceReset(session->vm).tag == INSTANCE_RESET_OK) pool->idle = session->vm;
    else InstanceDestroy(session->vm);
    free(session);
    return (business_session_close_result){BUSINESS_SESSION_CLOSE_OK};
}
