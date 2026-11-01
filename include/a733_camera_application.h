/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_CAMERA_APPLICATION_H
#define SUNSTONE_A733_CAMERA_APPLICATION_H
#include "a733_camera_native.h"
#include "sunstone_camera.h"
/* Fresh zero storage, private retained arguments and one privileged serialized consumer of one admitted native stream. Views refer
 * directly to guarded DMA slots and remain held until release. This contract
 * does not create EL0 mappings. The native retained path cannot prove drain;
 * stop drives the sensor quiet and returns its retained fault. */
struct a733_camera_application {
    struct a733_camera_native *native;
    uint64_t next_token,token;
    unsigned slot,starting,started,stopping,failed;
};
int a733_camera_application_init(struct a733_camera_application *,struct a733_camera_native *,
    struct sunstone_camera *);
#endif
