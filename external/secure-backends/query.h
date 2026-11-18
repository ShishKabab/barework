#ifndef SECURE_QUERY_H
#define SECURE_QUERY_H
#include "http_response_message.h"
#include "slice.h"

/* Query messages are deployment-local, little-endian and root-relative.
 * records.bytes counts bytes; records.count counts fixed-size row elements. */
enum { QUERY_MESSAGE_LIMIT = 16384, QUERY_ROW_LIMIT = 32, QUERY_DEPTH_LIMIT = 8 };
typedef struct query_records { byte_range bytes; u32 count; } query_records;
typedef enum query_outcome { QUERY_SUCCESS, QUERY_BACKEND_FAILED, QUERY_INVALID_PARAMETER } query_outcome;
typedef struct query_result { u32 operation, sequence, outcome; query_records rows; } query_result;
typedef struct query_arguments { u32 operation, sequence; u64 id; } query_arguments;
typedef struct query_call { u32 operation, sequence; } query_call;
_Static_assert(sizeof(query_result) == 24 && sizeof(query_arguments) == 16, "query ABI");

typedef enum query_field_kind { QUERY_ID, QUERY_TEXT, QUERY_BOOL, QUERY_NUMBER, QUERY_TIMESTAMP, QUERY_RELATION } query_field_kind;
typedef struct query_shape query_shape;
typedef struct query_field {
    string_slice name;
    query_field_kind kind;
    usize offset;
    bool optional, many;
    const query_shape *nested;
} query_field;
typedef struct query_field_slice { const query_field *ptr; usize len; } query_field_slice;
struct query_shape { usize row_size; query_field_slice fields; };
/* Scalars have an explicit presence tag; optional absence is distinct from an
 * empty string or numeric zero. These generated field layouts cross guests. */
typedef struct query_id { u32 present, reserved; u64 value; } query_id;
typedef struct query_text { u32 present; byte_range value; } query_text;
typedef struct query_bool { u32 present, value; } query_bool;
typedef struct query_number { u32 present, reserved; f64 value; } query_number;
typedef struct query_timestamp { u32 present, reserved; i64 value; } query_timestamp;

typedef enum query_validate_tag { QUERY_VALIDATE_OK, QUERY_VALIDATE_INVALID } query_validate_tag;
typedef struct query_validate_result { query_validate_tag tag; query_result root; } query_validate_result;
query_validate_result QueryValidate(const_byte_slice message, u32 operation, u32 sequence, const query_shape *shape);
typedef enum query_fake_tag { QUERY_FAKE_OK, QUERY_FAKE_NO_SPACE, QUERY_FAKE_INVALID } query_fake_tag;
typedef struct query_fake_result { query_fake_tag tag; const_byte_slice message; } query_fake_result;
/* Native synthetic backend only: 0 = empty, 2 = absent optional fields,
 * 13 = backend error. Other IDs produce bounded populated nested results. */
query_fake_result QueryFake(query_arguments arguments, const query_shape *shape, byte_slice output);
typedef enum query_json_tag { QUERY_JSON_OK, QUERY_JSON_INVALID, QUERY_JSON_NO_SPACE } query_json_tag;
typedef struct query_json_result { query_json_tag tag; const_byte_slice bytes; } query_json_result;
query_json_result QueryJson(const_byte_slice message, const query_shape *shape, byte_slice output);
typedef enum query_id_parse_tag { QUERY_ID_PARSE_OK, QUERY_ID_PARSE_INVALID } query_id_parse_tag;
typedef struct query_id_parse_result { query_id_parse_tag tag; u64 value; } query_id_parse_result;
query_id_parse_result QueryIdParse(const_byte_slice text);
#endif
