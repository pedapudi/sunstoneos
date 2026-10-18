/* Actual USB framing and retained output copy; only endpoint I/O is modeled. */
#include "../apps/i2s/usb0.c"
struct i2s_application app;
static uint8_t input[12],wire[4168];
static unsigned input_read,wire_bytes;
volatile unsigned i2s_check_result;
void vTaskDelay(TickType_t ticks)
{i2s_check_result=100+(unsigned)ticks;__asm__ volatile("brk #0");for(;;){}}
int i2s_live(void *v){(void)v;return 1;}
/* This framing fixture treats any terminal USB path as failure. The separate
 * cutoff fixture executes the actual metadata revocation implementation. */
void i2s_fail(void *v){(void)v;i2s_check_result=90;}
uint32_t tud_vendor_n_available(uint8_t i){return i?0:input_read<5?5-input_read:12-input_read;}
uint32_t tud_vendor_n_read(uint8_t i,void *out,uint32_t n)
{if(i||n>12-input_read)return 0;memcpy(out,input+input_read,n);input_read+=n;return n;}
uint32_t tud_vendor_n_write(uint8_t i,const void *p,uint32_t n)
{if(i||wire_bytes+n>sizeof(wire))return 0;if(n>512)n=512;memcpy(wire+wire_bytes,p,n);wire_bytes+=n;return n;}
uint32_t tud_vendor_n_write_flush(uint8_t i){(void)i;return 0;}
static void check(int ok,unsigned code)
{if(!ok){i2s_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
void i2s_check_run(unsigned mode)
{
 app.admitted.epoch=7;app.admitted.generation=8;
 for(unsigned i=0;i<I2S_PCM_BYTES;++i)app.output[i]=(uint8_t)(i*17);
 atomic_store(&app.result,mode==1?1:2);
 put32(input,mode==2?2:1);put32(input+4,mode==2?0:8191);put32(input+8,mode==2?0:4096);
 exchange();check(request_bytes==5&&!response_bytes,10);exchange();
 unsigned expected=mode?72:4168;check(response_bytes==expected,11);
 for(unsigned i=0;i<9&&response_sent<response_bytes;++i)exchange();
 check(wire_bytes==expected&&response_sent==expected&&response_bytes==expected,12);
 check(get32(wire)==0x53493241&&get32(wire+4)==1&&get32(wire+8)==(mode==2?3:mode==1?1:2),13);
 check(get32(wire+16)==(mode?0:4096)&&get32(wire+20)==I2S_PCM_BYTES,14);
 check(get32(wire+24)==7&&get32(wire+32)==8,15);
 uint8_t digest[32];storage_digest_bytes(&app.admitted,sizeof(app.admitted),digest);
 check(!memcmp(wire+40,digest,32),16);
 if(!mode)for(unsigned i=0;i<4096;++i)check(wire[72+i]==(uint8_t)((8191+i)*17),17);
 tud_vendor_tx_cb(0,expected-1);check(response_bytes==expected,18);
 tud_vendor_tx_cb(0,1);check(!response_bytes,19);
 if(mode==2)check(atomic_load(&app.cancel)==1,20);
 i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
