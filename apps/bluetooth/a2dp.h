/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BLUETOOTH_A2DP_H
#define SUNSTONE_BLUETOOTH_A2DP_H
#include "bt_avdtp.h"
#include "headset.h"
/* One serialized owner, one finite capture. Init once before scheduling.
 * A step decodes at most one packet (15 frames), directly into pcm. The
 * target counts bytes of interleaved PCM16LE, divisible by four. On any
 * error the caller retains the decoder, transport and unpublished bytes. */
int bluetooth_a2dp_init(void);
int bluetooth_a2dp_decode(struct bt_avdtp *,uint8_t pcm[BLUETOOTH_PCM_BYTES],unsigned target,
    struct bluetooth_pcm_snapshot *,struct bluetooth_pcm_record *,uint64_t acquired);
unsigned bluetooth_a2dp_rate(void);
unsigned bluetooth_a2dp_channels(void);
/* Allocation hooks apply only to the exclusive A2DP libsbc object. */
void *bluetooth_a2dp_alloc(size_t);
void bluetooth_a2dp_free(void *);
#endif
