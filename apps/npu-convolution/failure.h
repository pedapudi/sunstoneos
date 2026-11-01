#ifndef SUNSTONE_NPU_APPLICATION_FAILURE_H
#define SUNSTONE_NPU_APPLICATION_FAILURE_H
#include "sunstone_crumb.h"
/* Preserve the failing predicate before application_fail collapses it to a
 * lifecycle reason. The macro records its expansion's line and instruction.
 * Only a failed predicate evaluates value; successful checks perform no SVC. */
#define NPU_APPLICATION_CHECK(condition,value) ((condition) ? 1 : \
    (sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),(uint32_t)(value),SUNSTONE_CRUMB_HERE()),0))
#endif
