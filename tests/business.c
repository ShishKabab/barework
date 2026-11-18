#include "business_vm.h"
#include "business_guest_bytes.h"
#include "http_response.h"
#include "slice.h"
#include "wasm_pool.h"
#include "app-routes.h"
#include <assert.h>
#include <wasmtime.h>
#include <stdio.h>

typedef struct request_fixture { http_request root; char method[4]; char target[7]; } request_fixture;
static request_fixture request = {.root = {.MessageLength = sizeof(request_fixture),
    .MethodOffset = offsetof(request_fixture, method), .MethodLength = 3,
    .TargetOffset = offsetof(request_fixture, target), .TargetLength = 1,
    .VersionMajor = 1, .VersionMinor = 1}, .method = "GET", .target = "/empty"};

static const_byte_slice RoutedRequest(void)
{
    static u8 storage[1024];
    bump_allocator arena;
    assert(BumpInit(&arena, (byte_slice){storage, sizeof(storage)}).tag == BUMP_INIT_OK);
    route_match_result result = RouteMatch(BytesOf(request), &arena);
    assert(result.tag == ROUTE_MATCH_OK);
    return result.message;
}

static bool Contains(const_byte_slice bytes, string_slice text)
{
    SliceFor(bytes, index) {
        if (text.len > bytes.len - index) break;
        bool equal = true;
        SliceFor(text, offset) { if (bytes.ptr[index + offset] != (u8)text.ptr[offset]) equal = false; }
        if (equal) return true;
    }
    return false;
}

static void BadGuest(string_slice wat, bool invalid_module)
{
    wasm_byte_vec_t bytes;
    assert(wasmtime_wat2wasm(wat.ptr, wat.len, &bytes) == NULL);
    business_runtime *runtime;
    business_runtime_create_result created = BusinessRuntimeCreate((const_byte_slice){(const u8 *)bytes.data, bytes.size}, &runtime);
    wasm_byte_vec_delete(&bytes);
    if (invalid_module) { assert(created.tag == BUSINESS_RUNTIME_CREATE_INVALID_MODULE); return; }
    assert(created.tag == BUSINESS_RUNTIME_CREATE_OK);
    u8 output[BUSINESS_RESPONSE_LIMIT];
    assert(BusinessRun(runtime, BytesOf(request), (byte_slice){output, sizeof(output)}).tag == BUSINESS_RUN_INVALID_GUEST);
    BusinessRuntimeDestroy(runtime);
}

static void TestPoolReset(void)
{
    /* A recycled slot must restore globals, initial memory size, zero pages,
     * data segments and table entries, including after a dirty guest traps. */
    string_slice wat = StringSlice(
        "(module (memory (export \"memory\") 1 32) (global $used (mut i32) (i32.const 0))"
        "(table 1 funcref) (func $marker) (elem declare func $marker)"
        "(data (i32.const 256) \"\\18\\00\\00\\00\\c8\\00\\00\\00\")"
        "(func (export \"BusinessInit\") (param i32 i32) (result i32)"
        " global.get $used if unreachable end"
        " i32.const 4096 i32.load if unreachable end"
        " i32.const 260 i32.load i32.const 200 i32.ne if unreachable end"
        " memory.size i32.const 1 i32.ne if unreachable end"
        " i32.const 0 table.get ref.is_null i32.eqz if unreachable end"
        " i32.const 1 global.set $used"
        " i32.const 1 memory.grow i32.const 1 i32.ne if unreachable end"
        " i32.const 65536 i32.load if unreachable end"
        " i32.const 65536 i32.const 77 i32.store"
        " i32.const 4096 i32.const 99 i32.store"
        " i32.const 0 ref.func $marker table.set"
        " i32.const 68 i32.const 1024 i32.store"
        " i32.const 72 local.get 1 i32.store i32.const 64)"
        "(func (export \"BusinessHandle\") (result i32)"
        " i32.const 1024 i32.const 1028 i32.load i32.add i32.load8_u"
        " i32.const 80 i32.eq if i32.const 260 i32.const 999 i32.store unreachable end"
        " i32.const 84 i32.const 0x4002 i32.store"
        " i32.const 88 i32.const 256 i32.store"
        " i32.const 92 i32.const 24 i32.store i32.const 80))");
    wasm_byte_vec_t bytes;
    assert(wasmtime_wat2wasm(wat.ptr, wat.len, &bytes) == NULL);
    business_runtime *runtime;
    assert(BusinessRuntimeCreate((const_byte_slice){(const u8 *)bytes.data, bytes.size}, &runtime).tag == BUSINESS_RUNTIME_CREATE_OK);
    wasm_byte_vec_delete(&bytes);
    request_fixture input = request;
    u8 storage[BUSINESS_RESPONSE_LIMIT];
    for (usize index = 0; index < 2 * WASM_POOL_CAPACITY + 1; ++index) {
        input.method[0] = index % 2 ? 'P' : 'G';
        business_run_result result = BusinessRun(runtime, BytesOf(input), (byte_slice){storage, sizeof(storage)});
        if (index % 2) assert(result.tag == BUSINESS_RUN_INVALID_GUEST);
        else {
            assert(result.tag == BUSINESS_RUN_OK && result.response.len == sizeof(http_response));
            assert(HttpResponseValidate(result.response).tag == HTTP_RESPONSE_VALIDATE_OK);
        }
    }
    BusinessRuntimeDestroy(runtime);
}

