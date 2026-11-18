#include "bump.h"
#include "http.h"
#include "slice.h"
#include "wasm_memory.h"

#include <string.h>

/* Return a failing source line to the Bash runner; no libc startup, malloc,
 * stdout, or additional memory.grow implementation is needed by this guest. */
typedef enum test_global_allocator_tag { TEST_GLOBAL_ALLOCATOR_OK, TEST_GLOBAL_ALLOCATOR_FAILED } test_global_allocator_tag;
typedef struct test_global_allocator_result { test_global_allocator_tag tag; u32 line; } test_global_allocator_result;
typedef enum test_bump_arenas_tag { TEST_BUMP_ARENAS_OK, TEST_BUMP_ARENAS_FAILED } test_bump_arenas_tag;
typedef struct test_bump_arenas_result { test_bump_arenas_tag tag; u32 line; } test_bump_arenas_result;
typedef enum test_parser_arena_tag { TEST_PARSER_ARENA_OK, TEST_PARSER_ARENA_FAILED } test_parser_arena_tag;
typedef struct test_parser_arena_result { test_parser_arena_tag tag; u32 line; } test_parser_arena_result;
#define Check(condition) do { if (!(condition)) { failure.line = __LINE__; return failure; } } while (0)

static test_global_allocator_result TestGlobalAllocator(void)
{
    test_global_allocator_result failure = {.tag = TEST_GLOBAL_ALLOCATOR_FAILED};
    byte_slice first = {0};
    Check(WasmMemoryAllocate(31, 16, &first).tag == WASM_MEMORY_ALLOCATE_OK);
    Check((uintptr_t)first.ptr % 16 == 0 && first.len == 31);
    first.ptr[0] = 0x5a;
    u8 *base = first.ptr;
    Check(WasmMemoryExtend(&first, WASM_MEMORY_PAGE_SIZE * 2 + 7).tag == WASM_MEMORY_EXTEND_OK);
    Check(first.ptr == base && first.ptr[0] == 0x5a);
    Check(first.ptr[first.len - 1] == 0);
    byte_slice second = {0};
    Check(WasmMemoryAllocate(23, 16, &second).tag == WASM_MEMORY_ALLOCATE_OK);
    Check(second.ptr >= first.ptr + first.len);
    usize first_length = first.len;
    usize pages = __builtin_wasm_memory_size(0);
    Check(WasmMemoryExtend(&first, first.len + 1).tag == WASM_MEMORY_EXTEND_BUSY);
    Check(first.ptr == base && first.len == first_length);
    Check(__builtin_wasm_memory_size(0) == pages);

    byte_slice failed = {0};
    Check(WasmMemoryAllocate(8 * 1024 * 1024, 1, &failed).tag == WASM_MEMORY_ALLOCATE_OUT_OF_MEMORY);
    Check(failed.ptr == NULL && failed.len == 0);
    Check(__builtin_wasm_memory_size(0) == pages);
    Check(WasmMemoryExtend(&second, 8 * 1024 * 1024).tag == WASM_MEMORY_EXTEND_OUT_OF_MEMORY);
    Check(second.len == 23);
    Check(WasmMemoryAllocate(1, 3, &failed).tag == WASM_MEMORY_ALLOCATE_INVALID_ARGUMENT);
    Check(WasmMemoryAllocate(SIZE_MAX, 1, &failed).tag == WASM_MEMORY_ALLOCATE_INVALID_ARGUMENT);
    Check(WasmMemoryAllocate(7, 1, &failed).tag == WASM_MEMORY_ALLOCATE_OK);
    Check(failed.ptr == second.ptr + second.len); /* Failure didn't advance end. */
    Check(WasmMemoryExtend(&failed, failed.len - 1).tag == WASM_MEMORY_EXTEND_INVALID_ARGUMENT);
    return (test_global_allocator_result){.tag = TEST_GLOBAL_ALLOCATOR_OK};
}

