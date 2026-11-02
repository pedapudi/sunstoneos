#ifndef SUNSTONE_SD_READ_PROBE_H
#define SUNSTONE_SD_READ_PROBE_H
/* Wire formats of the read-only microSD probe (apps/sd-read-probe/README.md). */
#include <stdint.h>

#define SDP_INPUT_MAGIC UINT64_C(0x3144524450445353)   /* "SSPDRD1" in little-endian bytes */
#define SDP_REPORT_MAGIC UINT64_C(0x3154505250445353)
#define SDP_MAX_READS 256u
#define SDP_MAX_CAPTURE 6u
#define SDP_TIMED 32u            /* reads whose per-phase times are reported */

/* Sequential plan: read i covers blocks sectors from first + i * blocks. The probe also
 * stores up to SDP_MAX_CAPTURE single sectors, the card layout, outside any timed read. */
struct sdp_input {
    uint64_t magic;
    uint32_t reads, blocks, first, capture_count;
    uint32_t capture[SDP_MAX_CAPTURE];
    uint64_t fat_first, fat_sectors;   /* a FAT32 partition to inspect read-only; fat_sectors zero: none */
};

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
};
_Static_assert(sizeof(struct sdp_input) == 64, "input wire size");
_Static_assert(sizeof(struct sdp_report) <= 16000, "report fits its payload");
#endif
