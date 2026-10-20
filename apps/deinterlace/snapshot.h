#ifndef SUNSTONE_DEINTERLACE_SNAPSHOT_H
#define SUNSTONE_DEINTERLACE_SNAPSHOT_H
#include "a733_deinterlace_application.h"
#include "boot-record.h"
int deinterlace_session_prepare(const struct a733_smp_contract *);
const struct a733_deinterlace_session *deinterlace_admitted_session(void);
const uint8_t *deinterlace_admitted_session_digest(void);
#endif
