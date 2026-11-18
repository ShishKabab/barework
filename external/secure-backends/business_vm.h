#ifndef SECURE_BUSINESS_VM_H
#define SECURE_BUSINESS_VM_H
#include "business_protocol.h"
typedef struct business_runtime business_runtime;
typedef struct business_session business_session;
typedef enum business_runtime_create_tag {
    BUSINESS_RUNTIME_CREATE_OK, BUSINESS_RUNTIME_CREATE_INVALID_ARGUMENT,
    BUSINESS_RUNTIME_CREATE_OUT_OF_MEMORY, BUSINESS_RUNTIME_CREATE_INVALID_MODULE,
} business_runtime_create_tag;
typedef struct business_runtime_create_result { business_runtime_create_tag tag; } business_runtime_create_result;
business_runtime_create_result BusinessRuntimeCreate(const_byte_slice module, business_runtime **output);
void BusinessRuntimeDestroy(business_runtime *runtime);
/* Diagnostic/admission predicate: false means fresh-instance fallback. */
bool BusinessRuntimeCanReset(const business_runtime *runtime);
typedef enum business_run_tag {
    BUSINESS_RUN_OK, BUSINESS_RUN_INVALID_ARGUMENT, BUSINESS_RUN_OUT_OF_MEMORY,
    BUSINESS_RUN_INVALID_GUEST, BUSINESS_RUN_OUTPUT_TOO_SMALL,
} business_run_tag;
typedef struct business_run_result { business_run_tag tag; const_byte_slice response; } business_run_result;
/* Shared compiled module; one idle instance per runtime per calling thread.
 * Compatible guests are fully reset after successful calls, retaining grown
 * memory. Traps/invalid output discard the instance; unsupported reset profiles
 * use fresh instances. OK response borrows caller-owned output. No guest pointer
 * escapes. Input is a validated route dispatch envelope. Join all other runtime users
 * before Destroy: thread exit releases their caches, Destroy releases this
 * thread's cache. BusinessRun is synchronous with no imported callbacks. */
business_run_result BusinessRun(business_runtime *runtime, const_byte_slice request, byte_slice output);

typedef enum broker_runtime_create_tag { BROKER_RUNTIME_CREATE_OK, BROKER_RUNTIME_CREATE_INVALID, BROKER_RUNTIME_CREATE_NO_SPACE } broker_runtime_create_tag;
typedef struct broker_runtime_create_result { broker_runtime_create_tag tag; } broker_runtime_create_result;
/* Separate engine-owned pool, broker exports, always destroyed at request end. */
broker_runtime_create_result BrokerRuntimeCreate(const_byte_slice module, business_runtime **output);
typedef enum business_session_open_tag { BUSINESS_SESSION_OPEN_OK, BUSINESS_SESSION_OPEN_INVALID, BUSINESS_SESSION_OPEN_NO_SPACE } business_session_open_tag;
typedef struct business_session_open_result { business_session_open_tag tag; business_session *session; } business_session_open_result;
business_session_open_result BusinessSessionOpen(business_runtime *runtime, const_byte_slice request);
typedef enum business_session_step_tag { BUSINESS_SESSION_STEP_OK, BUSINESS_SESSION_STEP_INVALID, BUSINESS_SESSION_STEP_NO_SPACE, BUSINESS_SESSION_STEP_WRONG_THREAD } business_session_step_tag;
typedef struct business_session_step_result { business_session_step_tag tag; u32 type; const_byte_slice output; } business_session_step_result;
/* Synchronous steps, owner-thread only. Live sessions remain unavailable to the
 * idle cache across yields. Output is copied into caller storage before return. */
business_session_step_result BusinessSessionStep(business_session *session, u32 event, const_byte_slice input, byte_slice output);
typedef enum business_session_close_tag { BUSINESS_SESSION_CLOSE_OK, BUSINESS_SESSION_CLOSE_WRONG_THREAD } business_session_close_tag;
typedef struct business_session_close_result { business_session_close_tag tag; } business_session_close_result;
/* Reuse only after the host accepted a final response and broker completion. */
business_session_close_result BusinessSessionClose(business_session *session, bool reuse);
#endif
