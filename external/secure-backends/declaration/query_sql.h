#ifndef DECLARATION_QUERY_SQL_H
#define DECLARATION_QUERY_SQL_H
#include "physical_schema.h"
typedef enum query_physical_tag { QUERY_PHYSICAL_OK, QUERY_PHYSICAL_UNSUPPORTED, QUERY_PHYSICAL_NO_SPACE, QUERY_PHYSICAL_IO_ERROR } query_physical_tag;
typedef struct query_physical_result { query_physical_tag tag; string_slice message; } query_physical_result;
/* Appends versioned procedures and whole-table dependencies. SQL bodies and DROP
 * signatures are owned by schema. Caller destroys partial output on failure. */
query_physical_result QueryPhysical(const declaration_ir *ir, physical_schema *schema);
#endif
