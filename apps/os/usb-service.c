/* Class-neutral ownership queues. The native USB task publishes completions
 * and dispatches requests after its interrupt or request notification wakes. */
#include "sunstone_usb_service.h"
#include <stdatomic.h>
#include <string.h>
#ifdef SUNSTONE_USB_APP_ALIAS
#include "a733_usb_memory.h"
#endif
#include "sunstone_usb_serial.h"
#include "sunstone_usb_uvc.h"
/* Lease tables follow the drivers that fill them: every receive buffer of
 * every serial port can be leased at once, and the camera holds its frame ring
 * plus the frame being assembled. A frame has at most SUNSTONE_UVC_SLICES
 * pages, one per shared data page. The input queue is a depth, not a format
 * limit: build with -DSS_USB_INPUT_EVENTS=N to size it for the report rate
 * and the longest the application can be away from ss_input_event. */
#define serial_SLOTS (SS_USB_SERIAL_PORTS*SS_USB_SERIAL_SLOTS)
#define camera_SLOTS (SUNSTONE_UVC_FRAMES+1u)
#ifndef SS_USB_INPUT_EVENTS
#define SS_USB_INPUT_EVENTS 1024u
#endif
#define EVENTS SS_USB_INPUT_EVENTS
struct lease { atomic_uint state; uint64_t token; }; /* free, ready, held, release */
static struct { struct lease lease; struct ss_usb_serial_buffer value; } serial[serial_SLOTS];
static struct { struct lease lease; struct ss_usb_camera_frame value;
#ifdef SUNSTONE_USB_APP_ALIAS
    struct ss_usb_frame_slice slices[SUNSTONE_UVC_SLICES];
#endif
} camera[camera_SLOTS];
static struct { atomic_uint write,read; struct ss_input_event value[EVENTS]; } input;
static struct { atomic_uint state; uint64_t sequence; const void *data; size_t bytes,actual; int status; } tx;
static struct {atomic_uint state;uint64_t sequence;struct ss_usb_serial_line line;int result;} config;
static uint64_t last_config;
static atomic_uint connected,disconnected,retiring;
static atomic_uint input_epoch[2];
static unsigned event_epoch[EVENTS];
static uint64_t serial_token,camera_token,last_tx;
static struct os_usb_service_ops ops;
static void *context;
static int allowed(unsigned device)
{ const struct ss_grant *g=ss_grant(); return g&&(g->devices&device)&&!atomic_load_explicit(&retiring,memory_order_acquire)&&ss_live()>=0; }
static void wake(void){if(ops.wake)ops.wake(context);}
static void changed(void){if(ops.changed)ops.changed(context);}
void os_usb_service_init(const struct os_usb_service_ops *o,void *c)
{if(o)ops=*o;else memset(&ops,0,sizeof(ops));if(!ops.leave)ops.enter=NULL;context=c;}
void os_usb_service_connected(unsigned devices)
{atomic_fetch_and_explicit(&disconnected,~devices,memory_order_release);atomic_fetch_or_explicit(&connected,devices,memory_order_release);changed();}
void os_usb_service_disconnected(unsigned devices)
{
    atomic_fetch_or_explicit(&disconnected,devices,memory_order_release);
    atomic_fetch_and_explicit(&connected,~devices,memory_order_release);
    if(devices&SS_DEVICE_USB_KEYBOARD)atomic_fetch_add_explicit(&input_epoch[0],1,memory_order_release);
    if(devices&SS_DEVICE_USB_MOUSE)atomic_fetch_add_explicit(&input_epoch[1],1,memory_order_release);
    for(unsigned i=0;i<serial_SLOTS;++i){unsigned ready=1;
        if(devices&SS_DEVICE_USB_SERIAL)atomic_compare_exchange_strong_explicit(&serial[i].lease.state,&ready,3,memory_order_acq_rel,memory_order_acquire);
        ready=1;if(devices&SS_DEVICE_USB_CAMERA)atomic_compare_exchange_strong_explicit(&camera[i].lease.state,&ready,3,memory_order_acq_rel,memory_order_acquire);
    }
    changed();
}
int os_usb_service_input(const struct ss_input_event *e)
{
    if(!e||(e->kind!=SS_INPUT_KEY&&e->kind!=SS_INPUT_POINTER))return -1;
    if(atomic_load_explicit(&retiring,memory_order_acquire))return 1;
    unsigned w=atomic_load_explicit(&input.write,memory_order_relaxed);
    unsigned r=atomic_load_explicit(&input.read,memory_order_acquire);
    if(w-r==EVENTS)return 0;
    event_epoch[w%EVENTS]=atomic_load_explicit(&input_epoch[e->kind==SS_INPUT_POINTER],memory_order_acquire);
    input.value[w%EVENTS]=*e;atomic_store_explicit(&input.write,w+1,memory_order_release);changed();return 1;
}
int ss_input_event(struct ss_input_event *e)
{
    if(!e||!allowed(SS_DEVICE_USB_KEYBOARD|SS_DEVICE_USB_MOUSE))return -1;
    unsigned r=atomic_load_explicit(&input.read,memory_order_relaxed);
    unsigned w=atomic_load_explicit(&input.write,memory_order_acquire);
    while(r!=w){
        struct ss_input_event v=input.value[r%EVENTS];
        unsigned epoch=event_epoch[r%EVENTS];
        atomic_store_explicit(&input.read,++r,memory_order_release);
        if(epoch==atomic_load_explicit(&input_epoch[v.kind==SS_INPUT_POINTER],memory_order_acquire)&&allowed(v.kind==SS_INPUT_KEY?SS_DEVICE_USB_KEYBOARD:SS_DEVICE_USB_MOUSE)){*e=v;return 1;}
    }
    const struct ss_grant *g=ss_grant();
    return atomic_load_explicit(&disconnected,memory_order_acquire)&g->devices&(SS_DEVICE_USB_KEYBOARD|SS_DEVICE_USB_MOUSE)?-1:0;
}
static int serial_prepare(unsigned i)
{
    struct ss_usb_serial_buffer *v=&serial[i].value;
    if(!v->data||!v->bytes)return 0;
#ifdef SUNSTONE_USB_APP_ALIAS
    v->data=a733_usb_memory_to_app(v->data,v->bytes);if(!v->data)return 0;
#endif
    return 1;
}
static int camera_prepare(unsigned i)
{
    struct ss_usb_camera_frame *v=&camera[i].value;size_t total=0;
    if(!v->slices||!v->slice_count||v->slice_count>SUNSTONE_UVC_SLICES||!v->bytes)return 0;
    for(size_t n=0;n<v->slice_count;++n){const struct ss_usb_frame_slice *slice=&v->slices[n];
        if(!slice->data||!slice->bytes||slice->bytes>v->bytes-total)return 0;
        total+=slice->bytes;
#ifdef SUNSTONE_USB_APP_ALIAS
        const uint8_t *data=a733_usb_memory_to_app(slice->data,slice->bytes);if(!data)return 0;
        camera[i].slices[n]=(struct ss_usb_frame_slice){data,slice->bytes};
#endif
    }
    if(total!=v->bytes)return 0;
#ifdef SUNSTONE_USB_APP_ALIAS
    v->slices=camera[i].slices;
#endif
    return 1;
}
#define LEASE_FUNCTIONS(name,type,device) \
int os_usb_service_##name(const struct type *v) \
{ \
    if(atomic_load_explicit(&retiring,memory_order_acquire))return 0; \
    if(!v||!v->token||v->token<=name##_token||!(atomic_load_explicit(&connected,memory_order_acquire)&device))return -1; \
    for(unsigned i=0;i<name##_SLOTS;++i)if(atomic_load_explicit(&name[i].lease.state,memory_order_acquire)&&name[i].lease.token==v->token)return -1; \
    for(unsigned i=0;i<name##_SLOTS;++i)if(!atomic_load_explicit(&name[i].lease.state,memory_order_acquire)){ \
        name[i].value=*v;if(!name##_prepare(i))return -1;name##_token=v->token;name[i].lease.token=v->token;atomic_store_explicit(&name[i].lease.state,1,memory_order_release);changed();return 1;} \
    return 0; \
} \
int ss_usb_##name##_acquire(struct type *v) \
{ \
    if(!v||!allowed(device))return -1; \
    if(!(atomic_load_explicit(&connected,memory_order_acquire)&device))return atomic_load_explicit(&disconnected,memory_order_acquire)&device?-1:0; \
    for(unsigned i=0;i<name##_SLOTS;++i){unsigned ready=1;if(atomic_compare_exchange_strong_explicit(&name[i].lease.state,&ready,2,memory_order_acq_rel,memory_order_acquire)){ \
        *v=name[i].value;return 1;}} \
    return 0; \
} \
int ss_usb_##name##_release(uint64_t token) \
{ \
    const struct ss_grant *g=ss_grant();if(!token||!g||!(g->devices&device))return -1; \
    for(unsigned i=0;i<name##_SLOTS;++i)if(atomic_load_explicit(&name[i].lease.state,memory_order_acquire)==2&&name[i].lease.token==token){ \
        atomic_store_explicit(&name[i].lease.state,3,memory_order_release);wake();return 1;} \
    return -1; \
}
LEASE_FUNCTIONS(serial,ss_usb_serial_buffer,SS_DEVICE_USB_SERIAL)
LEASE_FUNCTIONS(camera,ss_usb_camera_frame,SS_DEVICE_USB_CAMERA)
int os_usb_service_frame(const struct ss_usb_camera_frame *v){return os_usb_service_camera(v);}
int os_usb_service_camera_pin(uint64_t token,struct ss_usb_camera_frame *frame)
{
    if(!frame||!token)return 0;
    for(unsigned i=0;i<camera_SLOTS;++i){unsigned held=2;
        if(camera[i].lease.token==token&&atomic_compare_exchange_strong_explicit(&camera[i].lease.state,&held,4,memory_order_acq_rel,memory_order_acquire)){
            *frame=camera[i].value;return 1;}}
    return 0;
}
void os_usb_service_camera_unpin(uint64_t token)
{
    for(unsigned i=0;i<camera_SLOTS;++i)if(camera[i].lease.token==token&&atomic_load_explicit(&camera[i].lease.state,memory_order_acquire)==4)
        atomic_store_explicit(&camera[i].lease.state,2,memory_order_release);
}
int ss_usb_serial_write(uint64_t sequence,const void *data,size_t bytes)
{
    if(!allowed(SS_DEVICE_USB_SERIAL)||(!sequence||sequence<=last_tx)||(!data&&bytes)||!bytes||
       !(atomic_load_explicit(&connected,memory_order_acquire)&SS_DEVICE_USB_SERIAL))return -1;
    if(atomic_load_explicit(&tx.state,memory_order_acquire))return 0;
    if(ops.enter){
        int entered=ops.enter(context);if(entered!=1)return entered<0?-1:0;
        if(!allowed(SS_DEVICE_USB_SERIAL)||atomic_load_explicit(&tx.state,memory_order_acquire)||
           !(atomic_load_explicit(&connected,memory_order_acquire)&SS_DEVICE_USB_SERIAL)){
            ops.leave(context);return 0;
        }
        tx.sequence=sequence;tx.data=data;tx.bytes=bytes;
        atomic_store_explicit(&tx.state,2,memory_order_release);
        int result=ops.write?ops.write(context,sequence,data,bytes):-1;
        if(result==1)last_tx=sequence;
        else atomic_store_explicit(&tx.state,0,memory_order_release);
        ops.leave(context);return result;
    }
    last_tx=sequence;tx.sequence=sequence;tx.data=data;tx.bytes=bytes;
    atomic_store_explicit(&tx.state,1,memory_order_release);wake();return 1;
}
int ss_usb_serial_write_result(uint64_t sequence,size_t *actual)
{
    const struct ss_grant *g=ss_grant();
    if(!g||!(g->devices&SS_DEVICE_USB_SERIAL)||!actual||!atomic_load_explicit(&tx.state,memory_order_acquire)||tx.sequence!=sequence)return -1;
    if(atomic_load_explicit(&tx.state,memory_order_acquire)!=3)return 0;
    *actual=tx.actual;int result=tx.status;
    atomic_store_explicit(&tx.state,0,memory_order_release);return result;
}
int ss_usb_serial_configure(uint64_t sequence,const struct ss_usb_serial_line *line)
{
    if(!allowed(SS_DEVICE_USB_SERIAL)||!line||!sequence||sequence<=last_config||!line->baud||
       line->bits<5||line->bits>8||line->parity>4||line->stop>2||line->rtscts>1)return -1;
    if(!(atomic_load_explicit(&connected,memory_order_acquire)&SS_DEVICE_USB_SERIAL))
        return atomic_load_explicit(&disconnected,memory_order_acquire)&SS_DEVICE_USB_SERIAL?-1:0;
    if(atomic_load_explicit(&config.state,memory_order_acquire))return 0;
    config.sequence=sequence;config.line=*line;last_config=sequence;
    atomic_store_explicit(&config.state,1,memory_order_release);wake();return 1;
}
int ss_usb_serial_config_result(uint64_t sequence)
{
    const struct ss_grant *g=ss_grant();
    if(!g||!(g->devices&SS_DEVICE_USB_SERIAL)||!atomic_load_explicit(&config.state,memory_order_acquire)||config.sequence!=sequence)return -1;
    if(atomic_load_explicit(&config.state,memory_order_acquire)!=3)return 0;
    int result=config.result;atomic_store_explicit(&config.state,0,memory_order_release);return result;
}
void os_usb_service_write_complete(uint64_t sequence,int status,size_t actual)
{
    if(atomic_load_explicit(&tx.state,memory_order_acquire)!=2||tx.sequence!=sequence||actual>tx.bytes)return;
    tx.status=status==1?1:-1;tx.actual=actual;atomic_store_explicit(&tx.state,3,memory_order_release);changed();
}
void os_usb_service_retire(void)
{atomic_store_explicit(&retiring,1,memory_order_release);wake();}
/* The host calls this only after Disable Slot completions and HCHalted prove
 * that an aborted transfer has no DMA writer. The following dispatch releases
 * borrowed pages through the ordinary class callbacks. */
void os_usb_service_stopped(void)
{
    if(!atomic_load_explicit(&retiring,memory_order_acquire))return;
    unsigned state=atomic_load_explicit(&tx.state,memory_order_acquire),notify=0;
    if(state==1||state==2){tx.status=-1;tx.actual=0;atomic_store_explicit(&tx.state,3,memory_order_release);notify=1;}
    state=atomic_load_explicit(&config.state,memory_order_acquire);
    if(state==1||state==2){config.result=-1;atomic_store_explicit(&config.state,3,memory_order_release);notify=1;}
    if(notify)changed();
}
int os_usb_service_idle(void)
{
    if(!atomic_load_explicit(&retiring,memory_order_acquire))return 0;
    unsigned state=atomic_load_explicit(&tx.state,memory_order_acquire);
    if(state==1||state==2)return 0;
    state=atomic_load_explicit(&config.state,memory_order_acquire);if(state==1||state==2)return 0;
    for(unsigned i=0;i<serial_SLOTS;++i)if(atomic_load_explicit(&serial[i].lease.state,memory_order_acquire))return 0;
    for(unsigned i=0;i<camera_SLOTS;++i)if(atomic_load_explicit(&camera[i].lease.state,memory_order_acquire))return 0;
    return 1;
}
void os_usb_service_dispatch(void)
{
    if(atomic_load_explicit(&retiring,memory_order_acquire)){
        unsigned cfg=atomic_load_explicit(&config.state,memory_order_acquire);
        if(cfg==1){config.result=-1;atomic_store_explicit(&config.state,3,memory_order_release);changed();}
        unsigned state=atomic_load_explicit(&tx.state,memory_order_acquire);
        if(state==1){tx.status=-1;tx.actual=0;atomic_store_explicit(&tx.state,3,memory_order_release);state=3;}
        /* A borrowed receive buffer may also be the pending transmit buffer. */
        if(state!=2){
            for(unsigned i=0;i<serial_SLOTS;++i){
                unsigned held=atomic_load_explicit(&serial[i].lease.state,memory_order_acquire);
                if(held==1||held==2)atomic_store_explicit(&serial[i].lease.state,3,memory_order_release);
            }
            for(unsigned i=0;i<camera_SLOTS;++i){
                unsigned held=atomic_load_explicit(&camera[i].lease.state,memory_order_acquire);
                if(held==1||held==2)atomic_store_explicit(&camera[i].lease.state,3,memory_order_release);
            }
        }
    }
    for(unsigned i=0;i<serial_SLOTS;++i){
        if(atomic_load_explicit(&serial[i].lease.state,memory_order_acquire)==3){
            if(ops.release)ops.release(context,SS_DEVICE_USB_SERIAL,serial[i].lease.token);
            atomic_store_explicit(&serial[i].lease.state,0,memory_order_release);}
    }
    for(unsigned i=0;i<camera_SLOTS;++i){
        if(atomic_load_explicit(&camera[i].lease.state,memory_order_acquire)==3){
            if(ops.release)ops.release(context,SS_DEVICE_USB_CAMERA,camera[i].lease.token);
            atomic_store_explicit(&camera[i].lease.state,0,memory_order_release);}
    }
    unsigned cfg=atomic_load_explicit(&config.state,memory_order_acquire);
    if(cfg==1){
        int r=ops.configure?ops.configure(context,&config.line):-1;
        if(r==1){atomic_store_explicit(&config.state,2,memory_order_release);cfg=2;}
        else{config.result=-1;atomic_store_explicit(&config.state,3,memory_order_release);changed();}
    }
    if(cfg==2){int r=ops.configured?ops.configured(context):-1;
        if(r){config.result=r==1?1:-1;atomic_store_explicit(&config.state,3,memory_order_release);changed();}}
    if(atomic_load_explicit(&tx.state,memory_order_acquire)==1){
        atomic_store_explicit(&tx.state,2,memory_order_release);
        if(!(atomic_load_explicit(&connected,memory_order_acquire)&SS_DEVICE_USB_SERIAL)||!ops.write||
           ops.write(context,tx.sequence,tx.data,tx.bytes)!=1)
            os_usb_service_write_complete(tx.sequence,-1,0);
    }
}
