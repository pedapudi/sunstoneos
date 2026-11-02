#ifndef SUNSTONE_SD_READ_PROBE_H
#define SUNSTONE_SD_READ_PROBE_H
/* Wire formats of the read-only microSD backup probe (apps/sd-backup-probe/README.md), a fork of apps/sd-read-probe. */
#include <stdint.h>

#define SDP_INPUT_MAGIC UINT64_C(0x3144524450445353)   /* "SSPDRD1" in little-endian bytes */
#define SDP_REPORT_MAGIC UINT64_C(0x3154505250445353)
#define SDP_MAX_READS 256u
#define SDP_MAX_CAPTURE 512u
#define SDP_TIMED 32u            /* reads whose per-phase times are reported */

/* Sequential plan: read i covers blocks sectors from first + i * blocks. The probe also
 * stores up to SDP_MAX_CAPTURE single sectors, the card layout, outside any timed read. */
struct sdp_input {
    uint64_t magic;
    uint32_t reads, blocks, first, capture_count;
    uint32_t capture[SDP_MAX_CAPTURE];
    uint64_t fat_first, fat_sectors;   /* a FAT32 partition to inspect read-only; fat_sectors zero: none */
    uint64_t region_bytes;             /* a region to take from upper memory; zero: none */
    uint32_t region_flags;             /* SDP_REGION_LOAD: load the named file into it */
    uint32_t uhs_mode;                 /* 0: high speed at 3.3 V, S18A observed; 1: UHS-I lifetime 1 (switch, SDR25 at 1.8 V, bank back to 3.3 V at close);
                                          2: UHS-I lifetime 2 (the card still signals 1.8 V: bank to 1.8 V first, identify without CMD11, SDR25) */
    char file_name[48];                /* root-directory path of the file to load */
    uint8_t file_sha256[32];           /* its SHA-256 */
};
#define SDP_REGION_LOAD 1u

#define SDP_FAT_ENTRIES 64u
struct sdp_fat_entry {
    char name[64];                  /* truncated to 63 characters */
    uint32_t size, first_cluster;
    uint8_t attributes, long_name, truncated, reserved;
};
/* The volume and its root directory, read through the read-only FAT32 reader. */
struct sdp_fat {
    int32_t mount_status, free_status, list_status;
    uint32_t sectors_per_cluster, reserved_sectors, fats, fat_sectors, root_cluster, clusters;
    uint32_t volume_id, hint_free, hint_next, counted_free;
    uint64_t data_start, partition_first, partition_sectors;
    uint32_t entry_count, entries_seen;
    struct sdp_fat_entry entry[SDP_FAT_ENTRIES];
};
/* The large application region test (apps/sd-read-probe/region.c). */
#define SDP_RT_CASES 16u
#define SDP_RT_PASSES 20u
#define SDP_RT_PREPARED 1u
#define SDP_RT_ISOLATION 2u
#define SDP_RT_PASSES_DONE 4u
#define SDP_RT_FILE_ABSENT 8u
#define SDP_RT_LOADED 16u
#define SDP_RT_DIGEST_OK 32u
#define SDP_RT_UPPER_MAPPED 64u
struct sdp_region_case {
    uint32_t space, op, ec, reserved;     /* space 0 owner, 1 other application, 2 2 MiB-block view; op 0 read, 1 write, 2 control */
    uint64_t address, esr, far, result;
};
struct sdp_region_pass {
    uint32_t kind, reserved;              /* 0: 40 MiB control, 1: 1 GiB with 1 GiB blocks, 2: 1 GiB with 2 MiB blocks */
    uint64_t bytes, ticks, cycles;        /* ticks of the 24 MHz counter */
    uint64_t counter[4];                  /* L1D TLB refill, L2D TLB refill, DTLB walk, L1D cache refill */
    uint64_t xor_fold;
};
struct sdp_region {
    int32_t status, ledger_status, build_status[3], file_status, load_status, reserved0;
    uint32_t flags, case_count, pass_count, pmu_ok;
    uint32_t tables[3], asid[3];
    uint64_t base, bytes, free_before, free_after;
    uint64_t file_bytes, load_ticks, load_requests;
    uint8_t expected[32], digest[32];
    uint64_t el1_xor, el0_xor_owner, el1_xor_strided;
    struct sdp_region_case cases[SDP_RT_CASES];
    struct sdp_region_pass pass[SDP_RT_PASSES];
    uint64_t stop_class, stop_esr, stop_far, stop_elr;   /* the exception that ended an unexpected EL0 call */
};
struct sdp_timing { uint32_t command_us, first_byte_us, done_us, clean_us, invalidate_us, polls; };

struct sdp_report {
    uint64_t magic, version;
    uint64_t stage, reason, done, planned, identified_us, elapsed_us;
    uint64_t read_window_ticks;     /* first submission to last completion, 24 MHz counter */
    uint64_t bytes;                 /* bytes read by the plan */
    uint64_t cid[4], csd[4], ocr, rca, sectors, scr[2];
    uint64_t mode;                  /* profile | width << 8 | timing << 16 | divisor << 24 */
    uint64_t card_hz;
    uint64_t baseline_start[16], baseline_end[16];   /* restoration words at start and after close */
    uint64_t clock_off, pmic_parked, capture_done;
    uint32_t elapsed[SDP_MAX_READS], crc[SDP_MAX_READS];
    struct sdp_timing timing[SDP_TIMED];
    uint8_t sector[SDP_MAX_CAPTURE][512];
    struct sdp_fat fat;
    struct sdp_region region;
    /* ACMD13 (the SD Status register) and the allocation unit decoded from it, both ways. */
    uint8_t sd_status[64];
    uint64_t status_result, au_sectors, au_sectors_uhs, status_us;
    /* The CMD6 response of the last switch: the support bits of every function group (byte 12 and 13 are group 1, the bus speed
     * modes: bit 0 SDR12, 1 SDR25, 2 SDR50, 3 SDR104, 4 DDR50) and the function the switch selected (byte 16 low nibble). */
    uint8_t switch_status[64];
    /* UHS-I lifetimes: [0] mode, [1] the card signals 1.8 V, [2] outcome (sunxi_mmc_uhs_outcome), [3] identified by re-entry, [4] GPIO_POW_VAL and
     * [5] GPIO_POW_VAL_SET_CTL when the card was identified, [6] and [7] the same after the close. */
    uint64_t uhs[8];
};
_Static_assert(sizeof(struct sdp_input) == 160 - 6 * 4 + 4 * SDP_MAX_CAPTURE, "input wire size");

#endif
