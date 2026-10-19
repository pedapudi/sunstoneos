/* Actual native framing/callbacks; endpoint transport and source samples modeled. */
#include "../apps/i2s/usb0.c"
struct i2s_application app;
volatile unsigned i2s_check_result;
static uint8_t input[12],wire[4224];
static unsigned position,wire_bytes;
void vTaskDelay(TickType_t ticks)
{(void)ticks;i2s_check_result=99;__asm__ volatile("brk #0");for(;;){}}
int i2s_live(void *v){(void)v;return !atomic_load(&app.fatal);}
void i2s_fail(void *v){(void)v;atomic_store(&app.fatal,1);}
uint32_t tud_vendor_n_available(uint8_t i){return i?0:12-position;}
uint32_t tud_vendor_n_read(uint8_t i,void *out,uint32_t n)
{if(i||n>12-position)return 0;memcpy(out,input+position,n);position+=n;return n;}
uint32_t tud_vendor_n_write(uint8_t i,const void *p,uint32_t n)
{if(i||n>sizeof(wire)-wire_bytes)return 0;if(n>512)n=512;memcpy(wire+wire_bytes,p,n);wire_bytes+=n;return n;}
uint32_t tud_vendor_n_write_flush(uint8_t i){(void)i;return 0;}
static void check(int ok,unsigned code){if(!ok){i2s_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
static void packet(unsigned operation,unsigned offset,unsigned bytes)
{
 position=wire_bytes=0;put32(input,operation);put32(input+4,offset);put32(input+8,bytes);
 exchange();for(unsigned i=0;i<9&&response_sent<response_bytes;++i)exchange();
 check(response_sent==response_bytes&&wire_bytes==response_bytes,20);
}
void i2s_check_run(unsigned mode)
{
 app.admitted=*(const struct i2s_session *)A733_STORAGE_SESSION_ADDRESS;app.session=app.admitted;
 check(i2s_session_valid(&app.session)&&app.session.direction==5,10);
 unsigned family=mode/100;uint64_t sequence=family?mode%100:0;
 unsigned stopped=family==3||family==5||family==6||family==18||family==19,failed=(family>=9&&family<=12)||(family>=15&&family<=17);
 unsigned empty=family==5||family==7||family==8||family==11||family==12||family==14||family==17||family==18||family==19;
 if(mode==5)sequence=UINT64_C(0x100000000);
 atomic_store(&app.repeated.head,empty?sequence:sequence+1);atomic_store(&app.repeated.tail,sequence);
 uint64_t rx=failed?5*2048:stopped?12*2048:family==7||family==8||family==14?8*2048:(sequence+1)*2048;
 uint64_t tx=failed?4*2048:family==14?7*2048:family==2||family==4?sequence*2048:rx;
 atomic_store(&app.repeated.rx_frames,rx);atomic_store(&app.repeated.tx_frames,tx);
 struct i2s_repeated_slot *slot=&app.repeated.slots[sequence%4];
 slot->period=(struct i2s_repeated_period){sequence+1,sequence*2048,2048,(uint32_t)((sequence+1)*2048)};
 for(unsigned i=0;i<4096;++i)slot->bytes[i]=(uint8_t)(i+sequence);
 atomic_store(&app.result,family>=15&&family<=17?1:failed?3:stopped?2:1);
 if(stopped||(failed&&family<15))atomic_store(&app.repeated.drained,3);
 if(failed)atomic_store(&app.repeated.flags,I2S_REPEATED_OVERRUN);
 if(family==8||family==12||family==17||family==18){packet(2,0,0);tud_vendor_tx_cb(0,response_bytes);i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}}
 packet(4,0,(mode==1||mode==2)?2048:4096);
 check(!i2s_repeated_ack(sequence+1)&&!app.repeated.delivered_ticket,21);
 tud_vendor_tx_cb(0,response_bytes-1);check(!i2s_repeated_ack(sequence+1),22);
 tud_vendor_tx_cb(0,1);check(!response_bytes,23);
 if(family==4||family==6||family==10||family==16){packet(5,(uint32_t)(sequence+1),(uint32_t)((sequence+1)>>32));tud_vendor_tx_cb(0,response_bytes);}
 if(mode>=100){i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}}
 if(mode==2||mode==4){packet(5,mode==4?2:1,0);check(0,28);}
 if(mode==1){
  check(!i2s_repeated_ack(1),24);packet(4,2048,2048);tud_vendor_tx_cb(0,response_bytes);
 }
 check(app.repeated.delivered_bytes==4096&&!i2s_repeated_ack(2),25);
 packet(5,(uint32_t)(sequence+1),(uint32_t)((sequence+1)>>32));check(atomic_load(&app.repeated.tail)==sequence+1&&get32(wire+4)==3,26);
 tud_vendor_tx_cb(0,response_bytes);check(!i2s_repeated_ack(sequence+1),27);
 if(mode==3){packet(5,1,0);check(0,29);}
 i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
