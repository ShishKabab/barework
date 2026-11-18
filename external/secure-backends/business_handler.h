#ifndef SECURE_BUSINESS_HANDLER_H
#define SECURE_BUSINESS_HANDLER_H
#include "query.h"
#include "business_protocol.h"

typedef enum business_json_response_tag { BUSINESS_JSON_RESPONSE_OK, BUSINESS_JSON_RESPONSE_FAILED } business_json_response_tag;
typedef struct business_json_response_result { business_json_response_tag tag; const_byte_slice response; } business_json_response_result;
/* Query failures are explicit data, not empty successful result sets. */
business_json_response_result BusinessJsonResponse(bump_allocator *arena, const_byte_slice message, const query_shape *shape);
#endif
