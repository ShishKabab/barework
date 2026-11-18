#include "route_empty.h"

route_empty_start_outcome RouteEmpty_Start(route_empty_context *context)
{
    const http_response_header headers[] = {{StringSlice("Content-Type"), StringSlice("text/plain; charset=utf-8")}};
    http_response_build_result response = HttpResponseBuild(context->arena, 204,
        (http_response_header_slice){headers, 1}, (const_byte_slice){0});
    if (response.tag != HTTP_RESPONSE_BUILD_OK) return (route_empty_start_outcome){.tag = ROUTE_EMPTY_START_FAILED};
    return (route_empty_start_outcome){ROUTE_EMPTY_START_RESPONSE, response.message};
}
