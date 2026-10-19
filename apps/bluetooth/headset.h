#ifndef SUNSTONE_BLUETOOTH_HEADSET_APPLICATION_H
#define SUNSTONE_BLUETOOTH_HEADSET_APPLICATION_H
#include <stddef.h>
#include <stdint.h>
struct bluetooth_application;
#define BLUETOOTH_PCM_BYTES 48000u
#define BLUETOOTH_PCM_RECORDS 1024u
struct bluetooth_pcm_record {uint64_t acquired;uint32_t offset;uint16_t source_bytes,copied_bytes;uint32_t status,reserved;};
_Static_assert(sizeof(struct bluetooth_pcm_record)==24,"Copied capture record geometry");
struct bluetooth_pcm_snapshot {
    uint64_t firmware,uart,acl,audio;
    uint32_t state,bytes,records,loss_packets;
    uint8_t sha256[32];
};
int bluetooth_headset_poll(struct bluetooth_application *,uint64_t now);
int bluetooth_headset_retain(struct bluetooth_application *);
/* Cross-task copied results only. Pending has no mutable producer fields.
 * kind0=PCM byte offset,kind1=raw record byte offset; bytes<=1024.
 * Complete follows full capture and actual synchronous disconnect. */
int bluetooth_headset_copy(struct bluetooth_application *,unsigned kind,uint32_t offset,size_t bytes,
                          uint8_t out[1024],size_t *copied,struct bluetooth_pcm_snapshot *);
#define BLUETOOTH_SEGMENT_SLOTS 2u
#define BLUETOOTH_SEGMENT_CANCEL 1u
#define BLUETOOTH_SEGMENT_WINDOW 2u
#define BLUETOOTH_SEGMENT_BACKLOG 4u
#define BLUETOOTH_SEGMENT_LOSS 8u
#define BLUETOOTH_SEGMENT_DEADLINE 16u
struct bluetooth_segment_snapshot {
    struct bluetooth_pcm_snapshot capture;
    uint64_t ticket,first_sample,first_acquired,last_acquired;
};
struct bluetooth_segment_status {uint64_t published,consumed;uint32_t flags,disconnected;};
/* Mode2: bind the sole copied-input consumer before scheduling. The Bluetooth
 * owner alone changes raw transport ownership; copy/ACK never call providers. */
int bluetooth_headset_repeated(const struct bluetooth_application *);
int bluetooth_headset_segment_bind(struct bluetooth_application *,void *consumer);
int bluetooth_headset_segment_copy(struct bluetooth_application *,uint64_t ticket,uint32_t offset,size_t,
    uint8_t out[1024],size_t *,struct bluetooth_segment_snapshot *);
int bluetooth_headset_segment_ack(struct bluetooth_application *,uint64_t);
int bluetooth_headset_segment_status(struct bluetooth_application *,struct bluetooth_segment_status *);
int bluetooth_headset_segment_stop(struct bluetooth_application *);
#endif
