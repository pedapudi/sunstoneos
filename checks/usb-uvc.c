/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_usb_uvc.h"
#include "usb-uvc-c270.h"
#include "usb-uvc-capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned releases, controls;
static uint8_t saved[48];
static void release(void *c, void *p) {
  (void)c;
  (void)p;
  releases++;
}
static int control(void *c, const uint8_t *s, void *p, size_t n) {
  (void)c;
  controls++;
  if (s[1] == 11)
    return 0;
  if (s[1] == 0x81) {
    memcpy(p, saved, n);
    ((uint8_t *)p)[22] = 0;
    ((uint8_t *)p)[23] = 4;
  } else
    memcpy(saved, p, n);
  return (int)n;
}
int main(void) {
  struct sunstone_uvc_ops ops = {control, release, 0};
  struct sunstone_uvc s;
  assert(sunstone_uvc_probe_size(0x100) == 26 &&
         sunstone_uvc_probe_size(0x110) == 34 &&
         sunstone_uvc_probe_size(0x150) == 48);
  struct sunstone_uvc_catalog c = {
      .version = 0x150,
      .alts = 2,
      .alt = {{1, 1, 0x81, 1, 1, 512}, {1, 2, 0x81, 1, 1, 1024}}};
  struct sunstone_uvc_mode m = {
      SUNSTONE_UVC_MJPEG, 1, 1, 1, 640, 480, 333333, 4096};
  struct sunstone_uvc_alt alt;
  uint32_t maximum;
  assert(!sunstone_uvc_negotiate(&ops, &c, &m, &alt, &maximum) &&
         alt.alt == 2 && maximum == 4096 && controls == 4);
  uint8_t first[] = {2, 0, 0xff, 0xd8}, last[] = {2, 2, 0xff, 0xd9};
  sunstone_uvc_init(&s, &ops, 7, 4096);
  assert(!sunstone_uvc_payload(&s, 7, first, sizeof(first), first, 0));
  assert(!sunstone_uvc_payload(&s, 7, last, sizeof(last), last, 0));
  const struct sunstone_uvc_frame *f = sunstone_uvc_acquire(&s);
  assert(f && f->bytes == 4 && f->slices == 2 &&
         f->slice[0].data == first + 2 && !releases);
  sunstone_uvc_disconnect(&s);
  assert(!releases);
  assert(!sunstone_uvc_release(&s, f) && releases == 2);
  assert(sunstone_uvc_release(&s, f) < 0);
  assert(sunstone_uvc_payload(&s, 7, first, sizeof(first), first, 0) < 0 &&
         releases == 3);
  sunstone_uvc_init(&s, &ops, 8, 3);
  assert(!sunstone_uvc_payload(&s, 8, first, sizeof(first), first, 0));
  assert(sunstone_uvc_payload(&s, 8, last, sizeof(last), last, 0) < 0 &&
         releases == 5);
  uint8_t malformed[] = {1, 0};
  assert(sunstone_uvc_payload(&s, 8, malformed, 2, malformed, 0) < 0);
  uint8_t fresh[] = {2, 3, 42};
  assert(sunstone_uvc_payload(&s, 8, fresh, 3, fresh, 0) < 0);
  fresh[1] = 2;
  assert(!sunstone_uvc_payload(&s, 8, fresh, 3, fresh, 0));
  f = sunstone_uvc_acquire(&s);
  assert(f && f->bytes == 1 && f->slice[0].data[0] == 42);
  assert(!sunstone_uvc_release(&s, f));
  sunstone_uvc_init(&s, &ops, 9, 16);
  for (unsigned i = 0; i < 3; i++) {
    fresh[1] = (uint8_t)(2 | (i & 1));
    assert(!sunstone_uvc_payload(&s, 9, fresh, 3, fresh, 0));
  }
  fresh[1] = 3;
  assert(sunstone_uvc_payload(&s, 9, fresh, 3, fresh, 0) < 0);
  for (unsigned i = 0; i < 3; i++) {
    f = sunstone_uvc_acquire(&s);
    assert(f);
    assert(!sunstone_uvc_release(&s, f));
  }
  struct sunstone_uvc_negotiation negotiation;
  c.version = 0x110;
  c.alts = 1;
  c.alt[0] = (struct sunstone_uvc_alt){1, 2, 0x81, 1, 1, 1024};
  assert(sunstone_uvc_negotiate_begin(&negotiation, &c, &m) == 1 &&
         negotiation.length == 34);
  assert(sunstone_uvc_negotiate_complete(&negotiation, 0, 34) == 1 &&
         negotiation.setup[1] == 0x81);
  negotiation.data[22] = 0;
  negotiation.data[23] = 4;
  assert(sunstone_uvc_negotiate_complete(&negotiation, 0, 34) == 1 &&
         negotiation.setup[3] == 2);
  assert(sunstone_uvc_negotiate_complete(&negotiation, 0, 34) == 1 &&
         negotiation.setup[1] == 11);
  assert(!sunstone_uvc_negotiate_complete(&negotiation, 0, 0));
  /* Truncated descriptors and zero lengths must terminate with failure. */
  uint8_t bad[] = {0, 4};
  assert(sunstone_uvc_descriptors(&c, bad, sizeof(bad)) < 0);
  assert(!sunstone_uvc_descriptors(&c, c270_descriptors,
                                   sizeof(c270_descriptors)));
  assert(c.version == 0x100 && c.modes == 38 && c.alts == 11);
  assert(c.mode[0].format == SUNSTONE_UVC_YUYV && c.mode[0].width == 640 &&
         c.mode[0].height == 480 && c.mode[0].max_frame == 614400);
  assert(c.mode[19].format == SUNSTONE_UVC_MJPEG && c.mode[19].width == 640 &&
         c.mode[19].height == 480);
  assert(c.alt[10].capacity == 3060 && c.alt[10].alt == 11);
  assert(!sunstone_uvc_negotiate(&ops, &c, &c.mode[19], &alt, &maximum));
  assert(alt.alt == 7 && alt.capacity == 1280);
  for (unsigned i = 0; i < sizeof(captured_probes) / sizeof(captured_probes[0]);
       i++) {
    const unsigned format = captured_probes[i].format,
                   frame = captured_probes[i].frame;
    struct sunstone_uvc_mode mode = c.mode[(format - 1) * 19 + frame - 1];
    mode.interval = captured_probes[i].interval;
    assert(sunstone_uvc_negotiate_begin(&negotiation, &c, &mode) == 1);
    assert(sunstone_uvc_negotiate_complete(&negotiation, 0, 26) == 1);
    negotiation.data[12] = (uint8_t)captured_probes[i].quality;
    for (unsigned b = 0; b < 4; b++) {
      negotiation.data[18 + b] =
          (uint8_t)(captured_probes[i].maximum >> (8 * b));
      negotiation.data[22 + b] =
          (uint8_t)(captured_probes[i].payload >> (8 * b));
    }
    assert(sunstone_uvc_negotiate_complete(&negotiation, 0, 26) == 1);
    assert(negotiation.alt.alt == 11 && negotiation.alt.capacity == 3060);
    assert(sunstone_uvc_negotiate_complete(&negotiation, 0, 26) == 1);
    assert(!sunstone_uvc_negotiate_complete(&negotiation, 0, 0));
  }
  sunstone_uvc_init(&s, &ops, 12, 10000);
  uint8_t bulk_start[] = {2, 2, 10, 20}, bulk_end[] = {30, 40};
  assert(!sunstone_uvc_payload_fragment(&s, 12, bulk_start, 4, bulk_start, 0, 1,
                                        0));
  assert(!sunstone_uvc_acquire(&s));
  assert(
      !sunstone_uvc_payload_fragment(&s, 12, bulk_end, 2, bulk_end, 0, 0, 1));
  f = sunstone_uvc_acquire(&s);
  assert(f && f->bytes == 4 && f->slices == 2 && f->slice[1].data[1] == 40);
  assert(!sunstone_uvc_release_ticket(&s, f, f->generation, f->sequence));
  sunstone_uvc_init(&s, &ops, 13, 100);
  uint8_t error_start[] = {2, 1, 42}, error_end[] = {2, 3, 43},
          recovered[] = {2, 2, 44};
  assert(sunstone_uvc_payload(&s, 13, error_start, 3, error_start, -1) < 0);
  assert(sunstone_uvc_payload(&s, 13, error_end, 3, error_end, 0) < 0);
  assert(!sunstone_uvc_acquire(&s));
  assert(!sunstone_uvc_payload(&s, 13, recovered, 3, recovered, 0));
  f = sunstone_uvc_acquire(&s);
  assert(f && f->bytes == 1 && f->slice[0].data[0] == 44);
  assert(!sunstone_uvc_release_ticket(&s, f, f->generation, f->sequence));
  puts("UVC: probe layouts, negotiated bandwidth, zero-copy bytes, detach "
       "leases, overflow, malformed payload and backpressure passed");
  return 0;
}
