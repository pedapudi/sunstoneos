#ifndef SUNSTONE_DSI_INPUT_H
#define SUNSTONE_DSI_INPUT_H
#include "profile.h"
#include <stdatomic.h>
/* Same bounded copied upload convention as native codec inputs. Header96:
 * LE32 magic/version/op/slot; LE64 epoch/generation/authority/sequence/bytes;
 * hash32 and reserved8. op1 uploads a slot once; op2 selects an immutable slot.
 * Fixed bank geometry and both expected hashes come from the sealed session. */
#define DSI_INPUT_MAGIC UINT32_C(0x55495344)
struct dsi_input {
 uint8_t header[96];size_t header_used,received;
 unsigned slot,failed;uint64_t last_now,next_sequence,sequence;
 atomic_uint allocated,committed[2],request;
 uintptr_t bank;uint64_t bytes,deadline,epoch,generation,authority;
 unsigned requested_slot;
};
int dsi_input_feed(struct dsi_input *,const struct dsi_session *,
 int (*clock)(void *,uint64_t *),void *,const uint8_t *,size_t);
size_t dsi_input_remaining(const struct dsi_input *);
#endif
