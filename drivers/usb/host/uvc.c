/* SPDX-License-Identifier: GPL-2.0-only
 * UVC protocol adaptation: Linux v6.12 drivers/media/usb/uvc/uvc_video.c.
 * Copyright (C) 2005-2010 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */
#include "sunstone_usb_uvc.h"
#include <string.h>
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) {
  return le16(p) | (uint32_t)le16(p + 2) << 16;
}
static void put32(uint8_t *p, uint32_t x) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(x >> (i * 8));
}
size_t sunstone_uvc_probe_size(uint16_t v) {
  return v < 0x110 ? 26 : v < 0x150 ? 34 : 48;
}
int sunstone_uvc_descriptors(struct sunstone_uvc_catalog *c, const void *data,
                             size_t n) {
  const uint8_t *p = data;
  uint8_t iface = 0, alt = 0, sub = 0, fmt = 0, idx = 0;
  memset(c, 0, sizeof(*c));
  if (n >= 2 && p[1] == 2 && (n < 9 || p[0] < 9 || le16(p + 2) != n))
    return -1;
  while (n) {
    if (n < 2 || p[0] < 2 || p[0] > n)
      return -1;
    unsigned l = p[0];
    if (p[1] == 4) {
      if (l < 9)
        return -1;
      iface = p[2];
      alt = p[3];
      sub = p[5] == 14 ? p[6] : 0;
      fmt = 0;
    }
    if (p[1] == 0x24 && sub == 1 && l >= 5 && p[2] == 1)
      c->version = le16(p + 3);
    if (p[1] == 0x24 && sub == 2 && l >= 4) {
      if (p[2] == 6) {
        if (l < 11)
          return -1;
        fmt = SUNSTONE_UVC_MJPEG;
        idx = p[3];
      }
      if (p[2] == 4) {
        if (l < 27)
          return -1;
        fmt = memcmp(p + 5, "YUY2\0\0\x10\0\x80\0\0\xaa\0\x38\x9b\x71", 16)
                  ? 0
                  : SUNSTONE_UVC_YUYV;
        idx = p[3];
      }
      if (((p[2] == 5 && fmt == SUNSTONE_UVC_YUYV) ||
           (p[2] == 7 && fmt == SUNSTONE_UVC_MJPEG))) {
        if (l < 26)
          return -1;
        if (c->modes == SUNSTONE_UVC_MODES)
          return -2;
        unsigned count = p[25];
        if (count ? (l < 26 + 4 * count) : (l < 38))
          return -1;
        unsigned at = c->modes++;
        struct sunstone_uvc_mode *m = &c->mode[at];
        *m = (struct sunstone_uvc_mode){fmt,          idx,         p[3],
                                        iface,        le16(p + 5), le16(p + 7),
                                        le32(p + 21), le32(p + 17)};
        if (!m->width || !m->height || !m->interval || !m->max_frame)
          return -1;
        if (count) {
          int found = 0;
          for (unsigned i = 0; i < count; i++) {
            uint32_t interval = le32(p + 26 + 4 * i);
            if (!interval)
              return -1;
            c->intervals[at].discrete[i] = interval;
            if (interval == m->interval)
              found = 1;
          }
          c->intervals[at].count = count;
          if (!found)
            return -1;
        } else {
          uint32_t lo = le32(p + 26), hi = le32(p + 30), step = le32(p + 34);
          if (!lo || !step || m->interval < lo || m->interval > hi ||
              (m->interval - lo) % step)
            return -1;
          c->intervals[at].minimum = lo;
          c->intervals[at].maximum = hi;
          c->intervals[at].step = step;
        }
      }
    }
    if (p[1] == 5 && sub == 2) {
      if (l < 7)
        return -1;
      unsigned type = p[3] & 3;
      if ((p[2] & 128) && !(p[2] & 0x70) && (p[2] & 15) &&
          (type == 1 || type == 2)) {
        if (c->alts == SUNSTONE_UVC_ALTS)
          return -2;
        uint16_t w = le16(p + 4);
        unsigned mult = (w >> 11) & 3;
        if (mult == 3 || !(w & 2047) || (w & 0xe000))
          return -1;
        c->alt[c->alts++] = (struct sunstone_uvc_alt){
            iface, alt, p[2], type, p[6], (w & 2047) * (1 + mult)};
      }
    }
    p += l;
    n -= l;
  }
  return c->version && c->modes && c->alts ? 0 : -1;
}
static int query(const struct sunstone_uvc_ops *o, uint8_t iface,
                 uint8_t request, uint8_t selector, void *p, size_t n) {
  uint8_t setup[8] = {(request & 128) ? 0xa1 : 0x21,
                      request,
                      0,
                      selector,
                      iface,
                      0,
                      (uint8_t)n,
                      (uint8_t)(n >> 8)};
  return o->control(o->context, setup, p, n) == (int)n ? 0 : -1;
}
int sunstone_uvc_negotiate(const struct sunstone_uvc_ops *o,
                           const struct sunstone_uvc_catalog *c,
                           const struct sunstone_uvc_mode *m,
                           struct sunstone_uvc_alt *a, uint32_t *maximum) {
  uint8_t p[48] = {0};
  size_t n = sunstone_uvc_probe_size(c->version);
  p[0] = 1;
  p[2] = m->index;
  p[3] = m->frame;
  put32(p + 4, m->interval);
  put32(p + 18, m->max_frame);
  if (query(o, m->interface, 1, 1, p, n) ||
      query(o, m->interface, 0x81, 1, p, n))
    return -1;
  uint32_t bytes = le32(p + 18), payload = le32(p + 22);
  if (p[2] != m->index || p[3] != m->frame || le32(p + 4) != m->interval ||
      !bytes || bytes > m->max_frame || !payload)
    return -2;
  int found = 0;
  for (unsigned i = 0; i < c->alts; i++) {
    const struct sunstone_uvc_alt *x = &c->alt[i];
    if (x->interface != m->interface || !x->capacity)
      continue;
    if (x->type == 1 && x->capacity < payload)
      continue;
    if (!found || x->capacity < a->capacity) {
      *a = *x;
      found = 1;
    }
  }
  if (!found)
    return -3;
  if (query(o, m->interface, 1, 2, p, n))
    return -1;
  uint8_t setup[8] = {1, 11, a->alt, 0, m->interface, 0, 0, 0};
  if (o->control(o->context, setup, 0, 0) != 0)
    return -1;
  *maximum = bytes;
  return 0;
}
static void clear(struct sunstone_uvc *s, struct sunstone_uvc_frame *f) {
  for (unsigned i = 0; i < f->slices; i++)
    s->ops.release(s->ops.context, f->slice[i].lease);
  memset(f, 0, sizeof(*f));
}
void sunstone_uvc_init(struct sunstone_uvc *s, const struct sunstone_uvc_ops *o,
                       uint64_t generation, uint32_t maximum) {
  memset(s, 0, sizeof(*s));
  s->ops = *o;
  s->generation = generation;
  s->max_frame = maximum;
  s->fid = -1;
  s->connected = 1;
}
static int next(struct sunstone_uvc *s) {
  for (unsigned i = 0; i < SUNSTONE_UVC_FRAMES; i++)
    if (!s->frame[i].state && !s->frame[i].slices) {
      s->current = i;
      return 0;
    }
  return -1;
}
static int payload_body(struct sunstone_uvc *s, uint64_t generation,
                        const uint8_t *data, size_t bytes, void *lease,
                        uint8_t flags) {
  struct sunstone_uvc_frame *f = &s->frame[s->current];
  int fid = flags & 1;
  if (s->lost_sync) {
    s->fid = fid;
    s->dropping = 1;
    s->lost_sync = 0;
  }
  if (s->fid != fid) {
    if (!f->state && f->slices) {
      if (!s->dropping &&
          (!s->expected_frame || f->bytes == s->expected_frame)) {
        f->sequence = ++s->sequence;
        f->generation = generation;
        f->state = 1;
      } else
        clear(s, f);
    }
    s->fid = fid;
    s->dropping = 0;
    if (next(s))
      s->dropping = 1;
    f = &s->frame[s->current];
  }
  if ((flags & 0x40) || s->dropping || f->state) {
    if (!f->state)
      clear(s, f);
    s->dropping = 1;
    s->ops.release(s->ops.context, lease);
    return -3;
  }
  if (bytes > s->max_frame - f->bytes || f->slices == SUNSTONE_UVC_SLICES) {
    clear(s, f);
    s->dropping = 1;
    s->ops.release(s->ops.context, lease);
    return -4;
  }
  if (bytes) {
    f->slice[f->slices++] = (struct sunstone_camera_span){data, bytes, lease};
    f->bytes += bytes;
  } else
    s->ops.release(s->ops.context, lease);
  if (flags & 2) {
    if (f->bytes && (!s->expected_frame || f->bytes == s->expected_frame)) {
      f->sequence = ++s->sequence;
      f->generation = generation;
      f->state = 1;
    }
    s->dropping = 1;
  }
  return 0;
}
static int payload_validated(struct sunstone_uvc *s, uint64_t generation,
                             const void *data, size_t n, void *lease,
                             int status) {
  const uint8_t *p = data;
  struct sunstone_uvc_frame *f = &s->frame[s->current];
  if (!s->connected || generation != s->generation) {
    s->ops.release(s->ops.context, lease);
    return -1;
  }
  int valid = n >= 2 && p[0] >= 2 && p[0] <= n && (!(p[1] & 4) || p[0] >= 6) &&
              (!(p[1] & 8) || p[0] >= ((p[1] & 4) ? 12 : 8));
  if (status || !valid) {
    if (!f->state)
      clear(s, f);
    s->dropping = 1;
    if (valid)
      s->fid = p[1] & 1;
    else
      s->lost_sync = 1;
    s->ops.release(s->ops.context, lease);
    return -2;
  }
  return payload_body(s, generation, p + p[0], n - p[0], lease, p[1]);
}
int sunstone_uvc_payload(struct sunstone_uvc *s, uint64_t generation,
                         const void *data, size_t n, void *lease, int status) {
  return payload_validated(s, generation, data, n, lease, status);
}
int sunstone_uvc_payload_fragment(struct sunstone_uvc *s, uint64_t generation,
                                  const void *data, size_t n, void *lease,
                                  int status, int start, int end) {
  const uint8_t *p = data;
  if (start) {
    if (status || n < 2 || p[0] < 2 || p[0] > n || ((p[1] & 4) && p[0] < 6) ||
        ((p[1] & 8) && p[0] < ((p[1] & 4) ? 12 : 8))) {
      s->bulk_active = 0;
      return payload_validated(s, generation, data, n, lease, -1);
    }
    s->bulk_flags = p[1];
    s->bulk_active = 1;
    data = p + p[0];
    n -= p[0];
  } else if (!s->bulk_active) {
    return payload_validated(s, generation, data, n, lease, -1);
  }
  if (!s->connected || generation != s->generation || status) {
    s->bulk_active = 0;
    uint8_t failed[2] = {2, s->bulk_flags};
    return payload_validated(s, generation, failed, 2, lease, -1);
  }
  uint8_t flags = s->bulk_flags;
  if (!end)
    flags &= (uint8_t)~2;
  else
    s->bulk_active = 0;
  return payload_body(s, generation, data, n, lease, flags);
}
const struct sunstone_uvc_frame *sunstone_uvc_acquire(struct sunstone_uvc *s) {
  struct sunstone_uvc_frame *best = 0;
  for (unsigned i = 0; i < SUNSTONE_UVC_FRAMES; i++)
    if (s->frame[i].state == 1 &&
        (!best || s->frame[i].sequence < best->sequence))
      best = &s->frame[i];
  if (best)
    best->state = 2;
  return best;
}
int sunstone_uvc_release(struct sunstone_uvc *s,
                         const struct sunstone_uvc_frame *frame) {
  for (unsigned i = 0; i < SUNSTONE_UVC_FRAMES; i++)
    if (frame == &s->frame[i] && frame->state == 2) {
      clear(s, &s->frame[i]);
      return 0;
    }
  return -1;
}
void sunstone_uvc_disconnect(struct sunstone_uvc *s) {
  s->connected = 0;
  for (unsigned i = 0; i < SUNSTONE_UVC_FRAMES; i++)
    if (s->frame[i].state != 2)
      clear(s, &s->frame[i]);
}

