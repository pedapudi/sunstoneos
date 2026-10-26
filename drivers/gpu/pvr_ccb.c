/* SPDX-License-Identifier: MIT
 * Firmware queue layout and publication semantics adapted from pvr_ccb.c,
 * pvr_fw.c and pvr_rogue_fwif.h. Copyright (c) 2023 Imagination Technologies Ltd.
 * See LICENSE.pvr for retained terms.
 */
#include "pvr_ccb.h"
#include "a733_range.h"
#include <string.h>

enum { WRITE_OFFSET = 0, READ_OFFSET = 128, WRAP_MASK = 132, COMMAND_SIZE = 136 };
enum { FREE, PENDING, DONE };

size_t pvr_ccb_region_bytes(enum pvr_ccb_region r)
{
    switch (r) {
    case PVR_CCB_HOST_CONTROL: case PVR_CCB_FIRMWARE_CONTROL: return PVR_CCB_CONTROL_BYTES;
    case PVR_CCB_HOST_COMMANDS: return PVR_CCB_HOST_SLOTS * PVR_CCB_COMMAND_BYTES;
    case PVR_CCB_RETURNS: return PVR_CCB_HOST_SLOTS * 4;
    case PVR_CCB_FIRMWARE_COMMANDS: return PVR_CCB_FIRMWARE_SLOTS * PVR_CCB_COMMAND_BYTES;
    default: return 0;
    }
}
static int outside(const struct pvr_ccb *q, const void *p, size_t bytes)
{
    return a733_ranges_disjoint_nonempty(q, sizeof(*q), p, bytes);
}
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int command_valid(const unsigned char *p)
{
    uint32_t type = u32(p);
    return (type & 0xffff8000u) == PVR_CCB_MAGIC && (type & 0x7fff);
}
static int read32(struct pvr_ccb *q, enum pvr_ccb_region r, uint32_t offset, uint32_t *out)
{
    unsigned char bytes[4];
    if (!q->ops.read(q->context, r, offset, bytes, 4)) return 0;
    *out = u32(bytes);
    return 1;
}
static int write32(struct pvr_ccb *q, enum pvr_ccb_region r, uint32_t offset, uint32_t value)
{
    unsigned char bytes[4];
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (i * 8));
    return q->ops.write(q->context, r, offset, bytes, 4);
}
static int barrier(struct pvr_ccb *q, enum pvr_ccb_barrier b)
{
    return q->ops.barrier(q->context, b);
}
static enum pvr_ccb_result fail(struct pvr_ccb *q)
{
    q->phase = PVR_CCB_FAILED;
    return PVR_CCB_FAULT;
}
static enum pvr_ccb_result active(struct pvr_ccb *q)
{
    if (!a733_span_excl_nonempty(q, sizeof(*q))) return PVR_CCB_INVALID;
    if (q->phase == PVR_CCB_FAILED || q->phase == PVR_CCB_RETIRED) return PVR_CCB_FAULT;
    return q->phase == PVR_CCB_ACTIVE ? PVR_CCB_OK : PVR_CCB_INVALID;
}

/* Validate immutable ABI fields as well as host-owned cursor values. Firmware
 * may advance its cursor during these reads; a sampled position is sufficient.
 */
static int control(struct pvr_ccb *q, enum pvr_ccb_region r, uint32_t mask,
                   uint32_t *write, uint32_t *read)
{
    uint32_t wrap, size;
    return barrier(q, PVR_CCB_ACQUIRE) && read32(q, r, WRITE_OFFSET, write) &&
        read32(q, r, READ_OFFSET, read) && read32(q, r, WRAP_MASK, &wrap) &&
        read32(q, r, COMMAND_SIZE, &size) && wrap == mask &&
        size == PVR_CCB_COMMAND_BYTES && *write <= mask && *read <= mask;
}
static int host_control(struct pvr_ccb *q)
{
    uint32_t write, read;
    if (!control(q, PVR_CCB_HOST_CONTROL, PVR_CCB_HOST_SLOTS - 1, &write, &read) ||
        write != q->host_write ||
        ((read - q->host_read) & (PVR_CCB_HOST_SLOTS - 1)) >
        ((write - q->host_read) & (PVR_CCB_HOST_SLOTS - 1))) return 0;
    q->host_read = read;
    return 1;
}

