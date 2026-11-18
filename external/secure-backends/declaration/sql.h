#ifndef DECLARATION_SQL_H
#define DECLARATION_SQL_H
#include "physical_schema.h"
bool DeclarationSqlText(string_slice text);
bool DeclarationSqlIdentifier(string_slice text);
typedef enum physical_build_tag { PHYSICAL_BUILD_OK, PHYSICAL_BUILD_UNSUPPORTED, PHYSICAL_BUILD_NO_SPACE, PHYSICAL_BUILD_IO_ERROR } physical_build_tag;
typedef struct physical_build_result { physical_build_tag tag; string_slice message; } physical_build_result;
/* Lower successful, unmodified IR into an owned desired physical schema.
 * Output must be fresh. Caller destroys partial output on failure. */
physical_build_result PhysicalBuild(const declaration_ir *ir, physical_schema *schema);
#endif
