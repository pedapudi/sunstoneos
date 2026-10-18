#ifndef SUNSTONE_HDMI_SNAPSHOT_H
#define SUNSTONE_HDMI_SNAPSHOT_H
#include "session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int hdmi_session_prepare(const struct a733_smp_contract *);
const struct hdmi_session *hdmi_admitted_session(void);
#endif
