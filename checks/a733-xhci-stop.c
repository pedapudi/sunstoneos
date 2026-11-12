/* SPDX-License-Identifier: Apache-2.0 */
/* The production xHCI return path with the existing command/transfer model.
 * xHCI 1.2 sections 5.4.1, 5.4.1.1 and 5.4.2 require received endpoint and
 * command events plus completed USB2 resets before R/S clears. HCH follows
 * pipeline drain within 16 ms. HSE/HCE and CNR do not prove orderly shutdown.
 * The 3 ms modeled halt and 5 ms reset are stimuli within those contracts;
 * they are not measured A733 latency. No hardware access occurs. */
#include <limits.h>
#define main detach_fixture_main
#include "a733-xhci-detach.c"
#undef main
static uint64_t halt_at,reset_clear_at,ready_at;
static unsigned accesses,halt_writes,reset_writes,busy,live_calls,halt_requested;
static unsigned delay_reset,never_halt,stop_bells;
static uint32_t halt_error;
static uint8_t destination[512];
static int stop_live(void *c,uint64_t generation,uint64_t deadline)
{
    ++live_calls;
    return busy?0:live(c,generation,deadline);
}
static int stop_rd(void *c,uint32_t offset,uint32_t *value)
{
    ++accesses;
    if(reset_clear_at&&now>=reset_clear_at){reg[0x40/4]&=~2u;reset_clear_at=0;}
    if(ready_at&&now>=ready_at){reg[0x44/4]&=~BIT(11);ready_at=0;}
    if(halt_requested&&!never_halt&&now>=halt_at)reg[0x44/4]|=1|halt_error;
    return rd(c,offset,value);
}
static int stop_wr(void *c,uint32_t offset,uint32_t value)
{
    ++accesses;
    if(offset==0x40&&(value&2)){
        ++reset_writes;
        assert(!x.stop_phase); /* Return never issues a controller reset. */
        if(delay_reset){
            reg[0x40/4]=value;reg[0x44/4]=1|BIT(11);
            reset_clear_at=now+5000;ready_at=now+6000;return 1;
        }
    }
    if(offset==0x40&&(reg[0x40/4]&1)&&!(value&1)){
        /* Independently modeled device work and queued events must be gone. */
        assert(!slots&&!x.command_kind&&!(portsc&BIT(4)));
        assert((x.events[x.event_index].control&1)!=x.event_cycle);
        assert(!(reg[0x1020/4]&2)&&!(value&(2|BIT(2)|BIT(3))));
        assert(!a733_xhci_stop_draining(&x));
        ++halt_writes;halt_requested=1;halt_at=now+3000;
    }
    return wr(c,offset,value);
}
static int stop_wr64(void *c,uint32_t offset,uint64_t value)
{++accesses;return wr64(c,offset,value);}
static void construct(void)
{
    struct device_memory_extent extent;
    assert(device_memory_pool_init(&pool,(uintptr_t)dma,PHYS,sizeof(dma))&&
           device_memory_acquire(&pool,99,sizeof(dma),&lease,&extent));
    reg[0]=0x01100040;reg[1]=(1u<<24)|(1u<<8)|3;
    reg[4]=(0x20u<<16)|1|4|8;reg[5]=0x2000;reg[6]=0x1000;
    reg[0x80/4]=0x02000002;reg[0x84/4]=0x20425355;reg[0x88/4]=0x101;
    reg[0x44/4]=1;reg[0x48/4]=1;
    const struct a733_xhci_ops ops={cn,current,stop_live,stop_rd,stop_wr,stop_wr64,barrier,outside,notice};
    assert(a733_xhci_init(&x,&ops,&pool,&pool,lease,7,now+10000000,now+100000000));
}
static void running(void)
{UNTIL(portsc_reads&&(portsc&BIT(9)));step();}
static void irq_start(void)
{
    irq_model=1;
    assert(a733_xhci_interrupt_start(&x)==1);
    while(a733_xhci_interrupt_finish(&x)==0)assert(a733_xhci_poll(&x)!=A733_XHCI_RETAINED);
}
static int stop_step(uint64_t deadline)
{
    now+=100;reset_step();
    if(!is("command-timeout"))command_step();
    if(is("wrong-completion")&&x.command_kind&&last_command==x.command_pointer){
        unsigned i=events?events-1:A733_XHCI_EVENT_TRBS-1;
        x.events[i].parameter_lo^=16; /* Wrong command TRB identity. */
    }
    if(a733_xhci_stop_draining(&x)&&!x.irq_service&&(reg[0x1020/4]&1))
        assert(a733_xhci_interrupt_begin(&x)==1);
    int result=a733_xhci_stop(&x,deadline);
    if(a733_xhci_stop_draining(&x)&&x.irq_service)
        assert(a733_xhci_interrupt_finish(&x)>=0);
    return result;
}
static int complete(uint64_t deadline)
{
    int result=0;
    for(unsigned i=0;result==0&&i<40000;++i)result=stop_step(deadline);
    return result;
}
static void done(uint64_t deadline)
{
    unsigned before=notice_count;
    assert(complete(deadline)==1&&!x.failed&&memory()==DEVICE_MEMORY_DEVICE);
    assert(notice_count==before&&doorbells==stop_bells&&!slots);
    assert(!(reg[0x40/4]&1)&&(reg[0x44/4]&1));
    assert(!a733_xhci_stop_draining(&x)&&!a733_xhci_next_deadline(&x));
    unsigned io=accesses,calls=live_calls;
    assert(a733_xhci_stop(&x,deadline+1000000)==1&&accesses==io&&live_calls==calls);
    assert(a733_xhci_poll(&x)==A733_XHCI_INVALID);
    assert(a733_xhci_interrupt_start(&x)==-1&&a733_xhci_interrupt_begin(&x)==-1&&
           a733_xhci_interrupt_finish(&x)==-1&&accesses==io);
}
int main(int argc,char **argv)
{
    mode=argc>1?argv[1]:"empty";
    delay_reset=!strncmp(mode,"phase-",6)||is("reset-timeout");
    construct();
    if(!strncmp(mode,"phase-",6)){
        unsigned phase=(unsigned)strtoul(mode+6,NULL,10);assert(phase<=16);
        UNTIL(x.phase==phase);
        unsigned io=accesses,resets=reset_writes;
        if(phase==0)assert(!io);
        uint64_t deadline=now+1000000;
        if(phase==4)assert(reg[0x40/4]&2);
        stop_bells=doorbells;done(deadline);
        assert(reset_writes==resets&&halt_writes==(phase>=14));
        if(phase==0)assert(accesses==io);
        if(phase==4)assert(now>=ready_at&&!(reg[0x40/4]&2));
        printf("PASS %s preserves startup provenance and issued reset\n",mode);return 0;
    }
    if(is("unknown-phase")){x.phase=99;assert(a733_xhci_stop(&x,now+1000000)==-1&&x.failed);return 0;}
    if(is("publication")){x.publication=1;assert(a733_xhci_stop(&x,now+1000000)==-1&&x.failed);return 0;}
    if(is("reset-timeout")){
        UNTIL(x.phase==4);reset_clear_at=now+20000000;
        assert(complete(now+10000)==-1&&x.failed&&!halt_writes&&reset_writes==1);return 0;
    }
    running();
    if(is("port-reset")||is("port-reset-timeout")||is("enable-pending")){
        portsc=(portsc&~UINT32_C(0x1e0))|1|BIT(17)|(7u<<5);
        event(34,UINT64_C(1)<<24,1,0,0,0);UNTIL(x.announced);
        assert(a733_xhci_reset_port(&x,now+1000000));UNTIL(x.port_reset==2);
        if(is("port-reset-timeout"))reset_done=now+2000000;
        if(is("enable-pending"))UNTIL(x.command_kind==9);
    }else if(!is("empty")&&strncmp(mode,"halt-",5)&&!is("busy")&&!is("lost")&&!is("error")){
        attach(1);
        if(is("address-zero")){
            assert(a733_xhci_open(&x,0,0,0,64,now+1000000));
            const uint8_t setup[8]={0,5,1,0,0,0,0,0};
            assert(a733_xhci_setup(&x,0,setup,now+1000000));UNTIL(x.command_kind==11);
        }else{
            enumerate(1);
            if(is("iso")){
                assert(a733_xhci_open_isoch(&x,1,0x81,1024,1,now+1000000));
                struct a733_xhci_endpoint *e=&x.devices[0].endpoints[1];UNTIL(e->configured);
                assert(a733_xhci_isoch_start(&x,1,0x81));UNTIL(x.isoch.count>x.isoch.done);
                unsigned i=x.isoch.head;
                event(32,x.isoch.trb[i],1,0,e->number,x.devices[0].slot);UNTIL(x.isoch.done);
                const uint8_t *data;unsigned bytes;uint64_t token;
                assert(a733_xhci_isoch_acquire(&x,1,0x81,&data,&bytes,&token)==1&&bytes==1024);
                stop_bells=doorbells;done(now+1000000);
                unsigned io=accesses,calls=live_calls;
                assert(a733_xhci_isoch_release(&x,1,0x81,token)&&accesses==io&&live_calls==calls);
                assert(!x.isoch.requested);puts("PASS isochronous drain retains borrowed pages without refill");return 0;
            }
            if(is("hub")){
                assert(a733_xhci_mark_hub(&x,1)&&a733_xhci_hub_configure(&x,1,4,0,0)&&
                       a733_xhci_open_interrupt(&x,1,0x81,1,12,now+1000000));
                UNTIL(x.devices[0].hub_configured);
                assert(a733_xhci_bind_child(&x,1,1,3,now+1000000));
                UNTIL(x.devices[1].port_ready==3);enumerate(2);open_bulk(2);
            }else open_bulk(1);
            unsigned address=is("hub")?2:1;
            if(is("borrowed")){
                uint8_t *data;unsigned capacity;uint64_t token;
                assert(a733_xhci_dma_acquire(&x,address,0x81,&data,&capacity,&token)==1);
                assert(a733_xhci_transfer(&x,address,0x81,data,512,now+1000000));
                expect(notice_count,address,0x81,512);
                memset(data,0x7d,512);hold=UINT_MAX;
                assert(a733_xhci_transfer(&x,address,2,data,512,now+1000000));
                UNTIL(device(address)->endpoints[2].pending==2);
                assert(!a733_xhci_dma_release(&x,address,0x81,token));
                stop_bells=doorbells;done(now+1000000);
                for(unsigned k=0;k<512;++k)assert(data[k]==0x7d);
                unsigned io=accesses,calls=live_calls;
                assert(a733_xhci_dma_release(&x,address,0x81,token)&&accesses==io&&calls==live_calls);
                assert(x.devices[0].detach==3&&x.devices[0].slot);puts("PASS borrowed pages survive controller DONE until broker release");return 0;
            }
            if(is("recovery-stop")){
                assert(a733_xhci_clear_bulk(&x,address,0x81,now+1000000));
                UNTIL(x.command_kind==15);stop_bells=doorbells;done(now+1000000);
                puts("PASS return adopts an outstanding recovery Stop Endpoint");return 0;
            }
            if(is("recovery-reset"))assert(a733_xhci_read_recovery(&x,address));
            memset(destination,0x5a,sizeof(destination));hold=UINT_MAX;
            assert(a733_xhci_transfer(&x,address,0x81,destination,512,now+1000000));
            UNTIL(device(address)->endpoints[1].pending==2);
            if(is("halted-endpoint")||is("recovery-reset")){
                struct a733_xhci_device *d=device(address);struct a733_xhci_endpoint *e=&d->endpoints[1];
                set_state(context(d->slot)+64*e->number,2);put64(context(d->slot)+64*e->number+8,e->pending_trb|1);
                event(32,e->pending_trb,is("recovery-reset")?6:4,e->length,e->number,d->slot);
                if(is("recovery-reset"))UNTIL(x.command_kind==14);
            }
        }
    }
    if(is("irq"))irq_start();
    never_halt=is("halt-timeout");
    if(is("halt-hse"))halt_error=BIT(2);
    if(is("halt-hce"))halt_error=BIT(12);
    if(is("halt-cnr"))halt_error=BIT(11);
    stop_bells=doorbells;
    uint64_t deadline=now+1000000;
    if(is("lost"))lost=1;
    if(is("error"))reg[0x44/4]|=BIT(12);
    if(is("busy")){
        busy=1;unsigned calls=live_calls,io=accesses;
        assert(a733_xhci_stop(&x,deadline)==0&&!x.failed&&accesses==io&&live_calls-calls==1);
        busy=0;
    }
    if(is("deadline-extension")){
        uint64_t original=now+2000;
        assert(a733_xhci_stop(&x,original)==0);
        assert(complete(deadline)==-1&&x.stop_deadline==original&&memory()==DEVICE_MEMORY_RETAINED);
    }else if(is("command-timeout")||is("transfer-deadline")){
        if(is("transfer-deadline")){
            x.devices[0].endpoints[1].deadline=now+300;
            assert(a733_xhci_stop(&x,deadline)==0);assert(x.devices[0].deadline==now+300);
            now+=301;
        }else deadline=now+5000;
        assert(complete(deadline)==-1&&x.failed&&!halt_writes&&memory()==DEVICE_MEMORY_RETAINED);
    }else if(never_halt||halt_error||is("wrong-completion")||is("port-reset-timeout")||is("lost")||is("error")){
        assert(complete(deadline)==-1&&x.failed&&memory()==DEVICE_MEMORY_RETAINED);
        if(never_halt)assert(now>=x.halt_deadline&&now<x.stop_deadline);
    }else{
        unsigned commands_before=command_count;
        done(deadline);
        if(is("hub")){
            unsigned first=0,second=0;
            for(unsigned i=commands_before;i<command_count;++i)if(commands[i].kind==10){if(!first)first=commands[i].slot;else second=commands[i].slot;}
            assert(first==2&&second==1);
        }
        for(unsigned i=0;i<sizeof(destination);++i)if(hold)assert(destination[i]==0x5a);
        assert(!a733_xhci_transfer(&x,1,0x81,destination,512,deadline));
        assert(!a733_xhci_reset_port(&x,deadline));
    }
    printf("PASS %s return ordering and terminal classification\n",mode);return 0;
}
