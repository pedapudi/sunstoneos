/* Bluetooth headset capture for the integrated OS image (docs/os-image.md).
 * The Bluetooth application (apps/bluetooth) runs on the board's shared
 * providers: UART1 H4/HCI, secure pairing, the hands-free audio gateway and
 * CVSD or wideband mSBC audio, over SCO or eSCO. Its repeated profile (mode 2
 * for CVSD, mode 3 for mSBC with CVSD fallback) publishes fixed-length
 * segments into two slots. This file, the Bluetooth stack and the mSBC codec
 * are one object library, linked into an image only when its application
 * declares the audio-headset device; every entry does nothing for a session
 * that does not grant it.
 *
 * When the session does not grant the radio (workloads 0 and 4), the
 * application owns the AIC8800 module: one Bluetooth owner task runs its
 * firmware bootstrap over SDIO and its HCI, as in the Bluetooth image. When it
 * grants the radio (workload 1), the board's radio owner owns the module and
 * loads the firmware once; the application attaches to that module as a
 * second client and the radio owner task services it after radio work
 * (os_module_poll). The Bluetooth client then owns only UART1, H4/HCI and its
 * upper layers, with its own recovery latch: its failure leaves the module and
 * Wi-Fi to the radio, and a module failure ends the Bluetooth client.
 *
 * One capture client task is the segment consumer. The headset records one
 * segment per voice-recognition activation, and the client requests one only
 * while a recognition generation waits for audio: in workload 0 it admits the
 * headset button (ACCEPT); in workload 4 each new generation, started by the
 * Enter key, starts recording at once (START), and if that activation ends
 * without a segment, as after a headset cancel, the button records it
 * instead. The next published segment is converted from the borrowed slot to
 * a 16 kHz WAV for the newest generation (apps/os/recognition.patch) and
 * acknowledged. A lost packet stops capture, and its stop flag, like any
 * stop but the supervised capture window, ends the session through
 * board_fail, as do a refused take, a failed Bluetooth owner and a failed
 * board. During a return the original owner stops the protocol, waits for
 * authenticated module reset, and drains UART. Uncertain resources stay
 * retained and the supervisor records the owner's fault. */
#include "os.h"
#include "cpu-return.h"
#include "quiesce.h"
#include "../board/application.h"
#include "../bluetooth/bluetooth.h"
#include "../bluetooth/headset.h"
#include "../bluetooth/pcm.h"
#include "../bluetooth/msbc.h"
#include "owner-memory.h"
#include "a733_cpu_coherence.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include "sunstone_crumb.h"
#include "sunstone_deadline.h"
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
#include "gicv3_spi.h"
#include "../bluetooth/work-state.h"
#endif
#include <string.h>
/* The headset's memory, taken from the ledger at admission in a session that grants the headset (os_headset_memory): the
 * Bluetooth application, the task stacks and the WAV, followed by the library's state (bluetooth_headset_memory). The capture
 * client runs at EL0 and reaches the application and the library's state, and the recognizer reads the WAV, so the extent is
 * readable and writable by the application's tasks, as the image's .bss that held it was. app is its first member: the address
 * is null in a session that does not grant the headset, and the entries of apps/bluetooth/headset.c refuse a null application. */
