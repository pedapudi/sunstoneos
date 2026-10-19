#ifndef SUNSTONE_BLUETOOTH_PLAYBACK_H
#define SUNSTONE_BLUETOOTH_PLAYBACK_H
#include <stdint.h>
#include <stddef.h>
#define BLUETOOTH_PLAYBACK_BYTES 2400u
struct bluetooth_application;
struct bt_sco;
struct bluetooth_playback_result {
    uint64_t firmware,uart,acl,audio,ticket;
    uint64_t encoded_samples,submitted_bytes,uart_packets,released_packets,flushed_packets,not_sent_packets;
    uint8_t sha256[32];
    uint32_t flags,codec;
};
struct bluetooth_playback_status {
    uint64_t accepted,acknowledged,silence_frames,discarded_packets,discarded_bytes,loss_packets;
    uint64_t silence_uart,silence_released,silence_flushed,silence_not_sent;
    uint32_t sealed,cancelled,disconnected,free_slots;
};
int bluetooth_playback_mode(const struct bluetooth_application *);
int bluetooth_playback_write(struct bluetooth_application *,uint64_t,uint32_t,const uint8_t *,size_t);
int bluetooth_playback_commit(struct bluetooth_application *,uint64_t,const uint8_t[32]);
int bluetooth_playback_copy(struct bluetooth_application *,struct bluetooth_playback_result *,struct bluetooth_playback_status *);
int bluetooth_playback_ack(struct bluetooth_application *,uint64_t);
int bluetooth_playback_end(struct bluetooth_application *,int cancel);
/* Bluetooth owner only; all provider calls remain on that task. */
int bluetooth_playback_step(struct bluetooth_application *,struct bt_sco *,uint64_t acl,uint64_t audio,uint64_t now,uint64_t deadline);
int bluetooth_playback_cancelled(struct bluetooth_application *);
int bluetooth_playback_done(struct bluetooth_application *);
int bluetooth_playback_discard(struct bluetooth_application *,unsigned bytes,unsigned status);
void bluetooth_playback_stop(struct bluetooth_application *);
void bluetooth_playback_closed(struct bluetooth_application *);
#endif