enum pvr_ccb_result pvr_ccb_init(struct pvr_ccb *q, const struct pvr_ccb_ops *ops,
                                void *context, uint64_t epoch)
{
    if (!outside(q, ops, sizeof(*ops)) || q->phase != PVR_CCB_NEW || !epoch ||
        !ops->read || !ops->write || !ops->barrier || !ops->kick) return PVR_CCB_INVALID;
    memset(q, 0, sizeof(*q));
    q->ops = *ops; q->context = context; q->epoch = epoch;
    q->phase = PVR_CCB_FAILED; /* Partial formatting never permits publication. */
    const unsigned char zero[PVR_CCB_CONTROL_BYTES] = {0};
    for (unsigned r = 0; r < 5; ++r) {
        size_t size = pvr_ccb_region_bytes((enum pvr_ccb_region)r);
        for (uint32_t offset = 0; offset < size;) {
            size_t bytes = size - offset;
            if (bytes > sizeof(zero)) bytes = sizeof(zero);
            if (!q->ops.write(context, (enum pvr_ccb_region)r, offset, zero, bytes)) return PVR_CCB_FAULT;
            offset += (uint32_t)bytes;
        }
    }
    if (!write32(q, PVR_CCB_HOST_CONTROL, WRAP_MASK, PVR_CCB_HOST_SLOTS - 1) ||
        !write32(q, PVR_CCB_HOST_CONTROL, COMMAND_SIZE, PVR_CCB_COMMAND_BYTES) ||
        !write32(q, PVR_CCB_FIRMWARE_CONTROL, WRAP_MASK, PVR_CCB_FIRMWARE_SLOTS - 1) ||
        !write32(q, PVR_CCB_FIRMWARE_CONTROL, COMMAND_SIZE, PVR_CCB_COMMAND_BYTES) ||
        !barrier(q, PVR_CCB_RELEASE)) return PVR_CCB_FAULT;
    q->phase = PVR_CCB_PREPARED;
    return PVR_CCB_OK;
}

enum pvr_ccb_result pvr_ccb_start(struct pvr_ccb *q, uint64_t now)
{
    if (!a733_span_excl_nonempty(q, sizeof(*q))) return PVR_CCB_INVALID;
    if (q->phase == PVR_CCB_FAILED) return PVR_CCB_FAULT;
    if (q->phase != PVR_CCB_PREPARED) return PVR_CCB_INVALID;
    uint32_t write, read;
    if (!host_control(q) || q->host_read ||
        !control(q, PVR_CCB_FIRMWARE_CONTROL, PVR_CCB_FIRMWARE_SLOTS - 1, &write, &read) ||
        read) return fail(q);
    q->last_now = now;
    q->phase = PVR_CCB_ACTIVE;
    return PVR_CCB_OK;
}

enum pvr_ccb_result pvr_ccb_submit(struct pvr_ccb *q, const void *command,
                                  uint64_t now, uint64_t deadline, struct pvr_ccb_token *token)
{
    enum pvr_ccb_result status = active(q);
    if (status != PVR_CCB_OK) return status;
    if (!outside(q, command, PVR_CCB_COMMAND_BYTES) || !outside(q, token, sizeof(*token)) ||
        !command_valid(command) || now < q->last_now) return PVR_CCB_INVALID;
    if (deadline <= now) return PVR_CCB_EXPIRED;
    q->last_now = now;
    if (!host_control(q)) return fail(q);
    uint32_t old = q->host_write, next = (old + 1) & (PVR_CCB_HOST_SLOTS - 1);
    if (next == q->host_read || q->slots[old].state != FREE) return PVR_CCB_AGAIN;
    if (q->serial == UINT64_MAX) return fail(q);
    struct pvr_ccb_slot *slot = &q->slots[old];
    *slot = (struct pvr_ccb_slot){++q->serial, deadline, PENDING, 0};
    if (!q->ops.write(q->context, PVR_CCB_HOST_COMMANDS, old * PVR_CCB_COMMAND_BYTES,
                      command, PVR_CCB_COMMAND_BYTES) ||
        !write32(q, PVR_CCB_RETURNS, old * 4, 0) || !barrier(q, PVR_CCB_RELEASE)) return fail(q);
    q->host_write = next;
    if (!write32(q, PVR_CCB_HOST_CONTROL, WRITE_OFFSET, next) ||
        !barrier(q, PVR_CCB_RELEASE) || !q->ops.kick(q->context, 0x0b00, 0) ||
        !barrier(q, PVR_CCB_DEVICE_COMPLETE)) return fail(q);
    *token = (struct pvr_ccb_token){q->epoch, slot->serial, old};
    return PVR_CCB_OK;
}

enum pvr_ccb_result pvr_ccb_poll(struct pvr_ccb *q, uint64_t now)
{
    enum pvr_ccb_result status = active(q);
    if (status != PVR_CCB_OK) return status;
    if (now < q->last_now) return PVR_CCB_INVALID;
    q->last_now = now;
    if (!host_control(q) || !barrier(q, PVR_CCB_ACQUIRE)) return fail(q);
    for (unsigned i = 0; i < PVR_CCB_HOST_SLOTS; ++i) {
        struct pvr_ccb_slot *slot = &q->slots[i];
        if (slot->state != PENDING) continue;
        uint32_t value;
        if (!read32(q, PVR_CCB_RETURNS, i * 4, &value) || value & ~7u) return fail(q);
        if (value & 1) {
            slot->status = value;
            slot->state = DONE;
        } else if (now >= slot->deadline) return fail(q);
    }
    return PVR_CCB_OK;
}