struct headset_memory {
    struct bluetooth_application app;
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
    struct bluetooth_work_state work[2]; /* Owner-private comparisons; no payload or keys. */
#endif
    StaticTask_t owner_task;     /* the Bluetooth owner when the radio is not granted */
    StackType_t owner_stack[4096];
    StaticTask_t client_task;
    StackType_t client_stack[2048];
    _Alignas(4) uint8_t headroom[SS_CAPTURE_HEADROOM-44];
    uint8_t wav[BLUETOOTH_WAV_BYTES];
};
static struct headset_memory *memory;
#define app (memory->app)
/* The profile that os_headset_identity copied at EL3; its bytes field is zero until then. */
static struct bluetooth_profile profile;
static struct {
    atomic_uint latch; /* With the radio, the attached Bluetooth client's own recovery latch. */
    TaskHandle_t owner,client;
    atomic_uint failed,requested,captured;
    unsigned started; /* The newest generation that requested START. */
    size_t bytes;
} headset;
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
static struct {
    struct gicv3_spi source;
    atomic_uint failed,pending;
    atomic_uint_fast64_t since; /* Counter microseconds plus one; earliest unclassified IRQ. */
    uint64_t pre_us,budget_us,retry_at,control_deadline;
    unsigned bound,armed,masked,registered;
} headset_irq={.source={UINT32_C(0x03400000),35},.masked=1};
struct {
    uint64_t deliveries,passes,software_passes,max_classification_us,pre_irq_us,service_budget_us;
    uint64_t sync_waits,rearm_waits,retained;
} os_headset_irq_record;
static void serve_irq(void);
static uint64_t owner_next_deadline(uint64_t);
static uint64_t classification_deadline(uint64_t);
static uint64_t wait_limit(void);
static void irq_fault(unsigned);
/* The existing scheduling authority attests the complete receive bound. A
 * separate local guard reserves the source-conservative trigger/timeout
 * delay: at most 128 characters, spaced just under four character times.
 * UART1 is 8N1 with a half-full 256-byte FIFO; polling mode has no such guard. */
static int irq_profile(const struct bluetooth_profile *p,uint64_t *pre)
{
    struct a733_uart_plan plan;
    if(!p||p->uart.deferred_irq!=1||p->uart.format.port!=1||p->uart.format.data_bits!=8||
       p->uart.format.stop_half_bits!=2||p->uart.format.parity||
       a733_uart_plan(&p->uart.format,&plan)!=1||plan.fcr!=0x81u||!plan.actual_baud)return 0;
    uint64_t numerator=UINT64_C(4)*(plan.fifo_bytes/2u)*10u*1000000u;
    uint64_t allowance=(numerator+plan.actual_baud-1u)/plan.actual_baud;
    if(!allowance||p->headset.receive_latency_us<=allowance)return 0;
    if(pre)*pre=allowance;
    return 1;
}
void os_headset_uart_irq(void)
{
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    headset_irq.masked=1;
    if(!gicv3_spi_mask(&headset_irq.source)||!headset_irq.bound||!headset_irq.armed)
        atomic_store_explicit(&headset_irq.failed,1,memory_order_release);
    uint64_t at=a733_smp_counter()/24+1;
    if(!atomic_load_explicit(&headset_irq.since,memory_order_relaxed))
        atomic_store_explicit(&headset_irq.since,at,memory_order_release);
    atomic_store_explicit(&headset_irq.pending,1,memory_order_release);
    ++os_headset_irq_record.deliveries;
    BaseType_t woken=pdFALSE;
    if(headset.owner)vTaskNotifyGiveFromISR(headset.owner,&woken);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    configASSERT(headset_irq.bound&&headset.owner); /* Unowned delivery cannot be serviced safely. */
    portYIELD_FROM_ISR(woken);
}
#endif
/* Optional post-return telemetry: client first, standalone Bluetooth owner
 * second. Each task writes only its own six-counter record. */
struct { uint64_t waits,wakes,idle_us,idle_notifications,idle_timeouts,deadline_expiries; }
    os_headset_wake_record[2];
static enum os_quiesce_result headset_quiesce_step(struct os_quiesce *);
static struct os_quiesce headset_quiesce={.owner=OS_QUIESCE_HEADSET,
    .order=OS_QUIESCE_DEVICES,.step=headset_quiesce_step};
