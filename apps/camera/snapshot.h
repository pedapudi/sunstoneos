#ifndef SUNSTONE_CAMERA_SNAPSHOT_H
#define SUNSTONE_CAMERA_SNAPSHOT_H
#include "session.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int camera_session_prepare(const struct a733_smp_contract *);
const struct camera_session *camera_admitted_session(void);
int camera_memory_profile_valid(void);
int a733_camera_memory_admitted(void);
#endif
