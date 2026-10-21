#ifndef SUNSTONE_A733_BOARD_APPLICATION_H
#define SUNSTONE_A733_BOARD_APPLICATION_H
#include <stdint.h>
#include <stddef.h>
#define A733_BOARD_SESSION_ADDRESS UINT64_C(0x41f00000)
#define A733_BOARD_SUPERVISED_SESSION_MAGIC UINT64_C(0x4153373342535332)
#define A733_BOARD_SUPERVISED_CONTRACT_MAGIC UINT64_C(0x4153373342534332)
#define A733_BOARD_SUPERVISED_RESULT_MAGIC UINT64_C(0x4153373342535232)
#ifdef SUNSTONE_BOARD_SUPERVISED
#define A733_BOARD_SESSION_MAGIC A733_BOARD_SUPERVISED_SESSION_MAGIC
#define A733_BOARD_CONTRACT_MAGIC A733_BOARD_SUPERVISED_CONTRACT_MAGIC
#define A733_BOARD_SESSION_VERSION 2u
#define A733_BOARD_CONTRACT_VERSION 2u
#else
#define A733_BOARD_SESSION_MAGIC UINT64_C(0x41533733424f4131)
#define A733_BOARD_CONTRACT_MAGIC UINT64_C(0x41533733424f4331)
#define A733_BOARD_SESSION_VERSION 1u
#define A733_BOARD_CONTRACT_VERSION 1u
#endif
/* Resident lifetime (docs/os-image.md). The session keeps the supervised
 * layout with zero time fields. The contract has its own magic number, arms
 * the four-second watchdog, and has no FEL return and no diagnostic deadline.
 * A supervising task renews the watchdog while every listed service makes
 * progress. */
#define A733_BOARD_RESIDENT_CONTRACT_MAGIC UINT64_C(0x4153373342524332)
#ifdef SUNSTONE_BOARD_RESIDENT
#ifndef SUNSTONE_BOARD_SUPERVISED
#error The resident lifetime uses the supervised session layout
#endif
#undef A733_BOARD_CONTRACT_MAGIC
#define A733_BOARD_CONTRACT_MAGIC A733_BOARD_RESIDENT_CONTRACT_MAGIC
#endif
/* The integrated OS image (apps/os) extends the board session to
 * A733_OS_SESSION_BYTES and has its own contract and result magic numbers
 * for each lifetime. The stage-2 loader produces only the resident contract. */
#define A733_OS_SESSION_MAGIC UINT64_C(0x415337334f534131)
#define A733_OS_CONTRACT_MAGIC UINT64_C(0x415337334f534331)
#define A733_OS_RESULT_MAGIC UINT64_C(0x415337334f535231)
#define A733_OS_RESIDENT_CONTRACT_MAGIC UINT64_C(0x415337334f524331)
#define A733_OS_RESIDENT_RESULT_MAGIC UINT64_C(0x415337334f525231)
#define A733_OS_SESSION_BYTES 808u
#ifndef A733_BOARD_SUPERVISED_SECONDS
#define A733_BOARD_SUPERVISED_SECONDS 60u
#endif
#ifndef A733_BOARD_STARTUP_SECONDS
#define A733_BOARD_STARTUP_SECONDS 1u
#endif
#if A733_BOARD_STARTUP_SECONDS < 1 || A733_BOARD_STARTUP_SECONDS >= A733_BOARD_SUPERVISED_SECONDS
#error Invalid supervised board startup interval
#endif
#if defined(SUNSTONE_BOARD_RESIDENT) && A733_BOARD_STARTUP_SECONDS >= 4
#error Resident startup must end before the first watchdog expiry
#endif
/* Existing0/1 include the USB2 source path. Supervised2/3 omit that path;
 * odd selections include radio. Supervised4 requires both USB leaf classes
 * without radio and is admitted only by the board target capability. */
static inline int a733_board_radio_workload(unsigned workload){return !!(workload&1);}
static inline int a733_board_host_workload(unsigned workload){return workload<2||workload==4||workload==6||workload==8||workload==10||workload==12;}
/* Immutable little-endian image-owned input. No pointer or storage grant is
 * accepted. Evidence hashes identify externally qualified, reviewed facts;
 * producing this record does not itself establish those facts. */
struct a733_board_session {
    uint64_t magic,version,bytes,epoch,generation,lease;
    uint8_t image[32],qualification[32];
    uint64_t minimum_reload_us,maximum_recovery_us,guard_us,reset_by_us;
    uint32_t rtc_control,rtc_divider,rtc_source,reserved0;
    uint64_t deadline_us;
    union {uint64_t reload_before_ticks;uint64_t session_start_ticks;};
    uint64_t reload_after_ticks;
    uint64_t topology,cold_master,memory_authority,recovery_authority;
    uint8_t wiring[32],memory[32],recovery[32];
    uint64_t sid_authority;
    uint8_t sid_evidence[32];
    uint64_t radio_profile_address,radio_profile_bytes;
    uint8_t radio_profile_digest[32];
    uint32_t workload,usb_topology; /* 0 direct,1 single self-powered hub for6/8/10/12. */
    uint64_t latch_identity,latch_owner;
    uint32_t latch_known_mask,latch_value;
    uint64_t latch_m_identity,latch_m_owner;
    uint32_t latch_m_known_mask,latch_m_value;
    uint64_t pmic_owner,usb0_owner,typec_owner,usb2_owner;
    uint32_t fabric[3],fabric_hz[3],usb_supply_uv[5];
    uint32_t usb0_cold_reference,usb2_revision,pck_client;
    uint32_t source_min_mv,source_max_mv,source_ma,zero_threshold_max_mv;
    uint32_t fusb_device_id;
    uint8_t pmic_pins[2][3],typec_pins[4][3];
    uint8_t reserved[6];
};
_Static_assert(sizeof(struct a733_board_session)==592,"board session layout");
struct a733_smp_contract;
/* Pure input checks also used by the native EL3 admission and focused check. */
_Static_assert(sizeof(struct a733_board_session)==592,"sealed board session geometry");
_Static_assert(offsetof(struct a733_board_session,usb_topology)==404,"sealed USB topology offset");
int a733_board_session_valid(const struct a733_board_session *,uint64_t now_ticks);
int a733_board_contract_valid(const struct a733_smp_contract *,
    const struct a733_board_session *,const uint8_t build_id[32],uint64_t now_ticks);
#endif
