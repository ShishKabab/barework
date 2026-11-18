#ifndef DECLARATION_WORKFLOW_H
#define DECLARATION_WORKFLOW_H
#include "ir.h"
typedef enum workflow_generate_tag { WORKFLOW_GENERATE_OK, WORKFLOW_GENERATE_UNSUPPORTED, WORKFLOW_GENERATE_IO_ERROR } workflow_generate_tag;
typedef struct workflow_generate_result { workflow_generate_tag tag; string_slice message; } workflow_generate_result;
/* Write only into a task-owned staging directory. Never writes route .c files. */
workflow_generate_result WorkflowGenerate(const declaration_ir *ir, string_slice directory);
#endif
