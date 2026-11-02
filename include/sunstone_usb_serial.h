/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_USB_SERIAL_H
#define SUNSTONE_USB_SERIAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "a733_xhci.h"
/* Each serial port is a function of one attached device, so the ports follow
 * the host's device records (A733_XHCI_DEVICES). A receive buffer is one
 * retained controller page, A733_XHCI_TRANSFER_BYTES; a single bulk TRB could
 * carry up to 64 KiB - 1 (xHCI 1.2 section 6.4.1.1 gives 17 bits), so the page
 * is the bound. Slots are the receive buffers a port rotates among: one owned
 * by the controller, one holding data, and the rest leased to the application.
 * Build with -DSS_USB_SERIAL_SLOTS=N for another depth. */
#ifndef SS_USB_SERIAL_PORTS
#define SS_USB_SERIAL_PORTS A733_XHCI_DEVICES
#endif
#ifndef SS_USB_SERIAL_SLOTS
#define SS_USB_SERIAL_SLOTS 4
#endif
#ifndef SS_USB_SERIAL_BUFFER
#define SS_USB_SERIAL_BUFFER A733_XHCI_TRANSFER_BYTES
#endif
_Static_assert(SS_USB_SERIAL_PORTS>=1&&SS_USB_SERIAL_PORTS<=A733_XHCI_DEVICES&&SS_USB_SERIAL_SLOTS>=2&&
    SS_USB_SERIAL_BUFFER>=1024&&SS_USB_SERIAL_BUFFER<=A733_XHCI_TRANSFER_BYTES,"serial geometry");
/* All calls execute in the USB owner task. A service marshals application calls.
 * A lease remains valid through disconnect until its explicit release. */
struct ss_usb_serial_view { const uint8_t *data; size_t size; uint64_t token; };
struct ss_usb_serial_stats {
 uint64_t received, released, backpressure, dropped, errors, stale;
};
/* Supply SS_USB_SERIAL_SLOTS disjoint, SS_USB_SERIAL_BUFFER-byte DMA mappings retained until every lease and
 * controller transfer is released. The adapter validates NC/device translation. */
struct ss_usb_serial_buffer_ops {
 int (*acquire)(void *, unsigned, uint8_t **, size_t *, uint64_t *);
 void (*release)(void *, unsigned, uint64_t);
};
bool ss_usb_serial_backend(unsigned port,const struct ss_usb_serial_buffer_ops *,void *context);
bool ss_usb_serial_bind(unsigned port, uint8_t *buffers[SS_USB_SERIAL_SLOTS]);
bool ss_usb_serial_attach(unsigned port, uint64_t generation, uint16_t packet, bool ftdi);
void ss_usb_serial_detach(unsigned port);
/* Call only after the controller proves all old endpoint DMA has stopped. */
void ss_usb_serial_quiesced(unsigned port);
uint8_t *ss_usb_serial_rx_buffer(unsigned port, uint64_t *cookie, size_t *capacity);
bool ss_usb_serial_rx_complete(unsigned port, uint64_t cookie, size_t bytes, bool success);
bool ss_usb_serial_peek(unsigned port, struct ss_usb_serial_view *view);
bool ss_usb_serial_rx_release(unsigned port, uint64_t token);
bool ss_usb_serial_stats(unsigned port, struct ss_usb_serial_stats *stats);
/* Patched TinyUSB class calls these; service may install notification hooks. */
void ss_usb_serial_ready(unsigned port);
void ss_usb_serial_changed(unsigned port, bool attached);
struct sunstone_usb_transport;
struct ss_usb_serial_line;
int os_usb_serial_native_start(struct sunstone_usb_transport *);
void os_usb_serial_native_release(uint64_t token);
int os_usb_serial_native_write(uint64_t sequence,const void *data,size_t bytes);
int os_usb_serial_native_configure(const struct ss_usb_serial_line *);
int os_usb_serial_native_configured(void);
void os_usb_serial_native_quiesced(unsigned port);
void os_usb_serial_native_quiesce(uint8_t address);
/* Drain old controller events before clearing retired slots; outstanding
 * application leases continue to block attachment index reuse. */
#endif
