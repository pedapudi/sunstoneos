/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_USB_UVC_H
#define SUNSTONE_USB_UVC_H
#include <stddef.h>
#include <stdint.h>
#define SUNSTONE_UVC_SLICES 1024
#define SUNSTONE_UVC_FRAMES 3
#define SUNSTONE_UVC_MODES 64
#define SUNSTONE_UVC_ALTS 16
enum sunstone_uvc_format { SUNSTONE_UVC_YUYV = 1, SUNSTONE_UVC_MJPEG = 2 };
struct sunstone_uvc_mode {
  uint8_t format, index, frame, interface;
  uint16_t width, height;
  uint32_t interval, max_frame;
};
struct sunstone_uvc_alt {
  uint8_t interface, alt, endpoint, type, interval;
  uint32_t capacity;
};
struct sunstone_uvc_catalog {
  uint16_t version;
  unsigned modes, alts;
  struct sunstone_uvc_mode mode[SUNSTONE_UVC_MODES];
  struct sunstone_uvc_alt alt[SUNSTONE_UVC_ALTS];
};
struct sunstone_uvc_slice {
  const uint8_t *data;
  size_t bytes;
  void *lease;
};
struct sunstone_uvc_frame {
  struct sunstone_uvc_slice slice[SUNSTONE_UVC_SLICES];
  size_t bytes;
  unsigned slices;
  uint64_t sequence, generation;
  uint8_t state;
};
/* All calls run on the USB owner task. Control returns actual length or <0.
 * release returns a transfer buffer to its transport pool, once per lease. */
struct sunstone_uvc_ops {
  int (*control)(void *, const uint8_t setup[8], void *, size_t);
  void (*release)(void *, void *);
  void *context;
};
struct sunstone_uvc {
  struct sunstone_uvc_ops ops;
  struct sunstone_uvc_frame frame[SUNSTONE_UVC_FRAMES];
  uint64_t generation, sequence;
  uint32_t max_frame, expected_frame;
  unsigned current;
  int fid;
  uint8_t connected, dropping, bulk_flags, bulk_active, lost_sync;
};
int sunstone_uvc_descriptors(struct sunstone_uvc_catalog *, const void *,
                             size_t);
size_t sunstone_uvc_probe_size(uint16_t version);
int sunstone_uvc_negotiate(const struct sunstone_uvc_ops *,
                           const struct sunstone_uvc_catalog *,
                           const struct sunstone_uvc_mode *,
                           struct sunstone_uvc_alt *, uint32_t *);
void sunstone_uvc_init(struct sunstone_uvc *, const struct sunstone_uvc_ops *,
                       uint64_t, uint32_t);
/* One complete UVC payload, including its header, per call. Bulk transport
 * reassembles transfer fragments through its short-packet/payload-size
 * boundary. Ownership of lease transfers on every call, including rejection. */
int sunstone_uvc_payload(struct sunstone_uvc *, uint64_t, const void *, size_t,
                         void *, int);
const struct sunstone_uvc_frame *sunstone_uvc_acquire(struct sunstone_uvc *);
int sunstone_uvc_release(struct sunstone_uvc *,
                         const struct sunstone_uvc_frame *);
int sunstone_uvc_release_ticket(struct sunstone_uvc *,
                                const struct sunstone_uvc_frame *, uint64_t,
                                uint64_t);
void sunstone_uvc_disconnect(struct sunstone_uvc *);
int sunstone_uvc_payload_fragment(struct sunstone_uvc *, uint64_t, const void *,
                                  size_t, void *, int, int, int);
void sunstone_uvc_expect_mode(struct sunstone_uvc *,
                              const struct sunstone_uvc_mode *);
/* Async owner-task negotiation: submit setup/data after begin and each
 * return 1. complete returns 0 after SET_INTERFACE, or a negative bounded
 * failure. */
struct sunstone_uvc_negotiation {
  const struct sunstone_uvc_catalog *catalog;
  struct sunstone_uvc_mode mode;
  struct sunstone_uvc_alt alt;
  uint8_t data[48], setup[8], phase;
  size_t length;
  uint32_t maximum, payload;
};
int sunstone_uvc_negotiate_begin(struct sunstone_uvc_negotiation *,
                                 const struct sunstone_uvc_catalog *,
                                 const struct sunstone_uvc_mode *);
int sunstone_uvc_negotiate_complete(struct sunstone_uvc_negotiation *, int,
                                    size_t);
#endif
