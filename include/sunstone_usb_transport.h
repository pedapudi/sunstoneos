/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_USB_TRANSPORT_H
#define SUNSTONE_USB_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
/* Operations return 1 when accepted and 0 on refusal. Completion result is
 * 0 for success, 1 for endpoint stall, and 2 for failure; actual is the
 * completed byte count. A refused submission never produces a completion.
 * acquire returns an identity-mapped, coherent Normal NC controller buffer,
 * with capacity and an immutable token bound to that buffer and attachment.
 * A buffer stays acquired through completion and application use; release
 * refuses an on-ring buffer. Stale tokens cannot release a reused address.
 * One transfer per endpoint may be outstanding; held completed buffers do
 * not prevent a new acquire while pool capacity remains.
 * All operations and completions run in the USB owner task. A successful
 * submission borrows its buffer until completion. A timeout or disconnect
 * does not authorize reuse of memory still owned by the controller. */
struct sunstone_usb_transport;
typedef void (*sunstone_usb_complete)(void *context,unsigned result,size_t actual);
struct sunstone_usb_endpoint {
    uint8_t address,attributes,interval;
    uint16_t packet;
};
struct sunstone_usb_transport_ops {
    int (*control)(struct sunstone_usb_transport *,uint8_t address,const uint8_t setup[8],
        uint8_t *buffer,sunstone_usb_complete,void *);
    int (*open)(struct sunstone_usb_transport *,uint8_t address,const struct sunstone_usb_endpoint *);
    int (*submit)(struct sunstone_usb_transport *,uint8_t address,uint8_t endpoint,
        uint8_t *buffer,size_t bytes,sunstone_usb_complete,void *);
    int (*acquire)(struct sunstone_usb_transport *,uint8_t address,uint8_t endpoint,
        uint8_t **buffer,size_t *capacity,uint64_t *token);
    int (*release)(struct sunstone_usb_transport *,uint8_t address,uint8_t endpoint,uint64_t token);
};
struct sunstone_usb_transport {
    const struct sunstone_usb_transport_ops *ops;
    void *context;
    uint64_t generation;
};
#endif