static int live(void){return board_live(NULL)&&!atomic_load_explicit(&headset.failed,memory_order_acquire);}
/* The session grants the radio: the board's radio owner owns the module and the headset attaches to it. */
static int shared(void){return os_granted(OS_GRANT_RADIO);}
static int granted(void){return os_granted(OS_GRANT_HEADSET);}
int os_headset_quiesce_pending(void)
{return os_quiesce_requested(&headset_quiesce)&&os_quiesce_result(&headset_quiesce)==OS_QUIESCE_PENDING;}
static enum os_quiesce_result headset_quiesce_step(struct os_quiesce *q)
{
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
    if(headset_irq.bound&&atomic_load_explicit(&headset_irq.failed,memory_order_acquire)){
        (void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_SITE(BOARD),0x12fu,SUNSTONE_CRUMB_HERE());
        return OS_QUIESCE_FAULT;
    }
#endif
    if(os_quiesce_result(q)!=OS_QUIESCE_PENDING)return os_quiesce_result(q);
    /* The supervisor never runs protocol or device work on the wrong task. */
    if(!headset.owner)(void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_SITE(BOARD),0x100u,SUNSTONE_CRUMB_HERE());
    else xTaskNotifyGive(headset.owner);
    if(headset.client)xTaskNotifyGive(headset.client);
    return os_quiesce_result(q);
}
static void fail(void)
{
    if(!atomic_exchange_explicit(&headset.failed,1,memory_order_acq_rel))(void)bluetooth_headset_segment_stop(&app);
    if(!os_quiesce_fault(&headset_quiesce,SUNSTONE_CRUMB_FAIL_SITE(BOARD),0x102u,SUNSTONE_CRUMB_HERE()))board_fail(NULL);
}
/* The Bluetooth image's own startup rules (apps/bluetooth/application.c) for
 * the OS workloads, limited to the repeated capture modes. The board
 * session's SID evidence is the profile's SHA-256, as in the Bluetooth image. */
static int headset_workload_valid(unsigned workload)
{
    return workload==0||workload==1||workload==4;
}
int os_headset_profile_valid(const struct a733_board_session *s,const struct bluetooth_profile *p)
{
    if(!s||!p||!headset_workload_valid(s->workload)||!s->sid_authority||s->sid_authority!=p->sid_authority||
       (p->version!=3&&p->version!=4)||!os_headset_mode_valid(p->headset.mode)||!bluetooth_profile_valid(p,s->sid_evidence)||p->topology!=s->topology||
       memcmp(p->radio.fabric,s->fabric,sizeof(s->fabric))||memcmp(p->radio.fabric_hz,s->fabric_hz,sizeof(s->fabric_hz)))
        return 0;
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
    if(!irq_profile(p,NULL))return 0;
#endif
    uint64_t owners[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner,p->radio.owner,p->uart.owner};
    for(unsigned i=0;i<6;++i){if(!owners[i])return 0;for(unsigned j=0;j<i;++j)if(owners[i]==owners[j])return 0;}
    return 1;
}
/* EL3 with translation and caches disabled, for a session whose device grant names the headset (apps/os/identity.c). */
int os_headset_identity(const struct a733_board_session *s,uint64_t deadline)
{
    const volatile struct bluetooth_profile *source=(const void *)OS_HEADSET_PROFILE_ADDRESS;
    uint32_t bytes=source->bytes;
    if(bytes!=832&&bytes!=sizeof(profile))return 0;
    for(uint32_t i=0;i<bytes;++i)((uint8_t *)&profile)[i]=((const volatile uint8_t *)source)[i];
    if(profile.bytes!=bytes||!os_headset_profile_valid(s,&profile)||
       a733_cpu_coherence_publish((uintptr_t)&profile,sizeof(profile),deadline)!=1||
       a733_cpu_coherence_publish(BLUETOOTH_PROFILE_ADDRESS,0x100000,deadline)!=1){profile.bytes=0;return 0;}
    return 1;
}
const struct bluetooth_profile *os_headset_profile(void)
{return board_radio_chip_id()&&profile.bytes?&profile:NULL;}
int os_headset_capture_frames(uint32_t *frames)
{
    const struct bluetooth_profile *p=os_headset_profile();
    if(!frames||!p||p->headset.samples>UINT32_MAX/2)return 0;
    *frames=2*p->headset.samples;return 1;
}
/* Called by the Bluetooth owner when it retains its transports. */
void bluetooth_recognition_abort(void){fail();}
/* One Bluetooth owner iteration, on the module owner's task. */
static void serve(void)
{
    if(os_quiesce_result(&headset_quiesce)!=OS_QUIESCE_PENDING)return;
    if(os_quiesce_requested(&headset_quiesce)){
        int stopped=bluetooth_application_stop(&app,os_cpu_return_deadline_us());
        if(stopped<0)(void)os_quiesce_fault(&headset_quiesce,SUNSTONE_CRUMB_FAIL_SITE(BOARD),
            0x110u|(app.stop_fault<<12),SUNSTONE_CRUMB_HERE());
        else if(stopped)os_quiesce_complete(&headset_quiesce);
        if(stopped>=0)BOARD_PROGRESS(OS_SERVICE_BLUETOOTH);
        return;
    }
    if(!board_live(NULL))(void)bluetooth_application_retain(&app);
    else if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))
        (void)bluetooth_application_poll(&app);
    /* A failed owner stops reporting, so the resident watchdog resets the board. */
    if(!atomic_load_explicit(&app.failed,memory_order_acquire))BOARD_PROGRESS(OS_SERVICE_BLUETOOTH);
}
void os_module_poll(void)
{
    if(!granted())return;
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
    if(headset_irq.bound){
        if(!headset_irq.registered){
            if(!board_service_deadline_register(OS_SERVICE_BLUETOOTH)){irq_fault(0x121);return;}
            headset_irq.registered=1;
        }
        serve_irq();
        if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_PENDING){
            uint64_t limit=classification_deadline(wait_limit());
            /* The radio's next real wait uses the same task/slot sequence.
             * The pure companion below never publishes this certificate. */
            if(!limit||limit<=a733_smp_counter()/24||!board_service_blocked_until(OS_SERVICE_BLUETOOTH,limit))
                irq_fault(0x122);
        }
        return;
    }
