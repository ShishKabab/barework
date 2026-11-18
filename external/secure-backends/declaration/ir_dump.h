#ifndef DECLARATION_IR_DUMP_H
#define DECLARATION_IR_DUMP_H
#include "ir.h"
#include <stdio.h>
/* stdio adapter: caller owns stream. Checks writes and flush before returning. */
typedef enum declaration_ir_dump_tag { DECLARATION_IR_DUMP_OK, DECLARATION_IR_DUMP_IO_ERROR, DECLARATION_IR_DUMP_INVALID } declaration_ir_dump_tag;
typedef struct declaration_ir_dump_result { declaration_ir_dump_tag tag; } declaration_ir_dump_result;
declaration_ir_dump_result DeclarationIrDump(const declaration_ir *ir, FILE *stream);
#endif
