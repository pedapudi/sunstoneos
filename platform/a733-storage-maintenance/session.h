#ifndef SUNSTONE_A733_STORAGE_SESSION_H
#define SUNSTONE_A733_STORAGE_SESSION_H
#include <stdint.h>
#include <stddef.h>
#define A733_STORAGE_SESSION_ADDRESS UINT64_C(0x41f00000)
#define A733_STORAGE_SESSION_BYTES 704u
#ifdef SUNSTONE_MIMUS_NATIVE
#define A733_STORAGE_SESSION_MAGIC UINT64_C(0x415337334d494d31)
#define A733_STORAGE_CONTRACT_MAGIC UINT64_C(0x415337334d494331)
#else
#define A733_STORAGE_SESSION_MAGIC UINT64_C(0x4153373353545331)
#define A733_STORAGE_CONTRACT_MAGIC UINT64_C(0x4153373353544331)
#endif
#define A733_STORAGE_SESSION_VERSION 1u
#define A733_STORAGE_GRANT_OFFSET 160u
#define A733_STORAGE_RELOAD_BEFORE 640u
#define A733_STORAGE_RELOAD_AFTER 648u
#define A733_STORAGE_TICKS_PER_US UINT64_C(24)
/* One little-endian session record. Offsets:
 * 0 magic (launcher publishes last), 8 version, 16 bytes, 24 epoch,
 * 32 generation, 40 lease, 48 ELF SHA256, 80 qualification SHA256,
 * 112 minimum reload us, 120 maximum recovery us, 128 guard us,
 * 136 absolute reset-by us, 144/148/152 RTC control/divider/source, 156 zero.
 * 160..415 is the existing storage grant, copied verbatim.
 * 416 topology, 424 topology artifact SHA256, 456 MMC SD profile u32
 * (0 one-bit 400 kHz, 1 four-bit default speed, 2 four-bit high speed;
 * nonzero only for SD), 460..487 zero,
 * 488 supply targets[4], 504/512/520 MMC/PMIC/USB owner identities,
 * 528 fabric[3], 540 fabric rates[3], 552 MMC voltage mask,
 * 556 USB cold-reference flag, 560 MMC expected[12][3],
 * 596 MMC desired[12][3], 632 PMIC expected[2][3], 638..639 zero.
 * 640/648 initial reload before/after ticks, written only by the launcher;
 * 656..703 zero. All u64/u32 values use the alignment implied above.
 * Configuration hashes identify reviewed physical evidence; they do not
 * establish it. No C pointers or second storage authorization are uploaded. */
static inline uint32_t a733_storage_wire32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static inline uint64_t a733_storage_wire64(const uint8_t *p)
{return (uint64_t)a733_storage_wire32(p)|(uint64_t)a733_storage_wire32(p+4)<<32;}
struct a733_storage_native_boot;
struct a733_maintenance_transfer;
/* Primary privileged bootstrap only. Input is an immutable local snapshot of
 * the fixed reserved session page. Outputs are private and mutually disjoint.
 * Returns zero without publishing either output on invalid or expired input. */
int a733_storage_session_decode(struct a733_storage_native_boot *,
    struct a733_maintenance_transfer *,const uint8_t[A733_STORAGE_SESSION_BYTES],uint64_t now_ticks);
/* Launcher and EL3 admission use absolute ticks, avoiding rounded early bounds.
 * Full grant and provider validation also runs before the scheduler starts. */
static inline int a733_storage_wire_bounds(const uint8_t *p,uint64_t now)
{
    uint64_t minimum=a733_storage_wire64(p+112),maximum=a733_storage_wire64(p+120);
    uint64_t guard=a733_storage_wire64(p+128),reset=a733_storage_wire64(p+136);
    uint64_t deadline=a733_storage_wire64(p+256);
    return minimum<=UINT64_MAX/24 && maximum<=UINT64_MAX/24 && guard &&
        guard<=UINT64_MAX/24 && reset<=UINT64_MAX/24 && guard<minimum &&
        minimum-guard>1000000 && maximum>=minimum && reset>maximum &&
        deadline && deadline<=reset-maximum && now<deadline*24 &&
        deadline*24-now>24000000;
}
#endif
