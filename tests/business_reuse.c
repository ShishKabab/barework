#include "business_vm.h"
#include "wasm_reset.h"
#include "slice.h"
#include <wasmtime.h>
#include <assert.h>
#include <stdio.h>

typedef struct request_fixture { http_request root; char method[3], target[1]; } request_fixture;
static request_fixture request = {.root = {.MessageLength = sizeof(request_fixture),
    .MethodOffset = offsetof(request_fixture, method), .MethodLength = 3,
    .TargetOffset = offsetof(request_fixture, target), .TargetLength = 1,
    .VersionMajor = 1, .VersionMinor = 1}, .method = "GET", .target = "/"};
static business_runtime *Runtime(string_slice wat, bool resettable)
{
    wasm_byte_vec_t bytes;
    assert(wasmtime_wat2wasm(wat.ptr, wat.len, &bytes) == NULL);
    business_runtime *runtime;
    assert(BusinessRuntimeCreate((const_byte_slice){(const u8 *)bytes.data, bytes.size}, &runtime).tag == BUSINESS_RUNTIME_CREATE_OK);
    assert(BusinessRuntimeCanReset(runtime) == resettable);
    wasm_byte_vec_delete(&bytes);
    return runtime;
}
static void Expect(business_runtime *runtime, char method, u32 pages)
{
    request.method[0] = method;
    u8 storage[1024];
    business_run_result run = BusinessRun(runtime, BytesOf(request), (byte_slice){storage, sizeof(storage)});
    if (!pages) { assert(run.tag == BUSINESS_RUN_INVALID_GUEST); return; }
    assert(run.tag == BUSINESS_RUN_OK && run.response.len == 28);
    http_response response;
    u32 actual;
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&response, sizeof(response)}, 0, run.response, 0, sizeof(response)).tag == BYTE_SLICE_COPY_RANGE_OK);
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&actual, sizeof(actual)}, 0, run.response, response.Body.index, sizeof(actual)).tag == BYTE_SLICE_COPY_RANGE_OK);
    assert(response.StatusCode == 201 && actual == pages);
}
int main(void)
{
    string_slice wat = StringSlice(
        "(module (memory (export \"memory\") 1 32)"
        "(global $sp (export \"__stack_pointer\") (mut i32) (i32.const 65536))"
        "(table (export \"__indirect_function_table\") 1 1 funcref)"
        "(data (i32.const 256) \"\\1c\\00\\00\\00\\c8\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\18\\00\\00\\00\\04\\00\\00\\00\")"
        "(func (export \"BusinessInit\") (param i32 i32) (result i32)"
        " global.get $sp i32.const 65536 i32.ne if unreachable end"
        " i32.const 4096 i32.load if unreachable end"
        " i32.const 260 i32.load i32.const 200 i32.ne if unreachable end"
        " i32.const 0 table.get ref.is_null i32.eqz if unreachable end"
        " memory.size i32.const 65536 i32.mul i32.const 4 i32.sub i32.load if unreachable end"
        " i32.const 280 memory.size i32.store"
        " memory.size i32.const 1 i32.eq if i32.const 1 memory.grow drop end"
        " i32.const 65536 i32.load if unreachable end"
        " i32.const 65536 i32.const 77 i32.store"
        " i32.const 4096 i32.const 99 i32.store"
        " i32.const 123 global.set $sp"
        " i32.const 0 ref.func $handle table.set"
        " i32.const 68 i32.const 1024 i32.store i32.const 72 local.get 1 i32.store i32.const 64)"
        "(func $handle (export \"BusinessHandle\") (result i32) (local $method i32)"
        " i32.const 1024 i32.const 1028 i32.load i32.add i32.load8_u local.set $method"
        " i32.const 260 i32.const 201 i32.store"
        " local.get $method i32.const 80 i32.eq if unreachable end"
        " local.get $method i32.const 70 i32.eq if (loop br 0) end"
        " local.get $method i32.const 77 i32.eq if i32.const 30 memory.grow i32.const 2 i32.ne if unreachable end end"
        " memory.size i32.const 65536 i32.mul i32.const 4 i32.sub i32.const 123 i32.store"
        " i32.const 84 i32.const 0x4002 i32.store"
        " local.get $method i32.const 73 i32.eq if i32.const 84 i32.const 999 i32.store end"
        " i32.const 88 i32.const 256 i32.store i32.const 92 i32.const 28 i32.store i32.const 80))");
    business_runtime *runtime = Runtime(wat, true);
    Expect(runtime, 'G', 1);
    for (usize index = 0; index < 2000; ++index) Expect(runtime, 'G', 2);
    Expect(runtime, 'P', 0); Expect(runtime, 'G', 1); /* Trap evicts. */
    Expect(runtime, 'I', 0); Expect(runtime, 'G', 1); /* Invalid result evicts. */
    Expect(runtime, 'F', 0); Expect(runtime, 'G', 1); /* Exhausted fuel evicts. */
    Expect(runtime, 'M', 2); Expect(runtime, 'G', 32); /* Size retained, entire 2 MiB cleared. */
    request.method[0] = 'G';
    assert(BusinessRun(runtime, BytesOf(request), (byte_slice){0}).tag == BUSINESS_RUN_OUTPUT_TOO_SMALL);
    Expect(runtime, 'G', 1);
    BusinessRuntimeDestroy(runtime);
#define Shape "(module (memory (export \"memory\") 1 32) (global (export \"__stack_pointer\") (mut i32) (i32.const 65536)) (table (export \"__indirect_function_table\") 1 1 funcref)"
    runtime = Runtime(StringSlice(Shape ")"), true); BusinessRuntimeDestroy(runtime);
    runtime = Runtime(StringSlice(Shape "(global (mut i32) (i32.const 0)))"), false); BusinessRuntimeDestroy(runtime);
    runtime = Runtime(StringSlice(Shape "(data \"passive\"))"), false); BusinessRuntimeDestroy(runtime);
    runtime = Runtime(StringSlice(Shape "(elem declare func $f) (func $f))"), false); BusinessRuntimeDestroy(runtime);
    runtime = Runtime(StringSlice(Shape "(func $start) (start $start))"), false); BusinessRuntimeDestroy(runtime);
#undef Shape
    assert(!WasmResetCompatible((const_byte_slice){0}));
    puts("PASS: retained instances, full memory/static/global/table reset, high-water growth, traps/fuel/invalid-output eviction, conservative admission");
    return 0;
}