static test_bump_arenas_result TestBumpArenas(void)
{
    test_bump_arenas_result failure = {.tag = TEST_BUMP_ARENAS_FAILED};
    bump_allocator first;
    Check(BumpCreate(&first, WASM_MEMORY_PAGE_SIZE * 3).tag == BUMP_CREATE_OK);
    u8 *base = first.storage.ptr;
    Check(first.committed == WASM_MEMORY_PAGE_SIZE);
    bump_allocate_result data_result = BumpAllocate(&first, WASM_MEMORY_PAGE_SIZE + 1, 16);
    Check(data_result.tag == BUMP_ALLOCATE_OK);
    byte_slice data = data_result.allocation;
    Check(data.ptr == base && data.len == WASM_MEMORY_PAGE_SIZE + 1);
    Check(first.committed == WASM_MEMORY_PAGE_SIZE * 2);
    data.ptr[0] = 0x51;
    data.ptr[data.len - 1] = 0xa7;

    bump_allocator second;
    Check(BumpCreate(&second, WASM_MEMORY_PAGE_SIZE * 2).tag == BUMP_CREATE_OK);
    Check(second.storage.ptr == base + first.storage.len);
    bump_allocate_result tail_result = BumpAllocate(&first, WASM_MEMORY_PAGE_SIZE - 1, 1);
    Check(tail_result.tag == BUMP_ALLOCATE_OK);
    byte_slice tail = tail_result.allocation;
    Check(tail.len == WASM_MEMORY_PAGE_SIZE - 1);
    usize before = first.used;
    Check(BumpAllocate(&first, 1, 1).tag == BUMP_ALLOCATE_BACKING_UNAVAILABLE);
    Check(first.used == before && first.storage.ptr == base);
    Check(data.ptr[0] == 0x51 && data.ptr[data.len - 1] == 0xa7);

    bump_allocate_result second_data_result = BumpAllocate(&second, WASM_MEMORY_PAGE_SIZE + 1, 1);
    Check(second_data_result.tag == BUMP_ALLOCATE_OK);
    byte_slice second_data = second_data_result.allocation;
    Check(second_data.len == WASM_MEMORY_PAGE_SIZE + 1);
    u8 *end = second.storage.ptr + second.storage.len;
    Check(BumpDestroy(&second).tag == BUMP_DESTROY_OK);
    Check(BumpAllocate(&first, 1, 1).tag == BUMP_ALLOCATE_BACKING_UNAVAILABLE); /* No rewind/reuse after destroy. */
    Check(BumpDestroy(&first).tag == BUMP_DESTROY_OK);

    byte_slice next = {0};
    Check(WasmMemoryAllocate(1, 1, &next).tag == WASM_MEMORY_ALLOCATE_OK);
    Check(next.ptr == end);
    bump_allocator bounded;
    Check(BumpCreate(&bounded, 7).tag == BUMP_CREATE_OK);
    Check(BumpAllocate(&bounded, 7, 1).tag == BUMP_ALLOCATE_OK);
    Check(BumpAllocate(&bounded, 1, 1).tag == BUMP_ALLOCATE_NO_SPACE);
    Check(BumpDestroy(&bounded).tag == BUMP_DESTROY_OK);
    return (test_bump_arenas_result){.tag = TEST_BUMP_ARENAS_OK};
}

static test_parser_arena_result TestParserArena(void)
{
    test_parser_arena_result failure = {.tag = TEST_PARSER_ARENA_FAILED};
    http_parser parser;
    http_parse_limits limits = {.max_bytes = 2 * WASM_MEMORY_PAGE_SIZE, .max_headers = 4};
    Check(HttpParserCreate(&parser, 3 * WASM_MEMORY_PAGE_SIZE, limits).tag == HTTP_PARSER_CREATE_READY);
    u8 *base = parser.arena.storage.ptr;
    string_slice prefix = StringSlice("GET / HTTP/1.1\r\nX: ");
    Check(HttpParserParse(&parser, (const_byte_slice){(const u8 *)prefix.ptr, prefix.len}).status == HTTP_PARSE_NEED_MORE);
    u8 storage[1024];
    byte_slice input = {storage, sizeof(storage)};
    memset(input.ptr, 'a', input.len);
    for (usize index = 0; index < 65; ++index) {
        Check(HttpParserParse(&parser, (const_byte_slice){input.ptr, input.len}).status == HTTP_PARSE_NEED_MORE);
    }
    string_slice suffix = StringSlice("\r\n\r\nBODY");
    http_parse_result result = HttpParserParse(&parser, (const_byte_slice){(const u8 *)suffix.ptr, suffix.len});
    Check(result.status == HTTP_PARSE_COMPLETE && result.remainder.len == 4);
    const_byte_slice message = HttpParserMessage(&parser);
    Check(message.ptr == base && parser.arena.committed == WASM_MEMORY_PAGE_SIZE * 2);
    http_request request;
    http_header header;
    Check(ByteSliceCopyRange((byte_slice){(u8 *)&request, sizeof(request)}, 0, message, 0, sizeof(request)).tag == BYTE_SLICE_COPY_RANGE_OK);
    Check(ByteSliceCopyRange((byte_slice){(u8 *)&header, sizeof(header)}, 0, message, request.FirstHeaderOffset, sizeof(header)).tag == BYTE_SLICE_COPY_RANGE_OK);
    Check(header.ValueLength == 65 * input.len);
    const_byte_slice value;
    Check(ConstByteSliceSubslice(message, header.ValueOffset, header.ValueLength, &value).tag == CONST_BYTE_SLICE_SUBSLICE_OK);
    SliceFor(value, index) {
        Check(value.ptr[index] == 'a');
    }
    HttpParserDestroy(&parser);
    return (test_parser_arena_result){.tag = TEST_PARSER_ARENA_OK};
}

i32 TestMemory(void)
{
    /* The Wasmtime CLI boundary expects one scalar: zero or a source line. */
    test_global_allocator_result global = TestGlobalAllocator();
    if (global.tag != TEST_GLOBAL_ALLOCATOR_OK) return (i32)global.line;
    test_bump_arenas_result arenas = TestBumpArenas();
    if (arenas.tag != TEST_BUMP_ARENAS_OK) return (i32)arenas.line;
    test_parser_arena_result parser = TestParserArena();
    return parser.tag == TEST_PARSER_ARENA_OK ? 0 : (i32)parser.line;
}
