#ifndef SUNSTONE_CAMERA_SESSION_H
#define SUNSTONE_CAMERA_SESSION_H
#include "a733_camera_native.h"
#include "a733_native_usb.h"
#define A733_STORAGE_SESSION_ADDRESS UINT64_C(0x41f00000)
#ifdef SUNSTONE_CAMERA_SUPERVISED
#define CAMERA_SESSION_MAGIC UINT64_C(0x4153373343415332)
#define CAMERA_CONTRACT_MAGIC UINT64_C(0x4153373343414333)
#define CAMERA_SESSION_VERSION 2u
#define CAMERA_CONTRACT_VERSION 3u
#else
#define CAMERA_SESSION_VERSION 1u
#define CAMERA_CONTRACT_VERSION 1u
#define CAMERA_SESSION_MAGIC UINT64_C(0x4153373343414131)
#define CAMERA_CONTRACT_MAGIC UINT64_C(0x4153373343414332)
#endif
#define CAMERA_SESSION_SECONDS 60u
/* Pointer-free immutable launch input, published with the existing contract.
 * Evidence must cover media domains, reset, all masters, electrical wiring,
 * memory coherence/guards, USB sink ownership and bounded independent recovery.
 * Bytes and hashes identify that authority; they do not manufacture it. */
struct camera_session {
    uint64_t magic,version,bytes,epoch,generation,lease;
    uint8_t image[32],evidence[32],memory[32],recovery[32];
    uint64_t deadline_us,minimum_reload_us,maximum_recovery_us,guard_us,reset_by_us;
    uint64_t reload_before_ticks,reload_after_ticks;
    uint32_t rtc_control,rtc_divider,rtc_source,pck_client;
    struct a733_native_usb_config shared;
    struct a733_camera_boot_record camera;
    struct a733_media_clock_config media;
};
_Static_assert(sizeof(struct camera_session)<=4096,"immutable session page");
int camera_session_valid(const struct camera_session *,uint64_t);
#endif
