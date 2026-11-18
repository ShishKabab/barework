#include "route_profile_summary.h"

route_profile_summary_start_outcome RouteProfileSummary_Start(route_profile_summary_context *context)
{
    (void)context;
    return (route_profile_summary_start_outcome){.tag = ROUTE_PROFILE_SUMMARY_START_QUERY};
}

route_profile_summary_get_profile_result_outcome RouteProfileSummary_GetProfile_Result(
    route_profile_summary_context *context, profile_summary_get_profile_result result)
{
    if (result.root.outcome == QUERY_SUCCESS)
        return (route_profile_summary_get_profile_result_outcome){.tag = ROUTE_PROFILE_SUMMARY_GET_PROFILE_RESULT_QUERY};
    business_json_response_result response = BusinessJsonResponse(context->arena, result.message, result.shape);
    if (response.tag != BUSINESS_JSON_RESPONSE_OK)
        return (route_profile_summary_get_profile_result_outcome){.tag = ROUTE_PROFILE_SUMMARY_GET_PROFILE_RESULT_FAILED};
    return (route_profile_summary_get_profile_result_outcome){ROUTE_PROFILE_SUMMARY_GET_PROFILE_RESULT_RESPONSE, response.response};
}

route_profile_summary_get_contact_result_outcome RouteProfileSummary_GetContact_Result(
    route_profile_summary_context *context, profile_summary_get_contact_result result)
{
    /* Return the retained profile after the contact lookup. Its backing storage
     * must survive both yields; the generated state owns the borrowed view. */
    app_query_shape_result profile = AppQueryShape(BROKER_OPERATION_PROFILE_SUMMARY_GET_PROFILE);
    if (profile.tag != APP_QUERY_SHAPE_OK)
        return (route_profile_summary_get_contact_result_outcome){.tag = ROUTE_PROFILE_SUMMARY_GET_CONTACT_RESULT_FAILED};
    business_json_response_result response = result.root.outcome == QUERY_SUCCESS ?
        BusinessJsonResponse(context->arena, context->state->get_profile, profile.shape) :
        BusinessJsonResponse(context->arena, result.message, result.shape);
    if (response.tag != BUSINESS_JSON_RESPONSE_OK)
        return (route_profile_summary_get_contact_result_outcome){.tag = ROUTE_PROFILE_SUMMARY_GET_CONTACT_RESULT_FAILED};
    return (route_profile_summary_get_contact_result_outcome){ROUTE_PROFILE_SUMMARY_GET_CONTACT_RESULT_RESPONSE, response.response};
}