#endif
    serve();
}
int os_headset_module_release_ready(void)
{
    if(!granted())return 1;
    if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT)return -1;
    return bluetooth_application_module_release(&app);
}
void os_headset_module_down(int proved)
{
    if(granted()){
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
        unsigned before=atomic_load_explicit(&app.stop_peer,memory_order_acquire);
#endif
        bluetooth_application_module_down(&app,proved);
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
        if(headset_irq.bound&&headset.owner&&before!=atomic_load_explicit(&app.stop_peer,memory_order_acquire))
            xTaskNotifyGive(headset.owner);
#endif
    }
}
static int wait_until(unsigned service,unsigned index,uint64_t until,uint64_t limit)
{
    if(!limit||a733_smp_counter()/24>=limit||!board_service_blocked_until(service,limit))return -1;
    uint64_t began=a733_smp_counter()/24;
    ++os_headset_wake_record[index].waits;
    int r=sunstone_deadline_wait_until(until);
    ++os_headset_wake_record[index].wakes;
    if(r==SUNSTONE_DEADLINE_EXPIRED)++os_headset_wake_record[index].deadline_expiries;
    if(!index){
        os_headset_wake_record[0].idle_us+=a733_smp_counter()/24-began;
        if(r==SUNSTONE_DEADLINE_EXPIRED)++os_headset_wake_record[0].idle_timeouts;
        else if(r>=0)++os_headset_wake_record[0].idle_notifications;
    }
    return r;
}
static uint64_t wait_limit(void)
{return os_quiesce_requested(&headset_quiesce)?os_cpu_return_deadline_us():board.session.deadline_us;}
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
static int irq_mask(void)
{
    int ok=1;taskENTER_CRITICAL();
    if(!headset_irq.masked){headset_irq.masked=1;ok=gicv3_spi_mask(&headset_irq.source);}
    taskEXIT_CRITICAL();return ok;
}
static uint64_t classification_deadline(uint64_t limit)
{
    uint64_t encoded=atomic_load_explicit(&headset_irq.since,memory_order_acquire);
    if(encoded){
        uint64_t end=a733_deadline_after_saturating(encoded-1,headset_irq.budget_us);
        if(end<limit)limit=end;
    }
    if(headset_irq.control_deadline&&headset_irq.control_deadline<limit)limit=headset_irq.control_deadline;
    return limit;
}
static void irq_fault(unsigned detail)
{
    (void)irq_mask();headset_irq.armed=0;
    atomic_store_explicit(&headset_irq.failed,1,memory_order_release);
    ++os_headset_irq_record.retained;
    if(app.self==&app&&!atomic_load_explicit(&app.failed,memory_order_acquire))
        (void)bluetooth_application_retain(&app);
    if(!os_quiesce_fault(&headset_quiesce,SUNSTONE_CRUMB_FAIL_SITE(BOARD),detail,SUNSTONE_CRUMB_HERE()))fail();
    else atomic_store_explicit(&headset.failed,1,memory_order_release);
}
static void irq_retry(uint64_t now,uint64_t limit,int control)
{
    /* GIC propagation has no completion interrupt. Retry spacing comes from
     * the admitted receive budget; the earlier protocol/return bound wins. */
    uint64_t interval=(headset_irq.budget_us+7u)/8u;
    if(control&&!headset_irq.control_deadline){
        uint64_t end=a733_deadline_after_saturating(now,headset_irq.budget_us);
        headset_irq.control_deadline=end<limit?end:limit;
    }
    limit=classification_deadline(limit);
    uint64_t observed=a733_smp_counter()/24;
    uint64_t next=a733_deadline_after_saturating(observed,interval);
    headset_irq.retry_at=next<limit?next:limit;
}
static uint64_t owner_next_deadline(uint64_t until)
{
    if(!until)return 0;
    if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_DONE)return until;
    if(app.hci.transport.state==BT_H4_ACTIVE){
        until=bluetooth_application_next_deadline(&app,until);if(!until)return 0;
    }
    if(headset_irq.retry_at&&headset_irq.retry_at<until)until=headset_irq.retry_at;
    return classification_deadline(until);
}
uint64_t os_module_next_deadline(uint64_t until)
{
    if(!granted()||!shared()||!memory)return until;
    return headset_irq.bound?owner_next_deadline(until):until;
}
static int irq_rearm(uint64_t now,uint64_t limit)
{
    int result=1;taskENTER_CRITICAL();
    if(headset_irq.armed&&!atomic_load_explicit(&headset_irq.failed,memory_order_acquire)){
        if(atomic_load_explicit(&headset_irq.pending,memory_order_acquire))result=0;
        else if(headset_irq.masked){
            result=gicv3_spi_rearm(&headset_irq.source);
            if(result==1)headset_irq.masked=0;
        }
    }
    taskEXIT_CRITICAL();
    /* GIC propagation has no deadline-aware accessor. A successful write
     * must still finish within the bound inherited by this attempt. */
    if(a733_smp_counter()/24>=limit)return -1;
    if(!result){++os_headset_irq_record.rearm_waits;irq_retry(now,limit,1);}
    else if(result>0)headset_irq.control_deadline=0;
    return result;
}
static void serve_irq(void)
{
    if(os_quiesce_result(&headset_quiesce)!=OS_QUIESCE_PENDING)return;
    uint64_t now=a733_smp_counter()/24,limit=classification_deadline(wait_limit());
    headset_irq.retry_at=0;
    if(!irq_mask()||atomic_load_explicit(&headset_irq.failed,memory_order_acquire)||!limit||now>=limit){irq_fault(0x123);return;}
    struct a733_uart *uart=a733_uart_platform_controller(&app.uart);
    if(uart){
        enum a733_uart_platform_result sync=a733_uart_platform_irq_sync(&app.uart,limit);
        if(sync==A733_UART_PLATFORM_BUSY||sync==A733_UART_PLATFORM_PENDING){
            ++os_headset_irq_record.sync_waits;irq_retry(now,limit,1);return;
        }
        if(sync!=A733_UART_PLATFORM_READY){irq_fault(0x124);return;}
    }
    atomic_store_explicit(&headset_irq.pending,0,memory_order_release);
    int before=bluetooth_application_work_state(&app,&memory->work[0]);
    serve();++os_headset_irq_record.passes;
    now=a733_smp_counter()/24;
    if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT){headset_irq.armed=0;return;}
    if(atomic_load_explicit(&app.failed,memory_order_acquire)||now>=classification_deadline(wait_limit())){irq_fault(0x125);return;}
    if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_DONE){
        headset_irq.armed=0;atomic_store_explicit(&headset_irq.since,0,memory_order_release);return;
    }
    if(app.hci.transport.state!=BT_H4_ACTIVE){
        /* The module's pre-H4 SDIO bootstrap still exposes bounded polls.
         * This is an intermediate path, separate from UART event service. */
        headset_irq.retry_at=a733_deadline_after_saturating(now,1000);return;
    }
    uart=a733_uart_platform_controller(&app.uart);
    if(!uart){irq_retry(now,limit,0);return;}
    int unclassified=bluetooth_application_rx_unclassified(&app);
    int empty=a733_uart_rx_empty(uart,classification_deadline(wait_limit()));
    if(unclassified<0||empty<0){irq_fault(0x126);return;}
    uint64_t encoded=atomic_load_explicit(&headset_irq.since,memory_order_acquire);
    if(encoded&&empty&&!unclassified){
        uint64_t elapsed=now-(encoded-1);
        if(elapsed>os_headset_irq_record.max_classification_us)os_headset_irq_record.max_classification_us=elapsed;
        atomic_store_explicit(&headset_irq.since,0,memory_order_release);
    }
    headset_irq.armed=bt_h4_irq_ready(&app.hci.transport)!=0;
    if(irq_rearm(now,classification_deadline(wait_limit()))<0){irq_fault(0x127);return;}
    int after=bluetooth_application_work_state(&app,&memory->work[1]);
    if(!before||!after){irq_fault(0x128);return;}
    if(memcmp(&memory->work[0],&memory->work[1],sizeof(memory->work[0]))&&
       !(headset_irq.armed&&!headset_irq.masked&&(uart->ier&2u)&&
         app.hci.transport.tx_offset<app.hci.transport.tx_length)){
        ++os_headset_irq_record.software_passes;xTaskNotifyGive(headset.owner);
    }
    /* After HCI retirement, UART TEMT and local reset holds have no completion
     * IRQ. The existing bounded close advances by precise observations. */
    if(os_quiesce_requested(&headset_quiesce)&&app.hci.state==BT_HCI_STOPPED)
        irq_retry(now,classification_deadline(wait_limit()),0);
}
#else
uint64_t os_module_next_deadline(uint64_t until){return until;}
#endif
static void owner(void *unused)
{
    (void)unused;
    if(!board_service_deadline_register(OS_SERVICE_BLUETOOTH)){fail();vTaskSuspend(NULL);return;}
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
    if(headset_irq.bound){
        headset_irq.registered=1;
        for(;;){
            serve_irq();
            if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT||atomic_load_explicit(&headset.failed,memory_order_acquire)){
                vTaskSuspend(NULL);return;
            }
            uint64_t limit=wait_limit(),until=owner_next_deadline(limit);
            if(!until||wait_until(OS_SERVICE_BLUETOOTH,1,until,classification_deadline(limit))<0){irq_fault(0x129);vTaskSuspend(NULL);return;}
        }
    }
