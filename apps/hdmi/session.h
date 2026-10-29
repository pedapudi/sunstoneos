#ifndef SUNSTONE_HDMI_SESSION_H
#define SUNSTONE_HDMI_SESSION_H
#include "a733_hdmi_native.h"
#include "a733_pins.h"
#if SUNSTONE_HDMI_FRAMEBUFFER
#include "framebuffer.h"
#endif
#if SUNSTONE_HDMI_AUDIO
#include "../hdmi-audio/session.h"
#endif
#if SUNSTONE_HDMI_AUDIO_USB
#include "../hdmi-audio/usb-authority.h"
#endif
#define A733_STORAGE_SESSION_ADDRESS UINT64_C(0x41f00000)
#define HDMI_SESSION_MAGIC UINT64_C(0x4153373348444131)
#define HDMI_CONTRACT_MAGIC UINT64_C(0x4153373348444331)
#define HDMI_WINDOW_TICKS (UINT64_C(20)*24000000)
/* Pointer-free immutable input. Evidence binds cold/reset analog provenance,
 * inactive VO1/resistor-word ownership, rails, inherited PMIC bus, memory and
 * supervised manual recovery to this image and session. No qualification bit
 * or built-in analog image is supplied. PMIC deadline is zero in this template;
 * the application fills its private copy from the authenticated boot contract. */
struct hdmi_session {
    uint64_t magic,version,bytes,epoch,generation,lease;
    uint8_t image[32],evidence[32],analog[32],power[32],supervision[32];
    uint64_t supervisor,owner;
    uint32_t pck_client,reserved;
    struct a733_hdmi_request request;
    struct a733_pmic_config pmic;
    struct a733_pin_handoff pins;
#if SUNSTONE_HDMI_FRAMEBUFFER
    struct hdmi_framebuffer_session framebuffer;
#endif
#if SUNSTONE_HDMI_AUDIO
    struct hdmi_audio_session audio;
#endif
#if SUNSTONE_HDMI_AUDIO_USB
    struct hdmi_audio_usb_authority usb;
#endif
#if SUNSTONE_HDMI_AUDIO_STREAM
    uint8_t manifest_sha256[32];
#endif
};
#if SUNSTONE_HDMI_AUDIO_STREAM
_Static_assert(sizeof(struct hdmi_session)==848&&offsetof(struct hdmi_session,manifest_sha256)==816,"audio stream session ABI");
#elif SUNSTONE_HDMI_AUDIO_USB
_Static_assert(sizeof(struct hdmi_session)==816&&offsetof(struct hdmi_session,audio)==440&&offsetof(struct hdmi_session,usb)==656,"audio USB session ABI");
#elif SUNSTONE_HDMI_AUDIO
_Static_assert(sizeof(struct hdmi_session)==656&&offsetof(struct hdmi_session,audio)==440,"audio session ABI");
#endif
_Static_assert(sizeof(struct hdmi_session)<=4096,"immutable HDMI session page");
_Static_assert(offsetof(struct hdmi_session,image)==48 &&
    offsetof(struct hdmi_session,evidence)==80 && offsetof(struct hdmi_session,analog)==112 &&
    offsetof(struct hdmi_session,power)==144 && offsetof(struct hdmi_session,supervision)==176 &&
    offsetof(struct hdmi_session,supervisor)==208&&offsetof(struct hdmi_session,request)==232,
    "offline session prefix ABI");
int hdmi_session_valid(const struct hdmi_session *);
#endif
