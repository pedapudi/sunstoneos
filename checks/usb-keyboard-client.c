/* The fixture includes the OS client's actual mailbox and HID callbacks. */
#include "sunstone_usb_service.h"
#include "sunstone_usb_hid.h"
#include "a733_usb2_native.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static const struct ss_grant grant={.devices=SS_DEVICE_USB_KEYBOARD|SS_DEVICE_USB_STORAGE};
static struct {void *handles[8];struct a733_usb2_native host;uint8_t sector[512];atomic_uint result;} board;
static atomic_uint owner_notifications,application_notifications;
static unsigned storage_submissions,storage_complete;
void xTaskNotifyGive(void *owner)
{assert(owner==(void *)1);atomic_fetch_add_explicit(&owner_notifications,1,memory_order_relaxed);}
void os_application_usb_changed(void)
{atomic_fetch_add_explicit(&application_notifications,1,memory_order_relaxed);}
int a733_usb2_read_sector(struct a733_usb2_native *n,uint32_t lba,uint64_t deadline)
{assert(n==&board.host&&lba==7&&deadline==20000);++storage_submissions;return 1;}
int a733_usb2_read_result(struct a733_usb2_native *n,uint8_t data[512])
{assert(n==&board.host);if(!storage_complete)return 0;memset(data,0x5a,512);return 1;}
int a733_usb2_hid_result(struct a733_usb2_native *n,struct a733_usb2_hid_report *report)
{(void)n;(void)report;assert(0);return -1;}
int a733_usb2_hid_receive(struct a733_usb2_native *n,uint64_t deadline)
{(void)n;(void)deadline;assert(0);return 0;}
const struct ss_grant *ss_grant(void){return &grant;}
int ss_live(void){return 1;}
unsigned os_session_devices(void){return grant.devices;}
void board_fail(void *p){(void)p;assert(0);}
int os_usb_service_input(const struct ss_input_event *e){(void)e;return 1;}
void os_usb_service_connected(unsigned devices){(void)devices;}
void os_usb_service_disconnected(unsigned devices){(void)devices;}
#include "usb-keyboard-client.inc"
static struct sunstone_hid_event event={.type=SUNSTONE_HID_ATTACH,
    .address=1,.instance=0,.generation=10,.usage=SUNSTONE_HID_KEYBOARD};
static void attach(void)
{event.type=SUNSTONE_HID_ATTACH;event.usage=SUNSTONE_HID_KEYBOARD;usb_input(NULL,&event);}
static void key(int pressed)
{event.type=SUNSTONE_HID_KEY;event.usage=4;event.value=pressed;usb_input(NULL,&event);}
static void detach(void)
{event.type=SUNSTONE_HID_DETACH;event.usage=SUNSTONE_HID_KEYBOARD;usb_input(NULL,&event);}
static atomic_uint done;
static unsigned observed;
static uint64_t stress_sequence_base;
static void *reader(void *unused)
{
    (void)unused;uint64_t sequence=0;
    for(;;){
        struct ss_key_report report;int r=ss_key_report(&report);
        if(r==1){
            assert(report.attachment==20&&report.address==1&&report.interface==0&&
                report.protocol==1&&report.bytes==8&&report.sequence>sequence);
            assert(report.data[2]==(((report.sequence-stress_sequence_base)&1)?4:0));
            for(unsigned i=0;i<8;++i)if(i!=2)assert(!report.data[i]);
            sequence=report.sequence;++observed;
        }
        if(atomic_load_explicit(&done,memory_order_acquire))return NULL;
        sched_yield();
    }
}
int main(void)
{
    board.handles[4]=(void *)1;board.host.platform.config.lifetime_deadline=20000;
    uint8_t sector[512];
    assert(ss_storage_read(7,sector)==0);
    assert(atomic_exchange(&owner_notifications,0)==1);
    assert(serve()==1&&storage_submissions==1&&atomic_load(&storage.state)==2);
    storage_complete=1;assert(serve()==1&&atomic_load(&storage.state)==3);
    assert(atomic_exchange(&application_notifications,0)==1);
    assert(ss_storage_read(7,sector)==1);
    for(unsigned i=0;i<512;++i)assert(sector[i]==0x5a);
    struct ss_key_report report;
    attach();key(1);key(0);
    assert(atomic_load(&application_notifications)==1);
    assert(ss_key_report(&report)==1&&report.data[2]==4);
    assert(atomic_exchange(&owner_notifications,0)==1);
    keyboard_publish();assert(ss_key_report(&report)==1&&!report.data[2]);
    assert(atomic_exchange(&application_notifications,0)==2);
    assert(ss_key_report(&report)==0);
    key(1);key(0);detach();
    assert(ss_key_report(&report)==0);

    event.generation=11;attach();key(1);
    /* Hold the reader claim while the owner receives key-up. */
    unsigned ready=1;
    assert(atomic_compare_exchange_strong(&keyboard.ready,&ready,2));
    key(0);assert(keyboard_pending.dirty&&keyboard.report.data[2]==4);
    atomic_store_explicit(&keyboard.ready,0,memory_order_release);
    keyboard_publish();
    assert(ss_key_report(&report)==1&&!report.data[2]);
    key(1);ready=1;
    assert(atomic_compare_exchange_strong(&keyboard.ready,&ready,2));
    detach();assert(keyboard_pending.invalidate&&!keyboard_pending.dirty);
    atomic_store_explicit(&keyboard.ready,0,memory_order_release);
    keyboard_publish();assert(ss_key_report(&report)==0);

    /* Detaching another interface leaves the current snapshot intact. */
    event.generation=12;attach();key(1);
    struct sunstone_hid_event other=event;other.type=SUNSTONE_HID_ATTACH;
    other.address=2;other.generation=13;other.usage=SUNSTONE_HID_KEYBOARD;
    usb_input(NULL,&other);other.type=SUNSTONE_HID_DETACH;usb_input(NULL,&other);
    assert(ss_key_report(&report)==1&&report.attachment==12&&report.data[2]==4);
    key(0);assert(ss_key_report(&report)==1);
    stress_sequence_base=report.sequence;detach();

    /* Retire the report being read while preserving another attachment's
     * pending report. Publication resumes after the reader drops its claim. */
    event.generation=14;attach();key(1);ready=1;
    assert(atomic_compare_exchange_strong(&keyboard.ready,&ready,2));
    other.type=SUNSTONE_HID_ATTACH;other.generation=15;other.usage=SUNSTONE_HID_KEYBOARD;
    usb_input(NULL,&other);other.type=SUNSTONE_HID_KEY;other.usage=5;other.value=1;
    usb_input(NULL,&other);detach();
    assert(keyboard_pending.invalidate&&keyboard_pending.dirty);
    atomic_store_explicit(&keyboard.ready,0,memory_order_release);keyboard_publish();
    assert(ss_key_report(&report)==1&&report.attachment==15&&report.data[2]==5);
    stress_sequence_base=report.sequence;
    other.type=SUNSTONE_HID_DETACH;other.usage=SUNSTONE_HID_KEYBOARD;
    usb_input(NULL,&other);assert(ss_key_report(&report)==0);

    /* One owner publishes while the application consumes on another CPU. */
    event.generation=20;attach();
    pthread_t thread;assert(!pthread_create(&thread,NULL,reader,NULL));
    for(unsigned i=0;i<200000;++i){key(!(i&1));keyboard_publish();if(!(i&255))sched_yield();}
    atomic_store_explicit(&done,1,memory_order_release);
    assert(!pthread_join(thread,NULL));keyboard_publish();
    assert(observed);puts("PASS: retained keyboard reports, detach and concurrent publication");
    return 0;
}
