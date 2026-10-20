#ifndef SUNSTONE_HEVC_SNAPSHOT_H
#define SUNSTONE_HEVC_SNAPSHOT_H
#include "a733_hevc_application.h"
#include "boot-record.h"
int hevc_session_prepare(const struct a733_smp_contract *);
const struct a733_hevc_session *hevc_admitted_session(void);
#endif
