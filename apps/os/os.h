#ifndef SUNSTONE_OS_H
#define SUNSTONE_OS_H
/* Integrated OS image (docs/os-image.md): the board composition plus a
 * continuous thermal service, optional I2S microphone receive, an optional
 * radio station, an optional Bluetooth headset, one fixed interrupt table and
 * one fixed start list. */
#include "a733_board_application.h"
#include "sunstone_app.h"
#include "sunstone_session.h"
/* EL0-safe load hint. True before thermal policy initialization and while
 * throttled or failed. It grants no clock or voltage change. */
int os_thermal_throttled(void);
/* Workloads 0 and 4 require storage, 2 is the storage-free recorded mode, and the radio has two: 1 the full
 * station with USB storage and 3 the scanning station without the USB host. The session decode ties the workload
 * to the device grant (apps/os/session.c). */
static inline int os_workload_valid(unsigned workload)
{
    return workload==0||workload==1||workload==2||workload==3||workload==4;
}
/* A radio workload grants the radio station. */
static inline int os_radio_workload(unsigned workload)
{
    return workload==1||workload==3;
}
/* The radio workload this image's station serves: the scanning image 3, the full station 1. */
#ifdef SUNSTONE_A733_OS_RADIO_SCAN
#define OS_RADIO_WORKLOAD 3u
#else
#define OS_RADIO_WORKLOAD 1u
#endif
/* Service records of the typed session (docs/designs/app-abi.md). The wire form carries a record only for a
 * service the session grants; the decoded session holds every record and a mask of the granted ones. */
#include "npu-session.h"
#include "npu.h"
#include "a733_dma_platform.h"
#include "a733_dma_startup.h"
#include "a733_cpu_clock.h"
#define OS_AUDIO_SESSION_MAGIC UINT64_C(0x415337334f415531)
#define OS_AUDIO_SESSION_VERSION 1u
#define OS_AUDIO_SESSION_BYTES 184u
/* I2S0 receive facts after the thermal facts. The DMA fields are the I2S
 * image's DMA0 handoff (struct a733_dma_handoff, which audio.c builds from
 * them) and follow its rules (docs/i2s-application.md). The evidence hash
 * identifies the reviewed microphone wiring, bank voltage, audio clock, FIFO
 * route, DMA prior-drain and remote-exclusion records. */
struct os_audio_session {
    uint64_t magic,version,bytes;
    uint8_t evidence[32];
    struct a733_dma_platform_config dma;
    uint64_t dma_controller,dma_evidence;
    uint32_t dma_established;
    uint16_t dma_owned_channels,dma_service_channels;
    /* capture_samples belongs to the capture client: admission passes it to
     * os_audio_client_admit and the image composition to the client. */
    uint32_t audio_cold,capture_samples;
    uint8_t inherited[3][3]; /* PB5 BCLK, PB6 LRCK, PB8 DIN: function, pull, drive */
    uint8_t reserved[7];
};
/* The handoff's established facts follow the lifetime: in the supervised
 * image manual power removal owns recovery (A733_DMA_HANDOFF_SUPERVISED); in
 * the resident image the watchdog reset does (A733_DMA_HANDOFF_REQUIRED). */
#ifdef SUNSTONE_BOARD_RESIDENT
#define OS_AUDIO_DMA_ESTABLISHED UINT32_C(0x3f)
#else
#define OS_AUDIO_DMA_ESTABLISHED UINT32_C(0x5f)
#endif
_Static_assert(sizeof(struct os_audio_session)==OS_AUDIO_SESSION_BYTES&&
    offsetof(struct os_audio_session,dma)==56&&offsetof(struct os_audio_session,audio_cold)==160,
    "OS audio session geometry");
/* The capture client's admission of its session parameter, bound by the
 * image composition (start.c). */
