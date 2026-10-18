/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_CAMERA_PLATFORM_H
#define SUNSTONE_A733_CAMERA_PLATFORM_H
#include <stdint.h>

#define A733_CAMERA_COMMON_BASE UINT64_C(0x05810000)
enum a733_camera_origin {
    A733_CAMERA_ORIGIN_UNKNOWN,
    A733_CAMERA_COLD_RESET,
    A733_CAMERA_RESOLVED_RESET_HANDOFF
};
enum a733_camera_proof {
    A733_CAMERA_RESET_PROVEN = 1u << 0,
    A733_CAMERA_HANDOFF_RESOLVED = 1u << 1,
    A733_CAMERA_ALL_PATHS_EXCLUDED = 1u << 2,
    A733_CAMERA_PRODUCERS_QUIET = 1u << 3,
    A733_CAMERA_CLOCKS_HELD = 1u << 4,
    A733_CAMERA_POWER_HELD = 1u << 5,
    A733_CAMERA_PINS_QUALIFIED = 1u << 6,
    A733_CAMERA_RAW_TRIM_APPROVED = 1u << 7,
    A733_CAMERA_RECOVERY_QUALIFIED = 1u << 8,
    A733_CAMERA_TRANSACTIONS_DRAINED = 1u << 9,
    A733_CAMERA_MANUAL_POWER_REMOVAL = 1u << 10
};
#define A733_CAMERA_REQUIRED_PROOF 0x3ffu
#define A733_CAMERA_SUPERVISED_PROOF 0x6ffu
struct a733_camera_admission {
    uint64_t token;
    uint32_t proof;
    enum a733_camera_origin origin;
};
struct a733_camera_platform_ops {
    int (*now_us)(void *,uint64_t *);
    /* 1 complete, 0 pending, -1 uncertain failure; token before acquisition.
     * Retain exclusive ownership of BOTH COMPLETE common registers and all
     * three PHY/packet paths across polls, ACTIVE and FAULT. After ACTIVE, a
     * higher-level coordinator may authenticate this same token/owner and
     * delegate a quiescent local path to its explicitly authorized receiver
     * and sensor. Common-word writers remain excluded; delegation must not
     * allow overlapping unsynchronized common-register RMW operations.
     * Prove reset and boot handoff independently; observed zeros are not proof.
     * A resolved handoff must establish a cold reset state, not adopt live PHYs.
     * Reserve clocks/reset, analog rails, sensor pins/rails and interrupt routing;
     * prove stable clocks/power, quiet producers, drained prior transactions and
     * independent recovery. The raw trim profile 0xb00 must be approved for the
     * board: manual/source field names disagree. Hold sensors quiet through
     * ACTIVE and until the coordinator authenticates the assembled capture
     * path and explicitly permits that sensor's packet enable. ACTIVE alone
     * cannot grant either delegation. Unrelated local writers stay excluded.
     * This callback must not configure common analog registers or change IOMMU
     * mappings. Pin/clock/power managers and the actual evidence provider remain
     * platform responsibilities. No current native provider is implemented.
     */
    int (*admit)(void *,uint64_t deadline,struct a733_camera_admission *);
    /* Ordered aligned 32-bit Device accesses relative to COMMON_BASE; read
     * offsets 0,4,0x100,0x200,0x300,0x1000,0x1400,0x1800 only; write 0/4 only.
     * Return 1 or -1; successful writes include a completion barrier.
     */
    int (*read)(void *,uint64_t token,uint16_t off,uint32_t *,uint64_t);
    int (*write)(void *,uint64_t token,uint16_t off,uint32_t,uint64_t);
};
enum a733_camera_platform_state {
    A733_CAMERA_PLATFORM_NEW,A733_CAMERA_PLATFORM_ADMIT,
    A733_CAMERA_PLATFORM_CHECK,A733_CAMERA_PLATFORM_PROGRAM,
    A733_CAMERA_PLATFORM_CALIBRATE,A733_CAMERA_PLATFORM_VERIFY,
    A733_CAMERA_PLATFORM_ACTIVE,A733_CAMERA_PLATFORM_FAULT
};
struct a733_camera_platform {
    struct a733_camera_platform_ops ops;
    struct a733_camera_admission admission;
    void *context;
    uint64_t now,deadline;
    uint32_t required_proof;
    uint32_t observed[8],saved,calibration;
    enum a733_camera_platform_state state;
    uint8_t index,phase,initialized;
};
/* Fresh zeroed storage, one serialized privileged owner; no callback reentry.
 * Fixed cold analog setup for link mode 0, raw trim 0xb00, software SET=0x10.
 * ACTIVE means software-mode RESULT/FLAG and control readbacks passed, not
 * hardware qualification or capture readiness. Ownership remains retained.
 * No inherited-state adoption, shutdown, retry, lease release or IOMMU changes.
 */
int a733_camera_platform_init(struct a733_camera_platform *,
    const struct a733_camera_platform_ops *,void *context);
/* Explicit retained-lifetime mode replaces only qualified independent recovery
 * with externally supervised whole-board power removal. Reset, exclusion,
 * quiet/drained transactions, clocks, pins, power and trim remain mandatory.
 * Neither deadline nor reconnection authorizes release or reuse. */
int a733_camera_platform_supervised_init(struct a733_camera_platform *,
    const struct a733_camera_platform_ops *,void *);
int a733_camera_platform_open(struct a733_camera_platform *,uint64_t deadline);
/* At most one admission/MMIO operation per poll plus monotonic time checks. */
int a733_camera_platform_poll(struct a733_camera_platform *);
#endif