#endif
    uint64_t next=a733_smp_counter()/24;
    for(;;){
        serve();
        if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT){vTaskSuspend(NULL);return;}
        uint64_t now=a733_smp_counter()/24,limit=wait_limit();
        /* This H4 composition uses the UART platform's polling profile. It
         * has no admitted ISR or rearm path. Preserve its 1 ms service cadence
         * with an absolute compare until that transport integration exists. */
        if(now>=next){
            uint64_t slots=(now-next)/1000+1;
            next=slots>(UINT64_MAX-next)/1000?UINT64_MAX:next+slots*1000;
        }
        uint64_t until=next<limit?next:limit;
        if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_DONE)until=limit;
        if(wait_until(OS_SERVICE_BLUETOOTH,1,until,limit)<0){fail();vTaskSuspend(NULL);return;}
    }
}
/* Client task only: one bounded step. 0 continue, -1 failed. */
static int step(void)
{
    if(os_quiesce_requested(&headset_quiesce))return 0;
    struct bluetooth_segment_status status;
    if(bluetooth_headset_segment_status(&app,&status)!=1)return -1;
    /* Capture stopped for any reason but the supervised window would leave
     * later generations without a capture. */
    if(status.flags&~BLUETOOTH_SEGMENT_WINDOW)return -1;
    unsigned g=atomic_load_explicit(&headset.requested,memory_order_acquire);
    struct bluetooth_segment_snapshot segment;const uint8_t *pcm;
    int r=bluetooth_headset_segment_take(&app,&pcm,&segment);
    if(r<0)return -1;
    if(r){
        const struct bluetooth_pcm_snapshot *c=&segment.capture;
        /* State 1 is CVSD at 8 kHz; state 2 is mSBC at 16 kHz. */
        if(c->state!=1&&c->state!=2)return -1;
        /* Requests are made only while a generation waits, so the segment is
         * the newest one's; the WAV of a generation already captured, which
         * the recognizer may be reading, is never rewritten. */
        unsigned wanted=g&&atomic_load_explicit(&headset.captured,memory_order_relaxed)!=g;
        /* mSBC segments are already 16 kHz; CVSD segments are interpolated. */
        if((wanted&&(c->state==2?bluetooth_pcm16_wav(pcm,c->bytes,memory->wav,sizeof(memory->wav),&headset.bytes):
            bluetooth_pcm_wav(pcm,c->bytes,memory->wav,sizeof(memory->wav),&headset.bytes))!=1)||
           bluetooth_headset_segment_ack(&app,segment.ticket)!=1)return -1;
        if(wanted)atomic_store_explicit(&headset.captured,g,memory_order_release);
        return 0;
    }
    if(!g||atomic_load_explicit(&headset.captured,memory_order_relaxed)==g)return 0;
    /* A new keyboard generation upgrades the word to START once; otherwise
     * the client admits the headset button whenever the word is 0. With no
     * segment taken, status.published is the head that take saw, so the
     * request is refused if an activation published after it. START counts
     * only once the word holds it: an activation that was running instead
     * may still end without a segment. */
    unsigned start=board.session.workload==4&&headset.started!=g;
    int requested=bluetooth_headset_segment_request(&app,start?BLUETOOTH_SEGMENT_START:BLUETOOTH_SEGMENT_ACCEPT,status.published);
    if(requested<0)return -1;
    if(start&&requested)headset.started=g;
    return 0;
}
void os_headset_task(void *unused)
{
    (void)unused;
    if(!board_service_deadline_register(OS_SERVICE_HEADSET)){fail();vTaskSuspend(NULL);return;}
    for(;;){
        /* A failed capture keeps its buffers, stops and stops reporting. */
        if(os_quiesce_result(&headset_quiesce)!=OS_QUIESCE_FAULT){
            if(!atomic_load_explicit(&headset.failed,memory_order_acquire)&&(!live()||step()<0))fail();
            if(!atomic_load_explicit(&headset.failed,memory_order_acquire))BOARD_PROGRESS(OS_SERVICE_HEADSET);
        }
        if(os_quiesce_result(&headset_quiesce)==OS_QUIESCE_FAULT){vTaskSuspend(NULL);return;}
        /* Segment publication and a new generation both notify this task.
         * An admitted deadline certificate covers its idle progress window. */
        uint64_t limit=wait_limit();
        if(wait_until(OS_SERVICE_HEADSET,0,limit,limit)!=SUNSTONE_DEADLINE_NOTIFIED){fail();vTaskSuspend(NULL);return;}
    }
}
int os_headset_capture(unsigned g,const uint8_t **wav,size_t *bytes)
{
    if(!live()||os_quiesce_requested(&headset_quiesce)||!g||!wav||!bytes)return -1;
    unsigned requested=atomic_load_explicit(&headset.requested,memory_order_relaxed);
    if(g<requested)return -1;
    if(g>requested){atomic_store_explicit(&headset.requested,g,memory_order_release);if(headset.client)xTaskNotifyGive(headset.client);}
    if(atomic_load_explicit(&headset.captured,memory_order_acquire)!=g)return 0;
    *wav=memory->wav;*bytes=headset.bytes;return live()?1:-1;
}
/* The Bluetooth owner drives the module, UART and pins; the client only
 * copies segments and runs unprivileged. With the radio, the board's radio
 * owner is the Bluetooth owner, and main.c already runs it privileged; the
 * headset then creates no owner task. */
