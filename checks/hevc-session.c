#include "a733_hevc_application.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Synthetic provider identities exercise pure admission only. They convey no
 * physical authority and do not form a native provider-construction fixture. */
int main(void)
{
    struct a733_hevc_session s = {
        .board = {
            .magic = A733_BOARD_SESSION_MAGIC, .version = 2, .bytes = 592,
            .epoch = 1, .generation = 2, .lease = 3, .topology = 4,
            .cold_master = 5, .memory_authority = 6, .recovery_authority = 7,
            .workload = 2, .pmic_owner = 10, .usb0_owner = 11,
            .fabric = {0, 0x80000000, 0x80000000},
            .fabric_hz = {24000000, 24000000, 24000000},
            .usb_supply_uv = {3300000, 3300000, 1200000, 900000},
            .usb0_cold_reference = 1
        },
        .magic = A733_HEVC_SESSION_MAGIC, .version = 1, .bytes = 856,
        .owner = 12, .authority = 13, .module = 1, .module_hz = 234000000,
        .iommu_gates = {0x10007, 0x10007}, .supply_uv = 800000, .supply_rail = 1,
        .pictures = {{0, 11698}, {11698, 6084}}
    };
    memset(s.board.image, 1, 32);
    memset(s.board.qualification, 2, 32);
    memset(s.board.recovery, 2, 32);
    memset(s.board.wiring, 3, 32);
    memset(s.board.memory, 4, 32);
    memset(s.initial, 5, 32);
    memset(s.memory, 6, 32);
    memset(s.electrical, 7, 32);
    memset(s.supervision, 8, 32);
    memset(s.input_digest, 9, 32);
    assert(a733_hevc_session_valid(&s));
    s.pictures[0].bytes = s.pictures[1].offset = s.pictures[1].bytes = 262144;
    assert(a733_hevc_session_valid(&s));

    const struct a733_hevc_session valid = s;
    const struct a733_hevc_input_range bad[][2] = {
        {{1, 11698}, {11699, 6084}},       /* Leading gap. */
        {{0, 11698}, {11697, 6084}},       /* Overlap. */
        {{0, 11698}, {11699, 6084}},       /* Interior gap. */
        {{0, 0}, {0, 6084}},              /* Empty first picture. */
        {{0, 11698}, {11698, 0}},         /* Empty second picture. */
        {{0, 262145}, {262145, 1}},       /* First-picture capacity. */
        {{0, 1}, {1, 262145}},            /* Second-picture capacity. */
        {{0, UINT64_MAX}, {UINT64_MAX, 1}}, /* Wrapped clip endpoint. */
        {{0, 1}, {1, UINT64_MAX}}         /* Wrapped second extent. */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        s = valid;
        memcpy(s.pictures, bad[i], sizeof(s.pictures));
        assert(!a733_hevc_session_valid(&s));
    }
    s = valid;
    memset(s.input_digest, 0, 32);
    assert(!a733_hevc_session_valid(&s));
    s = valid;
    s.board.workload = 0;
    assert(!a733_hevc_session_valid(&s));
    puts("HEVC bounded clip admission passed; provider identities are synthetic.");
    return 0;
}
