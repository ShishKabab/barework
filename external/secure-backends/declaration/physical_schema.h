#ifndef DECLARATION_PHYSICAL_SCHEMA_H
#define DECLARATION_PHYSICAL_SCHEMA_H
#include "ir.h"
#include <stdio.h>

/* Owned records, independent of IR lifetime. Definitions are SQL only for column
 * types and procedure bodies. Index keys and dependencies remain structural.
 * TABLE: name. COLUMN: owner=table, name, value=type, detail=flags.
 * INDEX: owner=table, name. INDEX_KEY: owner=index, name=column, detail=ordinal.
 * PROCEDURE: name, owner=DROP signature, value=CREATE SQL, detail=app version.
 * DEPENDENCY: owner=procedure, name=table (whole-row dependency, conservative).
 */
typedef enum physical_kind { PHYSICAL_TABLE = 1, PHYSICAL_COLUMN, PHYSICAL_INDEX, PHYSICAL_INDEX_KEY, PHYSICAL_PROCEDURE, PHYSICAL_DEPENDENCY } physical_kind;
enum { PHYSICAL_OPTIONAL = 1, PHYSICAL_PRIMARY = 2 };
typedef struct physical_record { physical_kind kind; u32 detail; string_slice owner, name, value; } physical_record;
typedef struct physical_record_slice { physical_record *ptr; usize len; } physical_record_slice;
typedef struct physical_schema { u32 version; physical_record_slice records; usize capacity; } physical_schema;
typedef enum physical_add_tag { PHYSICAL_ADD_OK, PHYSICAL_ADD_NO_SPACE } physical_add_tag;
typedef struct physical_add_result { physical_add_tag tag; } physical_add_result;
physical_add_result PhysicalAdd(physical_schema *schema, physical_record record);
void PhysicalDestroy(physical_schema *schema);
typedef enum physical_read_tag { PHYSICAL_READ_OK, PHYSICAL_READ_INVALID, PHYSICAL_READ_NO_SPACE } physical_read_tag;
typedef struct physical_read_result { physical_read_tag tag; } physical_read_result;
physical_read_result PhysicalRead(const_byte_slice bytes, physical_schema *schema);
typedef enum physical_write_tag { PHYSICAL_WRITE_OK, PHYSICAL_WRITE_IO_ERROR } physical_write_tag;
typedef struct physical_write_result { physical_write_tag tag; } physical_write_result;
physical_write_result PhysicalWrite(const physical_schema *schema, FILE *stream);
typedef enum physical_diff_tag { PHYSICAL_DIFF_OK, PHYSICAL_DIFF_INVALID, PHYSICAL_DIFF_UNSUPPORTED, PHYSICAL_DIFF_NO_SPACE, PHYSICAL_DIFF_IO_ERROR } physical_diff_tag;
typedef struct physical_diff_result { physical_diff_tag tag; string_slice message; } physical_diff_result;
/* Plans and validates the complete diff before emitting. Result includes retained
 * historical objects. Both output schemas must be fresh; caller destroys them. */
physical_diff_result PhysicalDiff(const physical_schema *desired, const physical_schema *previous, physical_schema *result, FILE *migration);
bool PhysicalEqual(string_slice a, string_slice b);
#endif
