/* Actual UART/H4 with source-defined register effects. USR.RFNE is bit 3;
 * UART1 RX triggers at 128 bytes and timeout IIR12 ends a shorter burst.
 * Sources: docs/uart.md, pinned sunxi-uart-ng.h and A733 manual 18.10.9.13.
 * The optional task scenario models only mask/notify/rearm and uses the real
 * FreeRTOS kernel and deadline facility; it is not native headset evidence. */
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "bt_h4.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
    struct a733_uart uart;
    struct bt_h4 h4;
    uint8_t storage[2][512],rx[512],tx[1024];
    size_t received,consumed,transmitted,room;
    uint32_t registers[64];
    unsigned timeout,thre,busy,error,fail_ier,fail_tx;
    unsigned iir_reads,usr_reads,rfl_reads,lsr_reads,ier_writes,accesses;
    unsigned enabled,pending,wakes;
    TaskHandle_t owner;
};
static struct fixture task_fixture;
static unsigned cause(const struct fixture *f)
{
    size_t rx=f->received-f->consumed;
    if(f->busy)return 7;
    if(f->error)return 6;
    if(rx>=128)return 4;
    if(rx&&f->timeout)return 12;
    if((f->uart.ier&2u)&&f->thre)return 2;
    return 1;
}
static int read_register(void *context,uintptr_t address,uint32_t *value)
{
    struct fixture *f=context;unsigned offset=(unsigned)(address-UINT32_C(0x02501000));
    assert(!host_in_isr()&&f->owner==xTaskGetCurrentTaskHandle());++f->accesses;
    assert(!(offset&3)&&offset<=0xa4);
    if(offset==0){assert(f->consumed<f->received);*value=f->rx[f->consumed++];}
    else if(offset==8){++f->iir_reads;*value=cause(f);if(*value==2)f->thre=0;}
    else if(offset==0x14){++f->lsr_reads;*value=0x60u|(f->consumed<f->received?1u:0u)|f->error;f->error=0;}
    else if(offset==0x7c){++f->usr_reads;*value=4u|(f->room?2u:0u)|(f->consumed<f->received?9u:0u);f->busy=0;}
    else if(offset==0x84){++f->rfl_reads;*value=(uint32_t)(f->received-f->consumed);}
    else if(offset==0xa4)*value=3;
    else *value=f->registers[offset/4];
    return 1;
}
static int write_register(void *context,uintptr_t address,uint32_t value)
{
    struct fixture *f=context;unsigned offset=(unsigned)(address-UINT32_C(0x02501000));
    assert(!host_in_isr()&&f->owner==xTaskGetCurrentTaskHandle());++f->accesses;
    if(!offset&&!(f->registers[3]&128u)){
        assert(f->room&&f->transmitted<sizeof(f->tx));
        --f->room;f->tx[f->transmitted++]=(uint8_t)value;
        if(f->fail_tx)return -1;
    }else if(offset==4&&!(f->registers[3]&128u)){
        ++f->ier_writes;
        if(!(f->registers[1]&2u)&&(value&2u)&&f->room)f->thre=1;
        f->registers[1]=value;
        if(f->fail_ier)return -1;
    }else if((offset!=0&&offset!=4)||!(f->registers[3]&128u))f->registers[offset/4]=value;
    return 1;
}
static int clock_read(void *unused,uint64_t *value)
{(void)unused;*value=host_time_us();return 1;}
static int current(void *context,uint64_t firmware,uint64_t uart)
{
    struct fixture *f=context;
    return f->owner==xTaskGetCurrentTaskHandle()&&firmware==4&&uart==9;
}
static void initialize(struct fixture *f)
{
    memset(f,0,sizeof(*f));f->owner=xTaskGetCurrentTaskHandle();f->room=256;
    struct a733_uart_ops ops={read_register,write_register,clock_read};
    struct a733_uart_format format={1,8,2,0,1,24000000,1500000,0};
    assert(a733_uart_begin(&f->uart,&ops,f,&format,3000000)==A733_UART_CONFIGURING);
    assert(a733_uart_poll(&f->uart)==A733_UART_READY);
    struct bt_h4_storage slots[2]={{f->storage[0],512},{f->storage[1],512}};
    struct bt_h4_binding binding={&f->uart,4,9,100000,1500000,current,f};
    assert(bt_h4_init(&f->h4,&binding,slots,2,3000000)==1);
    assert(bt_h4_irq_enable(&f->h4,3000000)==1);
    assert(bt_h4_irq_enable(&f->h4,3000000)==-1);
    f->accesses=f->iir_reads=f->usr_reads=f->lsr_reads=f->ier_writes=0;
}
static void append(struct fixture *f,const uint8_t *bytes,size_t count)
{
    assert(count<=sizeof(f->rx)-f->received);
    memcpy(f->rx+f->received,bytes,count);f->received+=count;
}
static int step(struct fixture *f,uint64_t tag,uint64_t until)
{return bt_h4_step(&f->h4,64,tag,until);}
static void release(struct fixture *f)
{
    const struct bt_h4_record *r=bt_h4_next(&f->h4,BT_H4_COMPLETE);
    assert(r&&bt_h4_release(&f->h4,r->serial)==1);
}
static void short_rx(void)
{
    struct fixture f;initialize(&f);
    const uint8_t frame[]={4,0xff,1,0xa5};append(&f,frame,sizeof(frame));
    assert(step(&f,0,3000000)==1&&f.iir_reads==1&&!f.lsr_reads&&!f.consumed);
    assert(bt_h4_irq_ready(&f.h4)); /* RX timeout still needs an armed source. */
    assert(bt_h4_command(&f.h4,0x0c03,NULL,0,200000)==1&&(f.uart.ier&2u));
    assert(step(&f,0,3000000)==1&&!f.transmitted&&!f.consumed&&!(f.uart.ier&2u));
    assert(bt_h4_irq_ready(&f.h4));
    assert(bt_h4_deadline(&f.h4,3000000)==200000);
    f.timeout=1;assert(step(&f,0,3000000)==1&&f.consumed==sizeof(frame)&&!f.transmitted);
    assert(bt_h4_next(&f.h4,BT_H4_COMPLETE)->tag==0);
    release(&f);assert(step(&f,0,3000000)==1&&(f.uart.ier&2u));
    assert(step(&f,0,3000000)==1&&f.transmitted==4&&f.h4.tx_offset==4&&!(f.uart.ier&2u));
    assert(!memcmp(f.tx,(uint8_t[]){1,3,12,0},4));
    puts("PASS subthreshold RX, timeout12, blocked THRE, release and explicit TX rearm");
}
static void suffix(void)
{
    struct fixture f;initialize(&f);const uint8_t payload[]={10,11,12};
    assert(bt_h4_acl(&f.h4,0x2001,payload,sizeof(payload),200000)==1);
    assert(step(&f,0,3000000)==1&&f.h4.tx_offset==5&&(f.uart.ier&2u));
    assert(step(&f,0,3000000)==1&&f.h4.tx_offset==8&&!(f.uart.ier&2u));
    assert(!memcmp(f.tx,(uint8_t[]){2,1,32,3,0,10,11,12},8));
    f.room=1;assert(bt_h4_sco(&f.h4,1,payload,sizeof(payload),200000)==1);
    assert(step(&f,0,3000000)==1&&f.h4.tx_offset==1&&(f.uart.ier&2u));
    f.room=256;f.thre=1;assert(step(&f,0,3000000)==1&&f.h4.tx_offset==4&&(f.uart.ier&2u));
    assert(step(&f,0,3000000)==1&&f.h4.tx_offset==7&&!(f.uart.ier&2u));
    puts("PASS ACL/SCO header and payload suffixes and one-byte FIFO backpressure");
}
static void pool(void)
{
    struct fixture f;initialize(&f);const uint8_t frames[]={4,0xff,0,4,0xfe,0,4,0xfd,0};
    append(&f,frames,sizeof(frames));f.timeout=1;
    assert(step(&f,0,3000000)==1&&f.h4.staged==9&&f.h4.stage_offset==6);
    assert(!bt_h4_irq_ready(&f.h4));unsigned lsr=f.lsr_reads;assert(step(&f,0,3000000)==1&&f.lsr_reads==lsr);
    release(&f);assert(step(&f,0,3000000)==1&&!f.h4.staged&&!bt_h4_irq_ready(&f.h4));
    release(&f);assert(bt_h4_irq_ready(&f.h4));release(&f);
    puts("PASS full record pool preserves staging/LSR and consumer release restores rearm capacity");
}
static void deadlines(void)
{
    struct fixture f;initialize(&f);const uint8_t partial[]={4,0xff};
    append(&f,partial,sizeof(partial));f.timeout=1;
    assert(step(&f,0,3000000)==1&&bt_h4_deadline(&f.h4,3000000)==100000);
    host_advance_us(99999);assert(step(&f,0,3000000)==1);
    host_advance_us(1);unsigned before=f.accesses;
    assert(step(&f,0,3000000)==-1&&f.accesses==before&&!bt_h4_deadline(&f.h4,3000000));
    struct fixture tx;initialize(&tx);assert(bt_h4_command(&tx.h4,0x0c03,NULL,0,100123)==1);
    host_advance_us(123);before=tx.accesses;assert(step(&tx,0,3000000)==-1&&tx.accesses==before);
    puts("PASS exact partial-frame/TX deadlines fault without later device access");
}
static void faults(void)
{
    struct fixture f;initialize(&f);
    assert(bt_h4_command(&f.h4,0x0c03,NULL,0,200000)==1);
    f.busy=1;assert(step(&f,0,3000000)==-1&&f.iir_reads==1&&f.usr_reads==1&&!f.transmitted);
    /* Exactly one USR read acknowledges IIR7 in the existing fault path;
     * a speculative RFNE read before IIR would hide this failure. */
    unsigned before=f.accesses;assert(step(&f,0,3000000)==-1&&f.accesses==before);
    struct fixture arm;initialize(&arm);arm.fail_ier=1;
    assert(bt_h4_command(&arm.h4,0x0c03,NULL,0,200000)==-1&&arm.h4.tx_length==4);
    before=arm.accesses;assert(step(&arm,0,3000000)==-1&&arm.accesses==before);
    struct fixture tx;initialize(&tx);tx.fail_tx=1;
    assert(bt_h4_command(&tx.h4,0x0c03,NULL,0,200000)==1);
    assert(step(&tx,0,3000000)==-1&&tx.transmitted==1&&tx.h4.state==BT_H4_FAULT);
    before=tx.accesses;assert(step(&tx,0,3000000)==-1&&tx.accesses==before);
    struct fixture rx;initialize(&rx);const uint8_t frame[]={4,0xff,0};
    append(&rx,frame,sizeof(frame));rx.error=2;
    assert(step(&rx,0,3000000)==-1&&rx.h4.staged==3&&rx.uart.errors==2);
    struct fixture mode;initialize(&mode);mode.uart.mode=A733_UART_MODE_LOOPBACK;
    before=mode.accesses;assert(step(&mode,0,3000000)==-1&&mode.accesses==before);
    puts("PASS IIR7 before RFNE, uncertain IER/TX and line-error retained faults");
}
static void quiet_busy(void)
{
    for(unsigned bytes=0;bytes<=3;bytes+=3){
        struct fixture f;initialize(&f);f.received=bytes;f.busy=1;
        assert(a733_uart_rx_empty(&f.uart,3000000)==!bytes);
        assert(f.busy&&!f.iir_reads&&!f.usr_reads&&!f.lsr_reads&&!f.consumed&&f.rfl_reads==1);
        assert(step(&f,0,3000000)==-1&&f.iir_reads==1&&f.usr_reads==1&&!f.busy&&!f.consumed);
        unsigned before=f.accesses;assert(step(&f,0,3000000)==-1&&f.accesses==before);
    }
    struct fixture full;initialize(&full);full.received=256;
    assert(a733_uart_rx_empty(&full.uart,3000000)==0&&!full.consumed);
    unsigned before=full.accesses;full.uart.plan.fcr=0;
    assert(a733_uart_rx_empty(&full.uart,3000000)==-1&&full.accesses==before);
    puts("PASS read-only FIFO level preserves pending BUSY7 with empty/nonempty RX and requires FIFO enable");
}
static void uart_irq(void *unused)
{
    (void)unused;struct fixture *f=&task_fixture;
    assert(host_in_isr()&&f->enabled);f->enabled=0;f->pending=1;
    BaseType_t woken=pdFALSE;vTaskNotifyGiveFromISR(f->owner,&woken);portYIELD_FROM_ISR(woken);
}
static void inject(void *unused)
{
    (void)unused;const uint8_t frame[]={4,0xff,0};append(&task_fixture,frame,sizeof(frame));
    task_fixture.timeout=1;uart_irq(NULL);
}
static void owner_task(void *unused)
{
    (void)unused;struct fixture *f=&task_fixture;initialize(f);
    assert(sunstone_deadline_register()&&step(f,0,2000000)==1);
    f->enabled=(unsigned)bt_h4_irq_ready(&f->h4);
    for(;;){
        assert(sunstone_deadline_wait_until(bt_h4_deadline(&f->h4,2000000))!=SUNSTONE_DEADLINE_ERROR);
        ++f->wakes;
        assert(f->pending&&!f->enabled);f->pending=0;
        assert(step(f,0,2000000)==1);f->enabled=(unsigned)bt_h4_irq_ready(&f->h4);
    }
}
static void wait_until(uint64_t until)
{
    while(host_time_us()<until)assert(sunstone_deadline_wait_until(until)!=SUNSTONE_DEADLINE_ERROR);
}
static void no_event(void)
{
    assert(sunstone_deadline_register());host_timer_handler(sunstone_deadline_irq);
    assert(host_task_create("H4 owner",owner_task,NULL,3));
    wait_until(1000000);
    assert(task_fixture.wakes==0&&task_fixture.iir_reads==1&&!task_fixture.usr_reads&&!task_fixture.lsr_reads);
    host_irq_at(1000123,inject,NULL);wait_until(1000200);
    assert(task_fixture.wakes==1&&bt_h4_next(&task_fixture.h4,BT_H4_COMPLETE));
    puts("PASS zero idle owner wakes in one second; interrupt notification services short RX at 123 us phase");
}
static void run(void *argument)
{
    const char *name=argument;
    if(!strcmp(name,"idle"))no_event();
    else if(!strcmp(name,"short-rx"))short_rx();
    else if(!strcmp(name,"suffix"))suffix();
    else if(!strcmp(name,"pool"))pool();
    else if(!strcmp(name,"deadlines"))deadlines();
    else if(!strcmp(name,"faults"))faults();
    else if(!strcmp(name,"quiet-busy"))quiet_busy();
    else abort();
}
int main(int argc,char **argv)
{assert(argc==2);host_rtos_run(run,argv[1],2,3000000);return 0;}
