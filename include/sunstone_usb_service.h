#ifndef SUNSTONE_USB_SERVICE_H
#define SUNSTONE_USB_SERVICE_H
#include "sunstone_app.h"
/* Single OS owner and single application consumer. Optional enter/leave admit
 * direct caller writes under the controller execution gate. Other callbacks
 * execute from os_usb_service_dispatch(). */
struct os_usb_service_ops {
    void (*wake)(void *context);
    void (*release)(void *context, unsigned device, uint64_t token);
    int (*write)(void *context, uint64_t sequence, const void *data, size_t bytes);
    int (*configure)(void *context, const struct ss_usb_serial_line *line);
    int (*configured)(void *context);
    void (*changed)(void *context);
    int (*enter)(void *context);
    void (*leave)(void *context);
};
void os_usb_service_init(const struct os_usb_service_ops *ops, void *context);
void os_usb_service_connected(unsigned devices);
void os_usb_service_disconnected(unsigned devices);
void os_usb_service_dispatch(void);
/* Application exit refuses new acquisitions and releases held leases after
 * pending transmit completion. The owner still owns controller drain. */
void os_usb_service_retire(void);
int os_usb_service_idle(void);
int os_usb_service_input(const struct ss_input_event *event);
int os_usb_service_serial(const struct ss_usb_serial_buffer *buffer);
int os_usb_service_camera_pin(uint64_t token,struct ss_usb_camera_frame *frame);
void os_usb_service_camera_unpin(uint64_t token);
int os_usb_service_frame(const struct ss_usb_camera_frame *frame);
void os_usb_service_write_complete(uint64_t sequence, int status, size_t actual);
#endif
