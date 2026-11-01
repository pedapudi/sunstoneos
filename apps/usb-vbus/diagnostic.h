/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef USB_VBUS_DIAGNOSTIC_H
#define USB_VBUS_DIAGNOSTIC_H
#include <stdint.h>
struct vbus_ops {
    uint64_t (*now)(void *);
    int (*io)(void *,unsigned,unsigned,uint8_t *);
    int (*output)(void *,unsigned);
};
struct vbus_diagnostic {
    struct vbus_ops ops;void *context;
    uint64_t deadline,last,until,on_at,off_at,rise_us,fall_us,max_gap;
    unsigned mode,state,step,substep,failure,done,enabled,rose,fell,cc_samples;
    uint8_t value;
};
/* The runner exclusively admits the dedicated pin and controller. These
 * checks establish operational state, without granting attachment authority. */
void vbus_begin(struct vbus_diagnostic *,const struct vbus_ops *,void *,unsigned,uint64_t);
int vbus_step(struct vbus_diagnostic *);
void vbus_stop(struct vbus_diagnostic *,unsigned);
#endif
