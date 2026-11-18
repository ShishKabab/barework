#ifndef SECURE_BROKER_PROTOCOL_H
#define SECURE_BROKER_PROTOCOL_H
#include "business_protocol.h"
#include "query.h"
enum {
    BROKER_ADMITTED = BROKER_NAMESPACE + 1,
    BROKER_QUERY_REQUEST, BROKER_EXECUTE_QUERY, BROKER_QUERY_RESULT,
    BROKER_FINISH, BROKER_FINISHED, BROKER_REJECTED, BROKER_REQUEST,
};
/* Only operation/sequence travel from business to broker. Route-derived
 * arguments are constructed by the broker from its immutable request copy. */
#endif
