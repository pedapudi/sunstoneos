#ifndef SUNSTONE_A733_STORAGE_UPDATE_H
#define SUNSTONE_A733_STORAGE_UPDATE_H
#include "a733_mmc_platform.h"
#include "a733_storage_digest.h"
#define A733_STORAGE_CHUNK 1024u /* BACKUP reads up to two sectors. */
#define A733_STORAGE_WRITE_BYTES 512u /* WRITE carries exactly one sector. */
#define A733_STORAGE_GRANT_BYTES 256u
#define A733_STORAGE_BODY 1152u
/* This grant belongs to trusted bootstrap memory, never to an application USB
 * request. Host bootstrap authenticates actual protected backup bytes and
 * physical qualification artifacts. Hashes identify those artifacts; a hash
 * cannot prove recovery. A missing grant never permits mutation. */
struct a733_storage_grant {
    uint32_t mode,kind,host; /* mode0 backup only; mode1 update or explicit restore. */
    uint8_t nonce[32];
    uint32_t cid[4];
    uint64_t sectors,first,blocks,deadline_us;
    uint8_t backup[32],candidate[32],allocation[32],recovery[32]; /* candidate: ordered chunk commitment. */
};
enum a733_storage_command { A733_STORAGE_INFO=1,A733_STORAGE_BACKUP=2,
    A733_STORAGE_SEAL=3,A733_STORAGE_WRITE=4,A733_STORAGE_FINISH=5 };
enum a733_storage_state { A733_STORAGE_IDLE,A733_STORAGE_READ_START,
    A733_STORAGE_READ_WAIT,A733_STORAGE_WRITE_START,A733_STORAGE_WRITE_WAIT,
    A733_STORAGE_VERIFY_START,A733_STORAGE_VERIFY_WAIT,A733_STORAGE_RETAINED };
struct a733_storage_update {
    struct a733_storage_update *self;
    struct a733_mmc_platform *platform;
    struct a733_storage_grant grant;
    struct sunxi_mmc_card card;
    struct sunxi_mmc_write_identity identity;
    struct sunxi_mmc_write_request request;
    struct storage_digest before_hash,after_hash;
    atomic_uint lost;
    const atomic_uint *transport_lost; /* Bound once by the permanent mailbox worker. */
    enum a733_storage_state state;
    uint64_t read_blocks,written_blocks,last_now,operation_deadline;
    uint32_t sequence,active_command;
    unsigned sealed,finished,response_ready,response_size,chunk_bytes;
    uint8_t input[A733_STORAGE_CHUNK],readback[A733_STORAGE_CHUNK];
    uint8_t response[A733_STORAGE_BODY],backup_hash[32],receipt[32],chain[32],next_chain[32];
    struct sunxi_mmc_result result;
};
/* All storage is private, permanent and disjoint from provider graphs. Init
 * accepts only an actually READY platform and copies actual card identity.
 * Same task owns init, request, poll and response. Transport may only latch
 * loss concurrently. The resulting exclusive range binding cannot be closed. */
int a733_storage_update_init(struct a733_storage_update *,struct a733_mmc_platform *,
    const struct a733_storage_grant *,uint64_t deadline);
int a733_storage_grant_decode(struct a733_storage_grant *,const uint8_t *,size_t);
void a733_storage_grant_encode(const struct a733_storage_grant *,uint8_t[256]);
/* Request wire: nonce32, sequence u32, command u32, offset u64, size u32,
 * reserved u32=0, durable artifact SHA25632, payload. WRITE adds the next chunk commitment32
 * before its payload. The immutable grant commits the complete ordered image. All integers little endian.
 * Exactly one request may be pending; a duplicate is a fault, never a replay.
 * Response: sequence u32, status u32, offset u64, bytes u32, reserved u32,
 * digest32, payload. Status0 completes; status1 is retained failure. */
int a733_storage_update_request(struct a733_storage_update *,const uint8_t *,size_t);
void a733_storage_update_poll(struct a733_storage_update *);
int a733_storage_update_response(struct a733_storage_update *,uint8_t *,size_t,size_t *);
void a733_storage_update_lost(struct a733_storage_update *);
/* Concrete provider used only by the platform/core binding. No success callback
 * can replace the immutable grant, completed backup reads or active request. */
int a733_storage_update_authorize(void *,const struct sunxi_mmc_write_identity *,const struct sunxi_mmc_write_request *);
int a733_storage_update_mmio(const struct a733_storage_update *,uintptr_t,uint32_t);
/* FIFO burst form: count words must fit in the active sector's remaining bytes. */
int a733_storage_update_fifo(const struct a733_storage_update *,uintptr_t,unsigned count);
int a733_storage_update_alive(const struct a733_storage_update *);
int a733_storage_update_owned(const struct a733_storage_update *,const struct a733_mmc_platform *);
/* One USB producer and one storage worker; mailbox bytes stay owned until the
 * complete response is consumed. Loss is permanent and never clears a slot. */
struct a733_storage_channel {
    atomic_uint state,lost; /* 0 empty,1 request,2 worker,3 response,4 producer. */
    unsigned request_size,response_size;
    uint8_t request[A733_STORAGE_BODY],response[A733_STORAGE_BODY];
};
void a733_storage_channel_init(struct a733_storage_channel *);
int a733_storage_channel_submit(struct a733_storage_channel *,const uint8_t *,size_t);
void a733_storage_channel_worker(struct a733_storage_channel *,struct a733_storage_update *);
int a733_storage_channel_response(struct a733_storage_channel *,uint8_t *,size_t,size_t *);
void a733_storage_channel_lost(struct a733_storage_channel *);
#endif