int sunstone_uvc_release_ticket(struct sunstone_uvc *s,
                                const struct sunstone_uvc_frame *f,
                                uint64_t generation, uint64_t sequence) {
  for (unsigned i = 0; i < SUNSTONE_UVC_FRAMES; i++)
    if (f == &s->frame[i] && f->generation == generation &&
        f->sequence == sequence)
      return sunstone_uvc_release(s, f);
  return -1;
}

static void async_setup(struct sunstone_uvc_negotiation *n, uint8_t request,
                        uint8_t selector) {
  uint8_t setup[8] = {(request & 128) ? 0xa1 : 0x21,
                      request,
                      0,
                      selector,
                      n->mode.interface,
                      0,
                      (uint8_t)n->length,
                      (uint8_t)(n->length >> 8)};
  memcpy(n->setup, setup, 8);
}
int sunstone_uvc_negotiate_begin(struct sunstone_uvc_negotiation *n,
                                 const struct sunstone_uvc_catalog *c,
                                 const struct sunstone_uvc_mode *m) {
  memset(n, 0, sizeof(*n));
  n->catalog = c;
  n->mode = *m;
  n->length = sunstone_uvc_probe_size(c->version);
  n->data[0] = 1;
  n->data[2] = m->index;
  n->data[3] = m->frame;
  put32(n->data + 4, m->interval);
  put32(n->data + 18, m->max_frame);
  n->phase = 1;
  async_setup(n, 1, 1);
  return 1;
}
int sunstone_uvc_negotiate_complete(struct sunstone_uvc_negotiation *n,
                                    int status, size_t actual) {
  if (!n->phase || status || actual != n->length) {
    n->phase = 0;
    return -1;
  }
  if (n->phase == 1) {
    n->phase = 2;
    async_setup(n, 0x81, 1);
    return 1;
  }
  if (n->phase == 2) {
    uint32_t payload = le32(n->data + 22);
    n->payload = payload;
    n->maximum = le32(n->data + 18);
    if (n->data[2] != n->mode.index || n->data[3] != n->mode.frame ||
        le32(n->data + 4) != n->mode.interval || !payload || !n->maximum ||
        n->maximum > n->mode.max_frame) {
      n->phase = 0;
      return -2;
    }
    int found = 0;
    for (unsigned i = 0; i < n->catalog->alts; i++) {
      const struct sunstone_uvc_alt *a = &n->catalog->alt[i];
      if (a->interface != n->mode.interface || !a->capacity ||
          (a->type == 1 && a->capacity < payload))
        continue;
      if (!found || a->capacity < n->alt.capacity) {
        n->alt = *a;
        found = 1;
      }
    }
    if (!found) {
      n->phase = 0;
      return -3;
    }
    n->phase = 3;
    async_setup(n, 1, 2);
    return 1;
  }
  if (n->phase == 3) {
    uint8_t setup[8] = {1, 11, n->alt.alt, 0, n->mode.interface, 0, 0, 0};
    memcpy(n->setup, setup, 8);
    n->length = 0;
    n->phase = 4;
    return 1;
  }
  n->phase = 0;
  return 0;
}

void sunstone_uvc_expect_mode(struct sunstone_uvc *s,
                              const struct sunstone_uvc_mode *m) {
  s->expected_frame =
      m->format == SUNSTONE_UVC_YUYV ? (uint32_t)m->width * m->height * 2 : 0;
}
