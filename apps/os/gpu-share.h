#ifndef SUNSTONE_OS_GPU_SHARE_H
#define SUNSTONE_OS_GPU_SHARE_H
#include "sunstone_app.h"
#define OS_GPU_PEER_STACK_BYTES 16384u
/* Pure admission: no task creation or device access. */
int os_gpu_share_admit(const struct ss_gpu_share *,const struct ss_manifest *,
                       const struct ss_grant *,struct ss_grant *);
#endif
