#ifndef DECLARATION_ROUTES_H
#define DECLARATION_ROUTES_H
#include "ir.h"
#include <stdio.h>
typedef enum declaration_routes_tag { DECLARATION_ROUTES_OK, DECLARATION_ROUTES_INVALID, DECLARATION_ROUTES_IO_ERROR } declaration_routes_tag;
typedef struct declaration_routes_result { declaration_routes_tag tag; u32 offset, other_offset; string_slice message; } declaration_routes_result;
/* Successful unmodified resolved IR; streams are borrowed. Validate before emitting. */
declaration_routes_result DeclarationRoutes(const declaration_ir *ir, FILE *file, bool header);
#endif
