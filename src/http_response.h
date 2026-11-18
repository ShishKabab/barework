#ifndef NEWS_PLATFORM_HTTP_RESPONSE_H
#define NEWS_PLATFORM_HTTP_RESPONSE_H

#include "http_response_message.h"

/* Serialized wire bytes plus connection policy, distinct from http_response. */
typedef struct http_wire_response {
    string_slice bytes;
    i32 keep_alive;
    u32 code;
    i32 head;
} http_wire_response;
/* Static response bytes survive parser destruction. Persistence is permitted
 * only for fully framed, bodyless requests; errors/unsupported bodies close. */
http_wire_response HttpResponse(const_byte_slice message, http_parse_status status);
typedef enum http_response_serialize_tag { HTTP_RESPONSE_SERIALIZE_OK, HTTP_RESPONSE_SERIALIZE_INVALID } http_response_serialize_tag;
typedef struct http_response_serialize_result { http_response_serialize_tag tag; const_byte_slice bytes; } http_response_serialize_result;
http_response_serialize_result HttpResponseSerialize(const_byte_slice message, i32 head, i32 keep_alive, byte_slice output);
http_wire_response HttpFailureResponse(void);
http_wire_response HttpNotFoundResponse(i32 head, i32 keep_alive);

#endif