int os_audio_client_admit(uint32_t capture_samples);
#define OS_DMA_SESSION_MAGIC UINT64_C(0x41533733444d4131)
#define OS_DMA_SESSION_BYTES 160u
#define OS_DMA_SESSION_VERSION 1u
struct os_dma_session {
    uint64_t magic, version, bytes;
    uint8_t evidence[32];
    struct a733_dma_handoff handoff;
};
_Static_assert(sizeof(struct os_dma_session)==OS_DMA_SESSION_BYTES,"DMA service record");
/* The CPU clock grant record is 96 bytes on the wire (include/a733_cpu_clock.h). With A76 admission, the decoded grant
 * names this boot's epoch. The loader requires a zero persisted grant epoch; OS admission stamps its private copy
 * after the loader stamps the board epoch. */
#define OS_CPU_CLOCK_SESSION_BYTES 96u
/* The services, their grant bits (OS_GRANT_*), whether this image links each one (OS_LINKED_*), their records and
 * their supervised ids (OS_SERVICE_*) come from one declaration (apps/os/os-services.h). The granted set is decided once
 * at admission from the device grant, the board prefix and the service extents that validated. A service that is not
 * granted touches no hardware. */
#include "os-services.h"
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
    uint8_t reserved[2];
    /* Zero keeps the vendor trip; otherwise a lower CPU-only CRITICAL trip. */
    uint32_t cpu_critical_millicelsius;
    struct os_npu_session npu;
    struct os_audio_session audio;
    /* The CPU clock driver's grant record (include/a733_cpu_clock.h). */
    struct a733_cpu_clock_grant_record cpu_clock;
    struct os_dma_session dma;
    uint32_t granted;  /* OS_GRANT_* */
};
_Static_assert(offsetof(struct os_session,cpu_critical_millicelsius)==804&&
    offsetof(struct os_session,npu)==A733_OS_SESSION_BYTES,"OS session geometry");