enum pvr_ccb_result pvr_ccb_collect(struct pvr_ccb *q, struct pvr_ccb_token token, uint32_t *out)
{
    if (!outside(q, out, sizeof(*out)) ||
        (q->phase != PVR_CCB_ACTIVE && q->phase != PVR_CCB_FAILED && q->phase != PVR_CCB_RETIRED) ||
        token.epoch != q->epoch || !token.serial || token.slot >= PVR_CCB_HOST_SLOTS)
        return PVR_CCB_INVALID;
    struct pvr_ccb_slot *slot = &q->slots[token.slot];
    if (slot->state == FREE || slot->serial != token.serial) return PVR_CCB_INVALID;
    if (slot->state != DONE) return q->phase == PVR_CCB_ACTIVE ? PVR_CCB_AGAIN : PVR_CCB_FAULT;
    *out = slot->status;
    slot->state = FREE;
    return PVR_CCB_OK;
}

enum pvr_ccb_result pvr_ccb_settle(struct pvr_ccb *q, struct pvr_ccb_token token, uint32_t *out)
{
    enum pvr_ccb_result result = pvr_ccb_collect(q, token, out);
    if (result != PVR_CCB_AGAIN) return result;
    /* Still pending in an active queue: the firmware's read offset, sampled
     * by the last poll, shows whether it has consumed this slot. */
    const uint32_t mask = PVR_CCB_HOST_SLOTS - 1;
    const uint32_t queued = (q->host_write - token.slot) & mask, consumed = (q->host_read - token.slot) & mask;
    if (!consumed || consumed > queued) return PVR_CCB_AGAIN;
    q->slots[token.slot].state = FREE;
    *out = 0;
    return PVR_CCB_OK;
}

enum pvr_ccb_result pvr_ccb_receive(struct pvr_ccb *q, void *event)
{
    enum pvr_ccb_result status = active(q);
    if (status != PVR_CCB_OK) return status;
    if (!outside(q, event, PVR_CCB_COMMAND_BYTES)) return PVR_CCB_INVALID;
    uint32_t write, read;
    if (!control(q, PVR_CCB_FIRMWARE_CONTROL, PVR_CCB_FIRMWARE_SLOTS - 1, &write, &read) ||
        read != q->firmware_read) return fail(q);
    if (read == write) return PVR_CCB_AGAIN;
    if (!barrier(q, PVR_CCB_ACQUIRE) ||
        !q->ops.read(q->context, PVR_CCB_FIRMWARE_COMMANDS, read * PVR_CCB_COMMAND_BYTES,
                     q->last_event, PVR_CCB_COMMAND_BYTES) || !command_valid(q->last_event)) return fail(q);
    uint32_t type = u32(q->last_event);
    uint32_t next = (read + 1) & (PVR_CCB_FIRMWARE_SLOTS - 1);
    /* Complete the payload read before releasing this slot to firmware. */
    if (!barrier(q, PVR_CCB_RELEASE) ||
        !write32(q, PVR_CCB_FIRMWARE_CONTROL, READ_OFFSET, next) ||
        !barrier(q, PVR_CCB_RELEASE)) return fail(q);
    q->firmware_read = next;
    memcpy(event, q->last_event, PVR_CCB_COMMAND_BYTES);
    if (type == (PVR_CCB_MAGIC | 109u) || type == (PVR_CCB_MAGIC | 112u)) {
        q->phase = PVR_CCB_FAILED;
        return PVR_CCB_RESTART;
    }
    return PVR_CCB_OK;
}

enum pvr_ccb_result pvr_ccb_abort(struct pvr_ccb *q)
{
    if (!a733_span_excl_nonempty(q, sizeof(*q)) || (q->phase != PVR_CCB_PREPARED &&
        q->phase != PVR_CCB_ACTIVE && q->phase != PVR_CCB_FAILED &&
        q->phase != PVR_CCB_RETIRED)) return PVR_CCB_INVALID;
    return fail(q);
}

enum pvr_ccb_result pvr_ccb_retire(struct pvr_ccb *q)
{
    if (!a733_span_excl_nonempty(q, sizeof(*q)) || (q->phase != PVR_CCB_PREPARED &&
        q->phase != PVR_CCB_ACTIVE)) return PVR_CCB_INVALID;
    q->phase = PVR_CCB_RETIRED;
    return PVR_CCB_OK;
}
