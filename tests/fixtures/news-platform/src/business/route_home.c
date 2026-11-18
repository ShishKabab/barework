#include "route_home.h"

route_home_start_outcome RouteHome_Start(route_home_context *context)
{
    const http_response_header headers[] = {{StringSlice("Content-Type"), StringSlice("text/plain; charset=utf-8")}};
    string_slice greeting = StringSlice("Hello, world!\n");
    http_response_build_result response = HttpResponseBuild(context->arena, 200,
        (http_response_header_slice){headers, 1}, (const_byte_slice){(const u8 *)greeting.ptr, greeting.len});
    if (response.tag != HTTP_RESPONSE_BUILD_OK) return (route_home_start_outcome){.tag = ROUTE_HOME_START_FAILED};
    return (route_home_start_outcome){ROUTE_HOME_START_RESPONSE, response.message};
}