/* Each declared record has its field, of its wire size. */
#define OS_RECORD_SIZE_(GRANT,extent,field,bytes,magic,version,need) \
    _Static_assert(sizeof(((struct os_session *)0)->field)==(bytes),"the " #field " record differs from its declaration");
OS_RECORD_ROWS(OS_RECORD_SIZE_)
/* The bytes the thermal record names on the wire: the board prefix and the service records the session carries. */
size_t os_session_wire_bytes(unsigned granted);
/* The row (from 1) of the refused composition table that the granted set matches, or 0 (apps/os/admission.c). */
unsigned os_composition_refused(unsigned granted);
/* ORs the cores of this many workers of a main task on main_core into *mask; 0 when the manifest asks for more than its
 * cluster order holds (apps/os/grants.c). */
int os_worker_cores(unsigned main_core,unsigned workers,uint32_t *mask);
const char *os_composition_reason(unsigned row);
#define OS_SESSION_VERSION 1u
int os_session_valid(const struct os_session *);
/* The admitted complete session after EL3 sealing; NULL before. */
const struct os_session *os_session(void);
/* The sealed session when it extends exactly the board prefix that main.c
 * admitted and names this image; NULL otherwise. Each entry starts from it. */
const struct os_session *os_session_admitted(void);
/* The full validation behind os_session_admitted; called until it succeeds, then never again (apps/os/start.c). */
const struct os_session *os_session_validated(void);
/* 1 when the admitted session grants every service in the OS_GRANT_* mask; 0 before admission. EL0-safe: after the first
 * admission it is one load of a word that holds the granted set, and it touches no hardware. */
int os_granted(unsigned mask);

/* Fixed start list entries; each runs at the board hook of the same name. */
struct a733_pin_handoff;
void os_thermal_task(void *);
int os_thermal_task_privileged(void (*)(void *),void *);
int os_thermal_pins(struct a733_pin_handoff *);
int os_thermal_start(void);
#if defined(SUNSTONE_A733_OS_GPU) || defined(SUNSTONE_A733_OS_GPU_LINEAR)
struct tskTaskControlBlock;
/* Published before the GPU and gpu-linear startups; immutable after task creation. */
struct tskTaskControlBlock *os_thermal_task_handle(void);
#endif
/* IRQ context, from the fixed interrupt table on CPU0. */
void os_thermal_irq(void);
/* Before the scheduler, after the thermal mappings: reads the CPU clock
 * configuration once (apps/os/cpu.c); 0 when CPU-B is not on its locked PLL
 * with a decodable rate. Run only when os_cpu_boot_point_needed says so. */
int os_cpu_admit(void);
int os_cpu_boot_point_needed(void);
int os_cpu_boot_point_ok(void);
/* Thermal task, after each thermal step once the policy has its first
 * reading: one bounded CPU clock step. -1 ends the session; 0 when the session
 * does not grant the CPU clock. */
struct a733_thermal_policy;
int os_cpu_clock_step(const struct a733_thermal_policy *);
/* Thermal task: 1 when the next owner pass is due within a tick (apps/os/cpu.c). */
int os_cpu_clock_busy(void);
/* EL0-safe: 1 once the grant's requests have finished, whatever their result, and in a session without a grant. */
int os_cpu_clock_ready(void);
#ifdef SUNSTONE_A733_OS_DMA
int os_dma_task_privileged(void (*)(void *),void *);
int os_dma_pins(struct a733_pin_handoff *);
int os_dma_start(void);
void os_dma_irq(void);
void os_dma0_irq(void);
int os_dma_return_ready(void);
#endif
/* The capture stream (audio-stream.h); its start entry is
 * os_audio_stream_open, which the composition calls with the client's
 * configuration. */
#include "audio-stream.h"
void os_audio_task(void *);
int os_audio_task_privileged(void (*)(void *),void *);
int os_audio_pins(struct a733_pin_handoff *);
void os_audio_irq(void);
/* The board starts the radio owner and network client for workload 1; this
 * entry refuses radio and crypto engine owners that collide with the image's. */
int os_radio_task_privileged(void (*)(void *),void *);
int os_radio_pins(struct a733_pin_handoff *);
int os_radio_start(void);
/* The radio profile that os_radio_pins authenticated; NULL before. */
struct board_radio_profile;
const struct board_radio_profile *os_radio_profile(void);
#ifdef SUNSTONE_A733_OS_RADIO_SCAN
/* The scanning radio station (apps/os/radio_scan.c). The radio owner task calls
 * os_radio_scan_step after every board_radio_poll with its result. Once the return has
 * begun, the owner shuts the module down (board_radio_shutdown), publishes the encoded
 * result and a descriptor (include/os_radio_result.h), and os_radio_scan_return_ready
 * reports 1. The CPU return waits for it before it stops the PMIC service. */
struct board_radio;
void os_radio_scan_step(struct board_radio *,int result);
/* The entropy stage's steps (apps/os/ce_probe.c), driven by os_radio_scan_step when the profile's stage limit is the
 * entropy stage: start once, step once per pass, done once the record is published. */
void os_ce_probe_start(void);
void os_ce_probe_step(void);
int os_ce_probe_done(void);
int os_ce_probe_failed(void);
/* The association stage's unqualified CE entropy (apps/os/ce_probe.c). */
void os_ce_entropy_begin(void);
void os_ce_entropy_step(void);
int os_ce_entropy_ready(void);
int os_ce_entropy_failed(void);
int os_ce_entropy_bytes(void *,uint8_t *,size_t);
void os_ce_entropy_wipe(void);
void os_radio_scan_return_request(void);
int os_radio_scan_return_ready(void);
#endif
/* The headset service (apps/os/headset.c, with the Bluetooth stack and the
 * mSBC codec) is an object library that an image links only when its
 * application's manifest declares the audio-headset device. The OS core
 * reaches it through weak references: in an image without it, a session that
 * grants the headset device is refused (apps/os/session.c).
 *
 * The Bluetooth profile address. The radio profile occupies 0x43000000 and
 * the Bluetooth profile follows it in the first 64 KiB; the firmware spans
 * from 0x43010000 serve both. */
#define OS_HEADSET_PROFILE_ADDRESS UINT64_C(0x43008000)
/* Repeated capture modes: CVSD (2) and wideband mSBC with CVSD fallback (3). */
static inline int os_headset_mode_valid(unsigned mode)
{
    return mode==2||mode==3;
}
/* The Bluetooth profile that EL3 copied from OS_HEADSET_PROFILE_ADDRESS and
 * published (os_headset_identity, called by identity.c for a session that
 * grants the headset); NULL before. Valid for the OS workloads and the
 * repeated capture modes only, with owners distinct from the board's. */
struct bluetooth_profile;
const struct bluetooth_profile *os_headset_profile(void);
int os_headset_profile_valid(const struct a733_board_session *,const struct bluetooth_profile *);
/* EL3, before CPU mappings take effect: copy and validate the profile, and
 * publish the copy and the 1 MiB profile span. 1 on success. */
int os_headset_identity(const struct a733_board_session *,uint64_t deadline);
/* The capture length of the admitted profile in 16 kHz frames. 1 on success. */
int os_headset_capture_frames(uint32_t *frames);
void os_headset_task(void *);
int os_headset_task_privileged(void (*)(void *),void *);
int os_headset_pins(struct a733_pin_handoff *);
int os_headset_start(void);
/* Internal EL0-safe headset buffer interface: the 16 kHz WAV of the
 * headset segment recorded for generation g, which the headset button starts
 * (and, in workload 4, the request itself). */
int os_headset_capture(unsigned g,const uint8_t **wav,size_t *bytes);
/* When the session grants the radio, one module owner serves Wi-Fi and
 * Bluetooth: the board's radio owner task calls this after each radio poll
 * (apps/os/main.patch). Otherwise the headset runs its own owner task. */
struct board_radio;
struct board_radio *os_board_radio(void);
void os_module_poll(void);
#ifdef SUNSTONE_A733_OS_GPU
#define OS_GPU_CLIENT 7u
#define OS_GPU_FIRMWARE_OWNER UINT64_C(0x4f53475055464d31)
#define OS_GPU_SESSION_OWNER UINT32_C(0x4f534731)
#define OS_STOP_DETAIL_GPU_LOSS UINT64_C(0x4750554c)
enum { OS_GPU_CALL_LOST=-1,OS_GPU_CALL_TIMEOUT=-2 };
int os_gpu_queue_task_privileged(void (*)(void *),void *);
int os_gpu_queue_pins(struct a733_pin_handoff *);
int os_gpu_queue_start_entry(void);
void os_gpu_queue_irq(void);
unsigned os_gpu_queue_state(void);
int os_gpu_queue_thermal(void);
int os_gpu_queue_retire(void);
int os_gpu_retire(void);
int os_gpu_return_ready(void);
int os_gpu_return_started(void);
#endif
/* The recognizer's capture source: the I2S microphone or the headset. */
int os_capture(unsigned g,const uint8_t **wav,size_t *bytes);

/* Resident services after the board's own (apps/board/application.h): the linked application reports as the first of them,
 * then the services of OS_SUPERVISED_ROWS (apps/os/os-services.h), whose ids OS_SERVICE_* are defined there. The
 * supervisor requires progress only from the services the session grants (apps/os/services.c). */
struct os_service { const char *name; uint32_t window_us; };
extern const struct os_service os_services[OS_SERVICES];
void board_service_progress(unsigned);
/* Bind this service once to its owning task and fixed deadline slot. A fresh
 * wait certificate supports a separately bounded operation health deadline. */
int board_service_deadline_register(unsigned);
/* Declare the next wait's operation budget, within the admitted session.
 * An alarm may mature normally; the owner must then check the device and
 * report real progress within its service window and this absolute budget. */
int board_service_blocked_until(unsigned,uint64_t);
void os_usb0_notify(void);
int os_application_finished(void);
int os_session_decode(const void *,size_t,struct os_session *);
/* As os_session_decode; a refusal stores the A733_DECODE_* sub-term (include/a733_admission_refusal.h) in *why. */
int os_session_decode_why(const void *,size_t,struct os_session *,unsigned *why);
const void *os_session_extent(const void *,uint32_t,size_t *);
uint32_t os_session_devices(void);
const struct ss_input_fact *os_session_inputs(unsigned *);
/* Interval between watchdog renewals while every service is in its window. */
#define OS_RENEW_US 1000000u
/* 1 when every service has fresh progress or a valid bounded wait certificate;
 * 0 on expired/failed/stale state; -1 before every required owner has reported.
 * now_us uses the architectural counter's microsecond epoch. */
int os_services_healthy(uint64_t now_us);
/* Resident WDT0 renewal from the supervising task (watchdog.c); 1 renewed. */
int os_watchdog_reload(void *supervisor);
#endif
