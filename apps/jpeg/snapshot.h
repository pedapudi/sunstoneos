#ifndef SUNSTONE_JPEG_SNAPSHOT_H
#define SUNSTONE_JPEG_SNAPSHOT_H
#include "a733_jpeg_application.h"
struct a733_smp_contract;
/* Primary EL3 calls once with its already validated private contract. */
int jpeg_session_prepare(const struct a733_smp_contract *);
const struct a733_jpeg_session *jpeg_admitted_session(void);
#endif
