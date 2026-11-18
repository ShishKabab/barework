#include "app-routes.h"
#include <assert.h>
#include <stdio.h>

static u8 request_storage[2048], arena_storage[4096];
static bump_allocator arena;
static const_byte_slice Request(string_slice path)
{
    byte_slice storage = {request_storage, sizeof(request_storage)};
    http_request root = {.MessageLength = (u32)(sizeof(root) + path.len), .TargetOffset = sizeof(root), .TargetLength = (u32)path.len};
    assert(ByteSliceCopy(storage, 0, BytesOf(root)).tag == BYTE_SLICE_COPY_OK);
    assert(ByteSliceCopy(storage, sizeof(root), (const_byte_slice){(const u8 *)path.ptr, path.len}).tag == BYTE_SLICE_COPY_OK);
    return (const_byte_slice){storage.ptr, root.MessageLength};
}
static route_match_result Match(string_slice path)
{
    BumpReset(&arena);
    return RouteMatch(Request(path), &arena);
}
static app_route_payload Expect(string_slice path, u32 tag)
{
    route_match_result matched = Match(path);
    assert(matched.tag == ROUTE_MATCH_OK);
    app_route_payload payload;
    assert(RouteRequestRead(matched.message, &payload).tag == ROUTE_REQUEST_READ_OK);
    assert(payload.tag == tag);
    return payload;
}
static void Missing(string_slice path)
{
    assert(Match(path).tag == ROUTE_MATCH_NOT_FOUND);
    assert(arena.used == 0);
}
int main(void)
{
    assert(BumpInit(&arena, (byte_slice){arena_storage, sizeof(arena_storage)}).tag == BUMP_INIT_OK);
    Expect(StringSlice("/"), APP_ROUTE_home);
    Expect(StringSlice("/?q=/else"), APP_ROUTE_home);
    Expect(StringSlice("/profile/new"), APP_ROUTE_newProfile);
    Expect(StringSlice("/profile/new?x=1"), APP_ROUTE_newProfile);
    app_route_payload payload = Expect(StringSlice("/profile/a%2Fb?x=/"), APP_ROUTE_profile);
    const_byte_slice dispatch = {arena.storage.ptr, arena.used};
    const_byte_range_resolve_result id = ConstByteRangeResolve(dispatch, payload.captures.route_profile.capture_id);
    assert(id.tag == CONST_BYTE_RANGE_RESOLVE_OK);
    assert(RouteLiteralEqual(id.bytes, StringSlice("a%2Fb")));
    /* Relocation preserves captures and the independent nested HTTP origin. */
    u8 copy_storage[4096];
    byte_slice copy = {copy_storage, dispatch.len};
    assert(ByteSliceCopy(copy, 0, dispatch).tag == BYTE_SLICE_COPY_OK);
    const_byte_slice copied = {copy.ptr, copy.len};
    assert(RouteRequestRead(copied, &payload).tag == ROUTE_REQUEST_READ_OK);
    id = ConstByteRangeResolve(copied, payload.captures.route_profile.capture_id);
    assert(id.tag == CONST_BYTE_RANGE_RESOLVE_OK && RouteLiteralEqual(id.bytes, StringSlice("a%2Fb")));
    Expect(StringSlice("/a/b/end"), APP_ROUTE_literalFirst);
    payload = Expect(StringSlice("/a/b/fallback"), APP_ROUTE_parameterFirst);
    assert(payload.captures.route_parameterFirst.capture_first.len == 1);
    Expect(StringSlice("/a/b/end/"), APP_ROUTE_trailing);
    Missing(StringSlice("/a/b/end//"));
    Missing(StringSlice("/profile/"));
    Missing(StringSlice("/profile//id"));
    Missing(StringSlice("/PROFILE/id"));
    Missing(StringSlice("/profile/id/"));
    Missing(StringSlice("/profile/id/more"));
    Expect(StringSlice("/quote/\"\\"), APP_ROUTE_escaped);
    /* Fields cannot escape the path, enter the query, or wrap arithmetic. */
    route_request root;
    assert(ByteSliceCopyRange((byte_slice){(u8 *)&root, sizeof(root)}, 0, copied, 0, sizeof(root)).tag == BYTE_SLICE_COPY_RANGE_OK);
    assert(RouteRequestRead(copied, &payload).tag == ROUTE_REQUEST_READ_OK);
    app_route_payload bad = payload;
    bad.tag = UINT32_MAX;
    assert(ByteSliceCopy(copy, root.payload.index, BytesOf(bad)).tag == BYTE_SLICE_COPY_OK);
    assert(RouteRequestRead(copied, &bad).tag == ROUTE_REQUEST_READ_INVALID);
    bad = payload; bad.captures.route_profile.capture_id = (byte_range){UINT32_MAX, UINT32_MAX};
    assert(ByteSliceCopy(copy, root.payload.index, BytesOf(bad)).tag == BYTE_SLICE_COPY_OK);
    assert(RouteRequestRead(copied, &bad).tag == ROUTE_REQUEST_READ_INVALID);
    bad = payload; bad.captures.route_profile.capture_id.index += bad.captures.route_profile.capture_id.len + 1;
    assert(ByteSliceCopy(copy, root.payload.index, BytesOf(bad)).tag == BYTE_SLICE_COPY_OK);
    assert(RouteRequestRead(copied, &bad).tag == ROUTE_REQUEST_READ_INVALID);
    assert(ByteSliceCopy(copy, root.payload.index, BytesOf(payload)).tag == BYTE_SLICE_COPY_OK);
    assert(RouteRequestRead((const_byte_slice){copy.ptr, copy.len - 1}, &bad).tag == ROUTE_REQUEST_READ_INVALID);
    root.http.index = UINT32_MAX;
    assert(ByteSliceCopy(copy, 0, BytesOf(root)).tag == BYTE_SLICE_COPY_OK);
    assert(RouteRequestRead(copied, &bad).tag == ROUTE_REQUEST_READ_INVALID);
    bump_allocator tiny;
    u8 tiny_storage[1];
    assert(BumpInit(&tiny, (byte_slice){tiny_storage, sizeof(tiny_storage)}).tag == BUMP_INIT_OK);
    assert(RouteMatch(Request(StringSlice("/profile/id")), &tiny).tag == ROUTE_MATCH_NO_SPACE && tiny.used == 0);
    assert(RouteMatch((const_byte_slice){NULL, 1}, &tiny).tag == ROUTE_MATCH_INVALID_REQUEST);
    assert(RouteMatch(Request(StringSlice("/")), NULL).tag == ROUTE_MATCH_INVALID_REQUEST);
    puts("PASS: generated routing precedence, fallback, captures, relocation, validation, allocation bounds");
}