int main(void)
{
    business_runtime *runtime;
    assert(BusinessRuntimeCreate((const_byte_slice){business_guest_bytes, business_guest_bytes_len}, &runtime).tag == BUSINESS_RUNTIME_CREATE_OK);
    assert(BusinessRuntimeCanReset(runtime));
    u8 storage[BUSINESS_RESPONSE_LIMIT], wire_storage[4096], saved[BUSINESS_RESPONSE_LIMIT];
    byte_slice output = {storage, sizeof(storage)}, wire = {wire_storage, sizeof(wire_storage)};
    business_run_result run = BusinessRun(runtime, RoutedRequest(), output);
    assert(run.tag == BUSINESS_RUN_OK);
    usize length = run.response.len;
    assert(ByteSliceCopy((byte_slice){saved, sizeof(saved)}, 0, run.response).tag == BYTE_SLICE_COPY_OK);
    /* Run returns after reset or destruction. Validate/copy/serialize now. */
    assert(HttpResponseValidate(run.response).tag == HTTP_RESPONSE_VALIDATE_OK);
    http_response root;
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&root, sizeof(root)}, 0, run.response, 0, sizeof(root)).tag == BYTE_SLICE_COPY_RANGE_OK);
    assert(root.HeaderCount == 1 && root.Body.len == 14 && root.StatusCode == 200);
    assert(HttpResponseSerialize(run.response, 0, 1, wire).tag == HTTP_RESPONSE_SERIALIZE_OK);
    http_response_serialize_result head = HttpResponseSerialize(run.response, 1, 1, wire);
    assert(head.tag == HTTP_RESPONSE_SERIALIZE_OK);
    usize head_length = head.bytes.len;
    http_response_serialize_result full = HttpResponseSerialize(run.response, 0, 1, wire);
    assert(full.tag == HTTP_RESPONSE_SERIALIZE_OK && full.bytes.len == head_length + 14);
    assert(HttpResponseSerialize(run.response, 0, 1, (byte_slice){wire.ptr, 1}).tag == HTTP_RESPONSE_SERIALIZE_INVALID);
    for (usize i = 0; i < 100; ++i) {
        run = BusinessRun(runtime, RoutedRequest(), output);
        assert(run.tag == BUSINESS_RUN_OK && run.response.len == length);
        SliceFor(run.response, index) { assert(run.response.ptr[index] == saved[index]); }
    }
    assert(BusinessRun(runtime, RoutedRequest(), (byte_slice){0}).tag == BUSINESS_RUN_OUTPUT_TOO_SMALL);
    request.root.TargetLength = 6;
    run = BusinessRun(runtime, RoutedRequest(), output);
    assert(run.tag == BUSINESS_RUN_OK);
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&root, sizeof(root)}, 0, run.response, 0, sizeof(root)).tag == BYTE_SLICE_COPY_RANGE_OK);
    assert(root.StatusCode == 204 && root.Body.len == 0 && root.Body.index == 0);
    /* Real guest rejects invalid dispatch tags before reading capture payloads. */
    const_byte_slice routed = RoutedRequest();
    u8 invalid_storage[1024];
    byte_slice invalid = {invalid_storage, routed.len};
    assert(ByteSliceCopy(invalid, 0, routed).tag == BYTE_SLICE_COPY_OK);
    u32 unknown = UINT32_MAX;
    assert(ByteSliceCopy(invalid, sizeof(route_request), BytesOf(unknown)).tag == BYTE_SLICE_COPY_OK);
    assert(BusinessRun(runtime, (const_byte_slice){invalid.ptr, invalid.len}, output).tag == BUSINESS_RUN_INVALID_GUEST);
    run = BusinessRun(runtime, RoutedRequest(), output);
    assert(run.tag == BUSINESS_RUN_OK);
    assert(HttpResponseSerialize(run.response, 0, 1, wire).tag == HTTP_RESPONSE_SERIALIZE_OK);
    /* Empty 200 and 304 also work; absent body is not an error sentinel. */
    root.StatusCode = 200;
    assert(ByteSliceCopy(output, 0, BytesOf(root)).tag == BYTE_SLICE_COPY_OK);
    http_response_serialize_result empty = HttpResponseSerialize(run.response, 0, 1, wire);
    assert(empty.tag == HTTP_RESPONSE_SERIALIZE_OK);
    assert(Contains(empty.bytes, StringSlice("\r\nContent-Length: 0\r\n")));
    assert(empty.bytes.len >= 4 && empty.bytes.ptr[empty.bytes.len - 4] == '\r' && empty.bytes.ptr[empty.bytes.len - 3] == '\n');
    root.StatusCode = 304;
    assert(ByteSliceCopy(output, 0, BytesOf(root)).tag == BYTE_SLICE_COPY_OK);
    empty = HttpResponseSerialize(run.response, 0, 1, wire);
    assert(empty.tag == HTTP_RESPONSE_SERIALIZE_OK && !Contains(empty.bytes, StringSlice("Content-Length:")));
    root.Body.len = 1;
    root.Body.index = sizeof(root);
    assert(ByteSliceCopy(output, 0, BytesOf(root)).tag == BYTE_SLICE_COPY_OK);
    assert(HttpResponseValidate(run.response).tag == HTTP_RESPONSE_VALIDATE_INVALID);
    root.Body.len = 0; root.Body.index = 0; root.StatusCode = 200;
    assert(ByteSliceCopy(output, 0, BytesOf(root)).tag == BYTE_SLICE_COPY_OK);
    http_header header;
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&header, sizeof(header)}, 0, run.response, root.FirstHeaderOffset, sizeof(header)).tag == BYTE_SLICE_COPY_RANGE_OK);
    u8 bad = '\r';
    assert(ByteSliceCopy(output, header.ValueOffset, BytesOf(bad)).tag == BYTE_SLICE_COPY_OK);
    assert(HttpResponseValidate(run.response).tag == HTTP_RESPONSE_VALIDATE_INVALID);
    assert(ByteSliceCopy(output, 0, (const_byte_slice){saved, length}).tag == BYTE_SLICE_COPY_OK);
    header.NextHeaderOffset = root.FirstHeaderOffset;
    assert(ByteSliceCopy(output, root.FirstHeaderOffset, BytesOf(header)).tag == BYTE_SLICE_COPY_OK);
    assert(HttpResponseValidate((const_byte_slice){output.ptr, length}).tag == HTTP_RESPONSE_VALIDATE_INVALID);
    assert(ByteSliceCopy(output, 0, (const_byte_slice){saved, length}).tag == BYTE_SLICE_COPY_OK);
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&header, sizeof(header)}, 0,
        (const_byte_slice){output.ptr, length}, root.FirstHeaderOffset, sizeof(header)).tag == BYTE_SLICE_COPY_RANGE_OK);
    string_slice forbidden = StringSlice("Content-Length");
    assert(ByteSliceCopy(output, header.KeyOffset, (const_byte_slice){(const u8 *)forbidden.ptr, forbidden.len}).tag == BYTE_SLICE_COPY_OK);
    header.KeyLength = (u32)forbidden.len;
    assert(ByteSliceCopy(output, root.FirstHeaderOffset, BytesOf(header)).tag == BYTE_SLICE_COPY_OK);
    assert(HttpResponseValidate((const_byte_slice){output.ptr, length}).tag == HTTP_RESPONSE_VALIDATE_INVALID);
    header.NextHeaderOffset = 0; header.KeyOffset = UINT32_MAX;
    assert(ByteSliceCopy(output, root.FirstHeaderOffset, BytesOf(header)).tag == BYTE_SLICE_COPY_OK);
    assert(HttpResponseValidate((const_byte_slice){output.ptr, length}).tag == HTTP_RESPONSE_VALIDATE_INVALID);
    BusinessRuntimeDestroy(runtime);

    BadGuest(StringSlice("(module (import \"bad\" \"import\" (func)))"), true);
    BadGuest(StringSlice("(module (memory (export \"memory\") 33) (func (export \"BusinessInit\") (param i32 i32) (result i32) i32.const 0) (func (export \"BusinessHandle\") (result i32) i32.const 0))"), true);
    BadGuest(StringSlice("(module (memory (export \"memory\") 1) (func (export \"BusinessInit\") (param i32 i32) (result i32) i32.const 65535) (func (export \"BusinessHandle\") (result i32) i32.const 0))"), false);
    BadGuest(StringSlice("(module (memory (export \"memory\") 1) (func (export \"BusinessInit\") (param i32 i32) (result i32) (loop br 0) i32.const 0) (func (export \"BusinessHandle\") (result i32) i32.const 0))"), false);
#define ValidInit "(module (memory (export \"memory\") 1) (func (export \"BusinessInit\") (param i32 i32) (result i32) i32.const 68 i32.const 1024 i32.store i32.const 72 local.get 1 i32.store i32.const 64)"
    BadGuest(StringSlice(ValidInit "(func (export \"BusinessHandle\") (result i32) unreachable))"), false);
    BadGuest(StringSlice(ValidInit "(func (export \"BusinessHandle\") (result i32) (loop br 0) i32.const 0))"), false);
    BadGuest(StringSlice(ValidInit "(func (export \"BusinessHandle\") (result i32) i32.const 84 i32.const 0x4002 i32.store i32.const 88 i32.const 65535 i32.store i32.const 92 i32.const 24 i32.store i32.const 80))"), false);
    BadGuest(StringSlice(ValidInit "(func (export \"BusinessHandle\") (result i32) i32.const 80 i32.const 999 i32.store i32.const 80))"), false);
#undef ValidInit
    TestPoolReset();
    puts("PASS: business instance lifecycle, copied result lifetime, bodyless/HEAD responses, ABI bounds, headers, traps, fuel, memory limits");
    return 0;
}
