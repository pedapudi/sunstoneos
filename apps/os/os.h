#ifndef SUNSTONE_OS_H
#define SUNSTONE_OS_H
/* Integrated OS image (docs/os-image.md): the mimus board composition plus a
 * continuous thermal service, optional I2S microphone receive, one fixed
 * interrupt table and one fixed start list. */
#include "a733_board_application.h"
#ifdef SUNSTONE_A733_OS_AUDIO
#include "a733_dma_platform.h"
#ifdef SUNSTONE_BOARD_RESIDENT
#error The stage-2 loader admits only the 808-byte OS session
#endif
/* I2S0 receive facts after the thermal facts. The DMA fields are the I2S
 * image's supervised DMA0 handoff (struct a733_dma_handoff, which audio.c
 * builds from them) and follow its rules (docs/i2s-application.md). The
 * evidence hash identifies the reviewed microphone wiring, bank voltage,
 * audio clock, FIFO route, DMA prior-drain and remote-exclusion records. */
struct os_audio_session {
    uint64_t magic,version,bytes;
    uint8_t evidence[32];
    struct a733_dma_platform_config dma;
    uint64_t dma_controller,dma_evidence;
    uint32_t dma_established;
    uint16_t dma_owned_channels,dma_service_channels;
    uint32_t audio_cold,capture_samples;
    uint8_t inherited[3][3]; /* PB5 BCLK, PB6 LRCK, PB8 DIN: function, pull, drive */
    uint8_t reserved[7];
};
#define OS_AUDIO_SESSION_MAGIC UINT64_C(0x415337334f415531)
#define OS_AUDIO_SESSION_VERSION 1u
#define OS_AUDIO_SESSION_BYTES 184u
/* A733_DMA_HANDOFF_SUPERVISED: manual power removal owns recovery. */
#define OS_AUDIO_DMA_SUPERVISED UINT32_C(0x5f)
_Static_assert(sizeof(struct os_audio_session)==OS_AUDIO_SESSION_BYTES&&
    offsetof(struct os_audio_session,dma)==56&&offsetof(struct os_audio_session,audio_cold)==160,
    "OS audio session geometry");
/* One finite capture of 1 to 5 s of 16 kHz mono S16 per recognition. */
#define OS_AUDIO_MIN_SAMPLES 16000u
#define OS_AUDIO_MAX_SAMPLES 80000u
#define OS_SESSION_BYTES (A733_OS_SESSION_BYTES+OS_AUDIO_SESSION_BYTES)
#else
#define OS_SESSION_BYTES A733_OS_SESSION_BYTES
#endif
/* Immutable little-endian session: the board session (workload 0 or 4)
 * followed by the thermal owner's facts. The evidence hashes identify
 * externally reviewed records; they do not establish those facts. */
struct os_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes;
    uint64_t thermal_owner,thermal_authority;
    uint8_t thermal_initial[32],thermal_electrical[32],thermal_clocks[32],thermal_supervision[32];
    uint32_t apb1,apb1_hz;
    uint8_t configure_adc_clock,alarm_mask,critical_mask,reserved0;
    uint16_t limits[5][3]; /* hot, hysteresis, critical hardware codes */
    uint8_t reserved[6];
#ifdef SUNSTONE_A733_OS_AUDIO
    struct os_audio_session audio;
#endif
};
_Static_assert(sizeof(struct os_session)==OS_SESSION_BYTES,"OS session geometry");
#define OS_SESSION_VERSION 1u
int os_session_valid(const struct os_session *);
/* The admitted complete session after EL3 sealing; NULL before. */
const struct os_session *os_session(void);
/* The sealed session when it extends exactly the board prefix that main.c
 * admitted and names this image; NULL otherwise. Each entry starts from it. */
const struct os_session *os_session_admitted(void);

/* Fixed start list entries; each runs at the board hook of the same name. */
struct a733_pin_handoff;
void os_thermal_task(void *);
int os_thermal_task_privileged(void (*)(void *),void *);
int os_thermal_pins(struct a733_pin_handoff *);
int os_thermal_start(void);
/* IRQ context, from the fixed interrupt table on CPU0. */
void os_thermal_irq(void);
#ifdef SUNSTONE_A733_OS_AUDIO
void os_audio_task(void *);
int os_audio_task_privileged(void (*)(void *),void *);
int os_audio_pins(struct a733_pin_handoff *);
int os_audio_start(void);
void os_audio_irq(void);
/* EL0-safe; the recognizer task only. Requests the capture for recognition
 * generation g, which also releases the buffer of every earlier generation.
 * Returns 1 with the complete 16 kHz mono WAV of generation g, 0 while it is
 * being captured and -1 once audio or the board has failed. */
int os_audio_capture(unsigned g,const uint8_t **wav,size_t *bytes);
#endif

/* Resident services after the board's own (apps/board/application.h). The
 * mimus board application reports as the first of them. */
enum { OS_SERVICE_MIMUS=5, OS_SERVICE_THERMAL, OS_SERVICES };
struct os_service { const char *name; uint32_t window_us; };
extern const struct os_service os_services[OS_SERVICES];
/* Interval between watchdog renewals while every service is in its window. */
#define OS_RENEW_US 1000000u
/* 1 when every service reported progress no more than its window before
 * now_us (counter microseconds); 0 when any record is older than its window;
 * -1 when no record is stale but some service has not yet reported. */
int os_services_healthy(uint64_t now_us);
/* Resident WDT0 renewal from the supervising task (watchdog.c); 1 renewed. */
int os_watchdog_reload(void *supervisor);
#endif
