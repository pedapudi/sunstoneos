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
/* Repeated modes 2, 3 and 5: bind the sole segment consumer before
 * scheduling. The Bluetooth owner alone changes raw transport ownership;
 * take/ack never call providers. capture.state binds the actual codec:
 * 1=CVSD PCM8k, 2=mSBC PCM16k.
 *
 * Modes 2 and 3 record one segment per voice-recognition activation, started
 * by the headset button (AT+BVRA=1) or by the owner on START. The request
 * word holds 0, ACCEPT (admit the next AT+BVRA=1), START (start now) or
 * ACTIVE. The consumer changes 0 to ACCEPT or START, or ACCEPT to START: 1
 * changed, 0 unchanged, -1 refused. When an activation ends the owner
 * publishes its segment if it holds a sample, stores 0 with release and
 * notifies the consumer (task notification index 0), also after a stop or
 * disconnection. Mode 5 records continuously and ignores the word.
 *
 * take borrows the oldest published segment: 0 none, 1 with a read-only
 * pointer to its PCM valid until the matching ack, -1 refused. The segment
 * holds an even number of bytes; first_sample counts samples in earlier
 * segments. */
int bluetooth_headset_wideband(const struct bluetooth_application *);
int bluetooth_headset_repeated(const struct bluetooth_application *);
int bluetooth_headset_segment_bind(struct bluetooth_application *,void *consumer);
#define BLUETOOTH_SEGMENT_ACCEPT 1u
#define BLUETOOTH_SEGMENT_START 2u
#define BLUETOOTH_SEGMENT_ACTIVE 3u
int bluetooth_headset_segment_request(struct bluetooth_application *,unsigned request);
int bluetooth_headset_segment_take(struct bluetooth_application *,const uint8_t **pcm,struct bluetooth_segment_snapshot *);
int bluetooth_headset_segment_ack(struct bluetooth_application *,uint64_t);
int bluetooth_headset_segment_status(struct bluetooth_application *,struct bluetooth_segment_status *);
int bluetooth_headset_segment_stop(struct bluetooth_application *);
#endif
