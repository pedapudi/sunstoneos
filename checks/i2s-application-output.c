/* Actual USB framing and retained output copy; only endpoint I/O is modeled. */
#include "../apps/i2s/usb0.c"
struct i2s_application app;
static uint8_t input[12],wire[4192];
static unsigned input_read,wire_bytes;
volatile unsigned i2s_check_result;
void vTaskDelay(TickType_t ticks)
{i2s_check_result=100+(unsigned)ticks;__asm__ volatile("brk #0");for(;;){}}
int i2s_live(void *v){(void)v;return 1;}
/* Only the file upload path reads the counter; these modes never reach it. */
uint64_t a733_smp_counter(void){return 0;}
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
 unsigned duplex=mode>=3,base=duplex?96:72;
 app.admitted.epoch=7;app.admitted.generation=8;
 if(duplex){
  if(((const struct i2s_session *)A733_STORAGE_SESSION_ADDRESS)->magic==I2S_SESSION_MAGIC)
   app.admitted=*(const struct i2s_session *)A733_STORAGE_SESSION_ADDRESS;
  app.admitted.direction=app.session.direction=2;
  app.output_bytes=app.playback_bytes=I2S_PCM_BYTES;app.periods=app.playback_periods=4;app.duplex_drained=3;
 }
 for(unsigned i=0;i<I2S_PCM_BYTES;++i)app.output[i]=(uint8_t)(i*17);
 atomic_store(&app.result,mode==1?1:2);
 put32(input,mode==2?2:1);put32(input+4,duplex?(mode-3)*4096:mode==2?0:8191);put32(input+8,mode==2?0:4096);
 exchange();check(request_bytes==5&&!response_bytes,10);exchange();
 unsigned expected=duplex?4192:mode?72:4168;check(response_bytes==expected,11);
 for(unsigned i=0;i<9&&response_sent<response_bytes;++i)exchange();
 check(wire_bytes==expected&&response_sent==expected&&response_bytes==expected,12);
 check(get32(wire)==0x53493241&&get32(wire+4)==(duplex?2u:1u)&&get32(wire+8)==(mode==2?3:mode==1?1:2),13);
 check(get32(wire+16)==(duplex?4096u:mode?0u:4096u)&&get32(wire+20)==I2S_PCM_BYTES,14);
 check(get32(wire+24)==app.admitted.epoch&&get32(wire+32)==app.admitted.generation,15);
 uint8_t digest[32];storage_digest_bytes(&app.admitted,sizeof(app.admitted),digest);
 check(!memcmp(wire+40,digest,32),16);
 if(!mode||duplex)for(unsigned i=0;i<4096;++i)
  check(wire[base+i]==(uint8_t)((get32(input+4)+i)*17),17);
 if(duplex)check(get32(wire+72)==16384&&get32(wire+76)==16384&&get32(wire+80)==4&&
  get32(wire+84)==4&&get32(wire+88)==3&&!get32(wire+92),21);
 tud_vendor_tx_cb(0,expected-1);check(response_bytes==expected,18);
 tud_vendor_tx_cb(0,1);check(!response_bytes,19);
 if(mode==2)check(atomic_load(&app.cancel)==1,20);
 i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
