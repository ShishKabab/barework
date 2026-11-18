#include "route_timeline.h"

route_timeline_start_outcome RouteTimeline_Start(route_timeline_context *context)
{
    (void)context;
    return (route_timeline_start_outcome){.tag = ROUTE_TIMELINE_START_QUERY};
}

route_timeline_get_timeline_result_outcome RouteTimeline_GetTimeline_Result(
    route_timeline_context *context, timeline_get_timeline_result result)
{
    business_json_response_result response = BusinessJsonResponse(context->arena, result.message, result.shape);
    if (response.tag != BUSINESS_JSON_RESPONSE_OK)
        return (route_timeline_get_timeline_result_outcome){.tag = ROUTE_TIMELINE_GET_TIMELINE_RESULT_FAILED};
    return (route_timeline_get_timeline_result_outcome){ROUTE_TIMELINE_GET_TIMELINE_RESULT_RESPONSE, response.response};
}