int os_headset_task_privileged(void (*entry)(void *),void *context)
{
    return !context&&entry==owner&&!shared();
}
static int same_latch(const struct a733_pin_latch_handoff *a,const struct a733_pin_latch_handoff *b)
{return a->identity==b->identity&&a->owner==b->owner&&a->known_mask==b->known_mask&&a->value==b->value;}
/* PG0..PG9 and PM0, PM1, PM3 and PM4 join the board pin manager's authority,
 * with the profile's PM latch, as in the Bluetooth image. With the radio, the
 * board already holds PG0..PG5, PM0, PM1 and the session's PM latch for the
 * module; the profile must name that latch and the radio's platform and
 * session owners, and adds UART1 (PG6..PG9) and the wake pins PM3 and PM4. */
int os_headset_pins(struct a733_pin_handoff *pins)
{
    const struct os_session *s=os_session_admitted();
    if(s&&!granted())return 1; /* no headset session: the pins stay unread */
    const struct bluetooth_profile *p=os_headset_profile();
    if(!pins||!s||!p||!os_headset_profile_valid(&s->board,p)||p->radio.owner==s->thermal_owner||
       p->uart.owner==s->thermal_owner)return 0;
    if(shared()){
        const struct board_radio_profile *r=os_radio_profile();
        if(!r||pins->authorized[A733_PIN_G]!=0x3f||pins->authorized[A733_PIN_M]!=3||!same_latch(&pins->latch[1],&p->pm_latch)||
           p->radio.owner!=r->platform.owner||p->session_owner!=r->session_owner||p->uart.owner==r->ce.owner)return 0;
    }else if(pins->authorized[A733_PIN_G]||pins->authorized[A733_PIN_M])return 0;
    static const struct a733_mapping_rule wb={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!a733_mapping_check_pages(aarch64_device_translate,BLUETOOTH_PROFILE_ADDRESS,BLUETOOTH_PROFILE_ADDRESS,
           UINT64_C(0x43100000)-BLUETOOTH_PROFILE_ADDRESS,&wb))return 0;
    pins->authorized[A733_PIN_G]=0x3ff;pins->authorized[A733_PIN_M]=p->version==4?0x1f:0x1b;pins->latch[1]=p->pm_latch;
    return 1;
}
int os_headset_memory(void)
{
    if(!granted()||memory)return 1;
    size_t own=(sizeof(struct headset_memory)+63)&~(size_t)63;
    uint8_t *m=os_owner_memory(own+bluetooth_headset_state_bytes,64,OS_MEMORY_EL0_WRITE);
    if(!m||bluetooth_headset_memory(m+own)!=1)return 0;
    memory=(struct headset_memory *)(void *)m;return 1;
}
int os_headset_start(void)
{
    const struct os_session *s=os_session_admitted();
    if(s&&!granted())return 1;
    const struct bluetooth_profile *p=os_headset_profile();
    if(!s||!p||!memory||headset.client||!board_live(NULL))return 0;
#ifdef SUNSTONE_A733_OS_HEADSET_IRQ
    if(!irq_profile(p,&headset_irq.pre_us))return 0;
    headset_irq.budget_us=p->headset.receive_latency_us-headset_irq.pre_us;
    os_headset_irq_record.pre_irq_us=headset_irq.pre_us;
    os_headset_irq_record.service_budget_us=headset_irq.budget_us;
    headset_irq.bound=1;
    if(!gicv3_spi_mask(&headset_irq.source)){atomic_store(&headset_irq.failed,1);return 0;}
#endif
    /* The recognizer's start entry ran runtime_init, so libsbc can allocate. */
    if(p->headset.mode==3&&bluetooth_msbc_init()!=1)return 0;
    if(!os_quiesce_register(&headset_quiesce))return 0;
    headset.client=xTaskCreateStatic(os_headset_task,"os-headset",2048,NULL,4,memory->client_stack,&memory->client_task);
    if(!headset.client)return 0;
    if(shared()){
        /* main.c built the radio and created its owner (handle 5) before this list. */
        struct board_radio *radio=os_board_radio();
        headset.owner=board.handles[5];
        atomic_init(&headset.latch,0);
        if(!radio||!headset.owner||bluetooth_application_attach(&app,p,&providers.shared,board_radio_chip_id(),headset.owner,
               &headset.latch,board.session.session_start_ticks/24,board.session.deadline_us,board.session.sid_evidence,
               &radio->native,&radio->session)!=1)return 0;
    }else{
        headset.owner=xTaskCreateStatic(owner,"bluetooth",4096,NULL,4,memory->owner_stack,&memory->owner_task);
        if(!headset.owner||bluetooth_application_init(&app,p,&providers.shared,board_radio_chip_id(),headset.owner,
               &board.watchdog_state,board.session.session_start_ticks/24,board.session.deadline_us,board.session.sid_evidence)!=1)
            return 0;
    }
    return bluetooth_headset_segment_bind(&app,headset.client)==1;
}
