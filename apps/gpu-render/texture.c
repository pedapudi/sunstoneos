#include "application.h"
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
#include "session.h"
#include "tusb.h"
#include <string.h>

/* Version 4 is private to the indexed texture profile. Each 64-byte request
 * carries a copied operation, epoch, frame, generation, offset and input shape.
 * A READ_FRAME has one response header followed by all framebuffer bytes. */
static uint8_t header[64];
static uint32_t received,payload_received,payload_bytes,sent,transferred,tx_active,tx_zlp;
static uint64_t receive_deadline;
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t get64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return v;}
static void put32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static void put64(uint8_t *p,uint64_t v){memcpy(p,&v,8);}
static _Noreturn void fault(void){gpu_application_fail(NULL);for(;;)vTaskDelay(1);}
static void live(void){if(!gpu_application_live(NULL))fault();}
static uint64_t budget(uint64_t maximum)
{
    live();uint64_t now=a733_smp_counter()/24,remaining=app.session.deadline_us-now;
    return remaining<maximum?remaining:maximum;
}
static void response(unsigned operation,unsigned result,uint32_t bytes)
{
    uint32_t width,height,stride,total;
    if(!gpu_application_texture_geometry(&width,&height) || !pvr_render_texture_geometry(width,height,&stride,&total))fault();
    uint8_t *p=app.texture_header;memset(p,0,64);
    put32(p,0x4f585047);put32(p+4,4);put32(p+8,result);put32(p+12,operation);
    put32(p+16,atomic_load_explicit(&app.runtime_sequence,memory_order_acquire));
    put32(p+20,width);put32(p+24,height);put32(p+28,stride);put32(p+32,total);
    put32(p+40,bytes);put32(p+44,atomic_load_explicit(&app.runtime_closed,memory_order_acquire));
    put64(p+48,app.session.epoch);put64(p+56,app.texture_generation);
}
static void publish(const void *data,uint32_t bytes,unsigned with_header)
{
    if(atomic_load_explicit(&app.texture_tx_state,memory_order_acquire))fault();
    app.texture_tx_data=data;app.texture_tx_bytes=bytes;app.texture_tx_header=with_header;
    atomic_store_explicit(&app.texture_tx_state,1,memory_order_release);
}
static void delivered(uint64_t operation_deadline)
{
    uint64_t end=a733_smp_counter()/24+budget(1000000);
    if(operation_deadline && operation_deadline<end)end=operation_deadline;
    while(atomic_load_explicit(&app.texture_tx_state,memory_order_acquire)!=2){
        live();if(a733_smp_counter()/24>=end)fault();vTaskDelay(1);
    }
    atomic_store_explicit(&app.texture_tx_state,0,memory_order_release);
}
static struct pvr_gpu_service_reply scoped(struct pvr_render_texture_request *q,const void *data,uint64_t timeout,uint64_t *ticket)
{
    struct pvr_gpu_service_reply reply={.result=PVR_GPU_SERVICE_INVALID};
    if(!pvr_gpu_service_texture(&app.service,app.last.epoch,q,data,budget(timeout),1,ticket))return reply;
    while(!pvr_gpu_service_collect(&app.service,*ticket,&reply,1))live();
    if(reply.result==PVR_GPU_SERVICE_FAULT)fault();return reply;
}
static struct pvr_gpu_service_reply status(void)
{
    struct pvr_gpu_service_request q={PVR_GPU_SERVICE_STATUS,0};struct pvr_gpu_service_reply r;uint64_t ticket;
    if(!pvr_gpu_service_send(&app.service,&q,1,&ticket))fault();
    while(!pvr_gpu_service_collect(&app.service,ticket,&r,1))live();
    if(r.result!=PVR_GPU_SERVICE_OK)fault();return r;
}
void gpu_application_texture_client(void)
{
    for(;;) {
        while(atomic_load_explicit(&app.runtime_state,memory_order_acquire)!=1){live();vTaskDelay(1);}
        unsigned operation=app.runtime_operation;
        if(operation==5)return;
        struct pvr_render_texture_request q=app.texture_request;
        uint64_t ticket=0;
        if(operation==4) {
            uint32_t width,height,stride,total;
            if(!gpu_application_texture_geometry(&width,&height) || !pvr_render_texture_geometry(width,height,&stride,&total))fault();
            q.operation=PVR_TEXTURE_READ;q.offset=0;q.bytes=total<PVR_RENDER_TEXTURE_CHUNK?total:PVR_RENDER_TEXTURE_CHUNK;
            q.generation=0;
            struct pvr_gpu_service_reply r=scoped(&q,NULL,1000000,&ticket);
            if(r.result!=PVR_GPU_SERVICE_OK){response(operation,4,0);publish(NULL,0,1);delivered(0);}
            else {
                uint64_t stream_deadline=a733_smp_counter()/24+budget(30000000);
                response(operation,1,total);publish(NULL,0,1);delivered(stream_deadline);
                for(uint32_t offset=0;offset<total;) {
                    live();if(a733_smp_counter()/24>=stream_deadline)fault();
                    const void *data;uint32_t bytes;
                    if(!pvr_gpu_service_texture_output(&app.service,ticket,&data,&bytes) || !bytes || bytes!=q.bytes)fault();
                    publish(data,bytes,0);delivered(stream_deadline);
                    uint64_t read_ticket=ticket;
                    struct pvr_render_texture_request release={.operation=PVR_TEXTURE_RELEASE,.previous=q.previous,
                        .generation=read_ticket,.offset=offset};
                    r=scoped(&release,NULL,1000000,&ticket);if(r.result!=PVR_GPU_SERVICE_OK)fault();
                    offset+=bytes;if(offset==total)break;
                    q.offset=offset;q.bytes=total-offset<PVR_RENDER_TEXTURE_CHUNK?total-offset:PVR_RENDER_TEXTURE_CHUNK;
                    r=scoped(&q,NULL,1000000,&ticket);if(r.result!=PVR_GPU_SERVICE_OK)fault();
                }
            }
        } else {
            struct pvr_gpu_service_reply r=scoped(&q,operation==2?app.runtime_input:NULL,
                operation==1?30000000:operation==3?5000000:1000000,&ticket);
            if(r.result==PVR_GPU_SERVICE_OK) {
                app.texture_generation=r.texture_generation;
                if(operation==3) {
                    atomic_store_explicit(&app.runtime_sequence,r.render_sequence,memory_order_release);
                    while(r.render_phase!=PVR_RENDER_SERVICE_COMPLETE){live();vTaskDelay(1);r=status();}
                }
            }
            response(operation,r.result==PVR_GPU_SERVICE_OK?1:4,0);publish(NULL,0,1);delivered(0);
        }
        atomic_store_explicit(&app.runtime_state,0,memory_order_release);
    }
}
void gpu_application_texture_closed(void)
{
    atomic_store_explicit(&app.runtime_closed,1,memory_order_release);
    response(5,1,0);publish(NULL,0,1);delivered(0);
}
void gpu_application_texture_tx(uint32_t bytes)
{
    if(!tx_active || bytes>sent-transferred || (tx_zlp && bytes))fault();
    transferred+=bytes;
    unsigned total=app.texture_tx_bytes+app.texture_tx_header*64;
    if(transferred==total && sent==total) {
        /* Pinned vendor_device.c calls us before deciding whether the empty
         * FIFO needs its automatic ZLP. Retain publication through that final
         * callback as well, including a 64-byte header at full speed. */
        unsigned packet=tud_speed_get()==TUSB_SPEED_HIGH?512:64;
        if(bytes && !(bytes&(packet-1))){tx_zlp=1;return;}
        if(!bytes && !tx_zlp)fault();
        tx_zlp=0;
        if(app.texture_tx_header && get32(app.texture_header+12)==0)
            atomic_store_explicit(&app.usb_observed,1,memory_order_release);
        tx_active=0;atomic_store_explicit(&app.texture_tx_state,2,memory_order_release);
    }
}
void gpu_application_texture_usb(void)
{
    unsigned tx=atomic_load_explicit(&app.texture_tx_state,memory_order_acquire);
    if(tx==1) {
        if(!tx_active){sent=transferred=tx_zlp=0;tx_active=1;}
        uint32_t header_bytes=app.texture_tx_header*64,total=header_bytes+app.texture_tx_bytes;
        if(sent==total)return;
        const uint8_t *data;uint32_t n;
        if(sent<header_bytes){data=app.texture_header+sent;n=header_bytes-sent;}
        else {data=app.texture_tx_data+sent-header_bytes;n=total-sent;}
        if(n){sent+=tud_vendor_write(data,n);tud_vendor_flush();}return;
    }
    if(tx==2) {
        /* STATUS is the only response consumed entirely by the USB task. */
        if(!atomic_load_explicit(&app.runtime_state,memory_order_acquire))
            atomic_store_explicit(&app.texture_tx_state,0,memory_order_release);
        return;
    }
    if(received && a733_smp_counter()/24>=receive_deadline)fault();
    if(atomic_load_explicit(&app.runtime_state,memory_order_acquire)==1 || !tud_vendor_available())return;
    if(!received)receive_deadline=a733_smp_counter()/24+budget(1000000);
    if(a733_smp_counter()/24>=receive_deadline)fault();
    if(received<64) {
        unsigned n=tud_vendor_available();if(n>64-received)n=64-received;
        received+=tud_vendor_read(header+received,n);if(received<64)return;
        payload_bytes=get32(header+36);
        if(payload_bytes>PVR_RENDER_TEXTURE_CHUNK)fault();
        unsigned idle=0;
        if(!atomic_compare_exchange_strong_explicit(&app.runtime_state,&idle,2,memory_order_acquire,memory_order_relaxed))fault();
    }
    if(payload_received<payload_bytes) {
        unsigned n=tud_vendor_available();if(n>payload_bytes-payload_received)n=payload_bytes-payload_received;
        payload_received+=tud_vendor_read(app.runtime_input+payload_received,n);if(payload_received<payload_bytes)return;
    }
    unsigned operation=get32(header+8),valid=get32(header)==0x49585047 && get32(header+4)==4 &&
        get64(header+16)==app.session.epoch && !get32(header+60) && operation<=5;
    struct pvr_render_texture_request q={.previous=get32(header+12),.generation=get64(header+24),
        .offset=get32(header+32),.bytes=payload_bytes,
        .input={get32(header+40),get32(header+44),get32(header+48),get32(header+52),get32(header+56)}};
    if(operation!=2 && payload_bytes)valid=0;
    if(operation && atomic_load_explicit(&app.runtime_closed,memory_order_acquire))valid=0;
    if((operation==4 || operation==5) && (q.generation || q.offset || q.bytes ||
        q.previous!=atomic_load_explicit(&app.runtime_sequence,memory_order_acquire)))valid=0;
    if(!valid || !operation) {
        response(operation,valid?(atomic_load_explicit(&app.runtime_closed,memory_order_acquire)?1:0):4,0);
        atomic_store_explicit(&app.runtime_state,0,memory_order_release);publish(NULL,0,1);
    } else {
        q.operation=operation==1?PVR_TEXTURE_BEGIN:operation==2?PVR_TEXTURE_WRITE:operation==3?PVR_TEXTURE_SUBMIT:PVR_TEXTURE_READ;
        app.texture_request=q;app.runtime_operation=operation;
        atomic_store_explicit(&app.runtime_state,1,memory_order_release);
    }
    received=payload_received=payload_bytes=0;
}
#endif
