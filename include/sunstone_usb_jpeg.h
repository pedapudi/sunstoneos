#ifndef SUNSTONE_USB_JPEG_H
#define SUNSTONE_USB_JPEG_H
#include <stddef.h>
#include <stdint.h>

/* Privileged OS consumer. Only the camera owner supplies these slices while
 * retaining its frame lease; application addresses never enter this interface. */
struct ss_usb_jpeg_slice { const uint8_t *data; size_t bytes; };
struct ss_usb_jpeg_frame {
    uint64_t token;
    const struct ss_usb_jpeg_slice *slices;
    size_t slice_count,bytes;
    uint32_t width,height;
};
struct ss_usb_jpeg_view {
    const uint8_t *data;
    size_t bytes;
    uint32_t width,height; /* packed active Y, Cb, Cr rows (YU12) */
    uint64_t frame_token;
};
struct a733_jpeg_session;
/* session is the authenticated OS-owned JPEG extent, with stamped live board
 * prefix. No caller may pass application-supplied session bytes. */
/* One serialized OS owner, one decode per image lifetime. begin: 1 accepted,
 * 0 refused before allocation, -1 retained fault. poll: 0 pending, 1 verified
 * private YU12 copy, -1 retained fault. Every allocation and hardware lease
 * persists until whole-board power removal, including after COPIED. */
int ss_usb_jpeg_native_begin(const struct a733_jpeg_session *,const struct ss_usb_jpeg_frame *);
int ss_usb_jpeg_native_poll(struct ss_usb_jpeg_view *);
#endif
