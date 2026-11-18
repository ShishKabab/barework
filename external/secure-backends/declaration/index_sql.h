#ifndef DECLARATION_INDEX_SQL_H
#define DECLARATION_INDEX_SQL_H
#include "physical_schema.h"
typedef enum index_physical_tag { INDEX_PHYSICAL_OK, INDEX_PHYSICAL_UNSUPPORTED, INDEX_PHYSICAL_NO_SPACE } index_physical_tag;
typedef struct index_physical_result { index_physical_tag tag; string_slice message; } index_physical_result;
/* Derive structural B-tree candidates. PhysicalDiff reconciles historical indexes. */
index_physical_result IndexPhysical(const declaration_ir *ir, physical_schema *schema);
#endif
