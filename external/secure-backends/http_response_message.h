#ifndef SECURE_HTTP_RESPONSE_MESSAGE_H
#define SECURE_HTTP_RESPONSE_MESSAGE_H
#include "http.h"

/* Relocatable HTTP response. All ranges/offsets refer to this root. Headers
 * use the shared http_header layout. Empty body is {0, 0}. This is the coupled
 * deployment's little-endian ABI; it contains no addresses or framing headers. */
typedef struct http_response {
    u32 MessageLength, StatusCode, FirstHeaderOffset, HeaderCount;
    byte_range Body;
} http_response;
enum { HTTP_RESPONSE_LIMIT = 3072, HTTP_RESPONSE_HEADER_LIMIT = 32 };
_Static_assert(sizeof(http_response) == 24, "HTTP response ABI");
_Static_assert(offsetof(http_response, Body) == 16, "HTTP response body ABI");

/* Borrowed inputs. Duplicate headers and input order are preserved. */
typedef struct http_response_header { string_slice key, value; } http_response_header;
typedef struct http_response_header_slice { const http_response_header *ptr; usize len; } http_response_header_slice;
typedef enum http_response_build_tag {
    HTTP_RESPONSE_BUILD_OK, HTTP_RESPONSE_BUILD_INVALID_ARGUMENT,
    HTTP_RESPONSE_BUILD_LIMIT, HTTP_RESPONSE_BUILD_NO_SPACE,
} http_response_build_tag;
typedef struct http_response_build_result {
    http_response_build_tag tag;
    const_byte_slice message; /* Owned by arena, valid until reset/destruction. */
} http_response_build_result;
/* Validate and size first, then allocate once and copy all input bytes. Inputs
 * must remain live through this call, disjoint from the arena's unused storage.
 * Failure does not allocate or modify the arena. Status/body/header policy is
 * shared with the boundary validator. Connection code supplies HTTP framing. */
http_response_build_result HttpResponseBuild(bump_allocator *arena, u32 status,
    http_response_header_slice headers, const_byte_slice body);
/* Validates header syntax and forbids application-supplied framing headers. */
bool HttpResponseHeaderValid(string_slice key, string_slice value);
typedef enum http_response_validate_tag {
    HTTP_RESPONSE_VALIDATE_OK, HTTP_RESPONSE_VALIDATE_INVALID,
} http_response_validate_tag;
typedef struct http_response_validate_result { http_response_validate_tag tag; } http_response_validate_result;
/* Checks bounds, list structure, syntax and status/body restrictions. */
http_response_validate_result HttpResponseValidate(const_byte_slice message);
#endif
