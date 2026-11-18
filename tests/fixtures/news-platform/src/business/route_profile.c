#include "route_profile.h"

route_profile_start_outcome RouteProfile_Start(route_profile_context *context)
{
    (void)context;
    return (route_profile_start_outcome){.tag = ROUTE_PROFILE_START_QUERY};
}

route_profile_get_profile_result_outcome RouteProfile_GetProfile_Result(
    route_profile_context *context, profile_get_profile_result result)
{
    business_json_response_result response = BusinessJsonResponse(context->arena, result.message, result.shape);
    if (response.tag != BUSINESS_JSON_RESPONSE_OK)
        return (route_profile_get_profile_result_outcome){.tag = ROUTE_PROFILE_GET_PROFILE_RESULT_FAILED};
    return (route_profile_get_profile_result_outcome){ROUTE_PROFILE_GET_PROFILE_RESULT_RESPONSE, response.response};
}
