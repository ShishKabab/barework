#ifndef SECURE_BACKENDS_HTTP_H
#define SECURE_BACKENDS_HTTP_H

#include "bump.h"

/* Relocatable, pointer-free message layout. Every offset is measured from the
 * first byte of http_request, including NextHeaderOffset. Zero ends the list
 * and represents absent/empty data. Strings are not null-terminated.
 * Header order, duplicates, and key case are preserved; value OWS is trimmed.
 * All fields are u32 in native byte order (little-endian on our native/WASM
 * targets). This is an in-memory ABI, not a portable network serialization.
 * Decode structs using checked slice copies if the range is not suitably aligned.
 * Validate offsets before using a message received from an untrusted guest. */
typedef struct http_header {
    u32 KeyOffset;
    u32 KeyLength;
    u32 ValueOffset;
    u32 ValueLength;
    u32 NextHeaderOffset;
} http_header;

typedef struct http_request {
    u32 MessageLength;
    u32 MethodOffset;
    u32 MethodLength;
    u32 TargetOffset;
    u32 TargetLength;
    u32 VersionMajor;
    u32 VersionMinor;
    u32 FirstHeaderOffset;
    u32 HeaderCount;
} http_request;

_Static_assert(sizeof(http_header) == 20, "http_header ABI changed");
_Static_assert(sizeof(http_request) == 36, "http_request ABI changed");

typedef enum http_parse_status {
    HTTP_PARSE_NEED_MORE,
    HTTP_PARSE_COMPLETE,
    HTTP_PARSE_INVALID,
    HTTP_PARSE_NO_SPACE,
    HTTP_PARSE_LIMIT,
    HTTP_PARSE_TRUNCATED,
} http_parse_status;

typedef struct http_parse_limits {
    u32 max_bytes;   /* Wire bytes through the final CRLF, including whitespace. */
    u32 max_headers;
} http_parse_limits;

typedef struct http_parse_result {
    http_parse_status status;
    usize consumed;
    const_byte_slice remainder; /* Borrowed suffix of this call's input. */
} http_parse_result;

/* Parser state is separate from the message and is never part of its copy.
 * Treat these fields as private. One parser owns its storage exclusively. */
typedef struct http_parser {
    bump_allocator arena;
    http_request request;
    http_header header;
    http_parse_limits limits;
    http_parse_status status;
    usize root_start;
    u32 header_offset;
    u32 previous_header_offset;
    u32 wire_bytes;
    u32 version_index;
    u32 state;
} http_parser;

http_parse_limits HttpDefaultLimits(void);
typedef enum http_parser_init_tag { HTTP_PARSER_INIT_READY, HTTP_PARSER_INIT_INVALID_ARGUMENT, HTTP_PARSER_INIT_NO_SPACE } http_parser_init_tag;
typedef struct http_parser_init_result { http_parser_init_tag tag; } http_parser_init_result;
typedef enum http_parser_create_tag { HTTP_PARSER_CREATE_READY, HTTP_PARSER_CREATE_INVALID_ARGUMENT, HTTP_PARSER_CREATE_NO_SPACE } http_parser_create_tag;
typedef struct http_parser_create_result { http_parser_create_tag tag; } http_parser_create_result;
typedef enum http_parser_reset_tag { HTTP_PARSER_RESET_READY, HTTP_PARSER_RESET_INVALID_ARGUMENT, HTTP_PARSER_RESET_NO_SPACE } http_parser_reset_tag;
typedef struct http_parser_reset_result { http_parser_reset_tag tag; } http_parser_reset_result;
typedef enum http_parser_finish_tag {
    HTTP_PARSER_FINISH_COMPLETE, HTTP_PARSER_FINISH_TRUNCATED, HTTP_PARSER_FINISH_INVALID,
    HTTP_PARSER_FINISH_NO_SPACE, HTTP_PARSER_FINISH_LIMIT,
} http_parser_finish_tag;
typedef struct http_parser_finish_result { http_parser_finish_tag tag; } http_parser_finish_result;
/* Storage must be stable, must not overlap parser/input, and must outlive the
 * returned message. It may be unaligned; initialization aligns the root.
 * Capacity is limited to UINT32_MAX. Success returns HTTP_PARSER_INIT_READY. */
http_parser_init_result HttpParserInit(http_parser *parser, byte_slice storage,
                                http_parse_limits limits);
/* Owns a growable contiguous arena instead of borrowing fixed storage. The
 * maximum message size is bounded by arena_limit (at most UINT32_MAX).
 * Destroy before reinitializing. Wasm storage lasts until instance teardown. */
http_parser_create_result HttpParserCreate(http_parser *parser, usize arena_limit,
                                  http_parse_limits limits);
/* Invalidates the message; frees owned host storage, never borrowed storage. */
void HttpParserDestroy(http_parser *parser);
/* Start a new request using the existing arena. Invalidates the old message;
 * callers must have copied/finished using it. Retains backing allocation. */
http_parser_reset_result HttpParserReset(http_parser *parser);
/* Parses strict CRLF-delimited HTTP/1.0 or HTTP/1.1 request line + headers.
 * Rejects bare LF, folded headers, invalid tokens, and value control bytes.
 * Stops exactly after CRLF CRLF; does not consume or interpret the body.
 * No Host/Content-Length/Transfer-Encoding policy or URI decoding is performed.
 * COMPLETE means syntactically parsed headers, not an authorized/framed request.
 * Input is copied as needed; no input pointer survives the call.
 * NEED_MORE consumes all input. On errors consumed excludes the rejected byte.
 * Complete/error states are sticky and consume zero on subsequent calls.
 * Empty input is not EOF. Use Finish to report an early disconnect. */
http_parse_result HttpParserParse(http_parser *parser, const_byte_slice input);
http_parser_finish_result HttpParserFinish(http_parser *parser);
/* Returns the exact copyable root + data range only after COMPLETE. */
const_byte_slice HttpParserMessage(const http_parser *parser);

#endif
