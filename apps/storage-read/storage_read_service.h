#ifndef SUNSTONE_STORAGE_READ_SERVICE_H
#define SUNSTONE_STORAGE_READ_SERVICE_H
#include <stddef.h>
#include <stdint.h>
#define STORAGE_READ_HEADER 288u
#define STORAGE_READ_PIO_BLOCKS 8u
#ifdef SUNSTONE_EMMC_READ
#define STORAGE_READ_MAX_BLOCKS STORAGE_READ_PIO_BLOCKS
#else
#define STORAGE_READ_MAX_BLOCKS 64u
#endif
#define STORAGE_READ_OUTPUT (512u * STORAGE_READ_MAX_BLOCKS)
enum storage_read_status { STORAGE_READ_PENDING, STORAGE_READ_SUCCESS,
 STORAGE_READ_RETAINED, STORAGE_READ_IDLE, STORAGE_READ_REJECTED };
struct storage_read_request {
 uint64_t first_sector, request_id;
 uint32_t blocks, reserved;
};
/* The image binds exactly two tasks before scheduling: USB and its native
 * application consumer. Each owns one copied request/result slot until ack.
 * submit: 1 accepted, 0 busy/not identified, -1 invalid, -2 terminal lifetime.
 * Invalid ranges cause no provider calls. Caller buffers are never retained. */
int storage_read_submit(const struct storage_read_request *,uint64_t *ticket);
/* Copies one complete immutable record into client-owned storage. Pending and
 * idle records contain no sector bytes. No partial copy is performed. */
int storage_read_copy(uint8_t *,size_t capacity,unsigned *bytes);
/* Acknowledge only after the copied result is no longer borrowed by transport.
 * Exact successful ticket only; 1 consumed, 0 busy/refused. */
int storage_read_ack(uint64_t ticket);
/* Matching active or completed request only. Irrevocably ends this session;
 * borrowed controller storage and every platform reservation remain retained.
 * Returns 1 accepted, 0 mutex busy, -1 stale/refused. */
int storage_read_cancel(uint64_t ticket);
#endif
