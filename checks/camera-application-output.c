/* Actual USB framing and retained output copy; only endpoint I/O is modeled. */
#include "../apps/camera/usb0.c"
struct camera_application app;
static uint8_t pixels[CAMERA_FRAME_BYTES],input[12],wire[4168];
static unsigned input_read,wire_bytes;
volatile unsigned camera_check_result;
void vTaskDelay(TickType_t ticks)
{camera_check_result=100+(unsigned)ticks;__asm__ volatile("brk #0");for(;;){}}
int camera_live(void *v){(void)v;return 1;}
/* The USB failure path must not run in these cases. */
void camera_fail(void *v){(void)v;camera_check_result=90;__asm__ volatile("brk #0");for(;;){}}
uint32_t tud_vendor_n_available(uint8_t i){return i?0:input_read<5?5-input_read:12-input_read;}
uint32_t tud_vendor_n_read(uint8_t i,void *out,uint32_t n)
{if(i||n>12-input_read)return 0;memcpy(out,input+input_read,n);input_read+=n;return n;}
uint32_t tud_vendor_n_write(uint8_t i,const void *p,uint32_t n)
{if(i||wire_bytes+n>sizeof(wire))return 0;if(n>512)n=512;memcpy(wire+wire_bytes,p,n);wire_bytes+=n;return n;}
uint32_t tud_vendor_n_write_flush(uint8_t i){(void)i;return 0;}
static void check(int ok,unsigned code)
{if(!ok){camera_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
void camera_check_run(unsigned mode)
{
 app.session.epoch=7;app.session.generation=8;app.pixels.cpu=(uintptr_t)pixels;
 for(unsigned i=0;i<CAMERA_FRAME_BYTES;++i)pixels[i]=(uint8_t)(i*17);
 atomic_store(&app.result,mode==1?1:2);
 put32(input,mode==2?2:1);put32(input+4,mode==2?0:307199);put32(input+8,mode==2?0:4096);
 exchange();check(request_bytes==5&&!response_bytes,10);exchange();
 unsigned expected=mode?72:4168;check(response_bytes==expected,11);
 for(unsigned i=0;i<9&&response_sent<response_bytes;++i)exchange();
 check(wire_bytes==expected&&response_sent==expected&&response_bytes==expected,12);
 check(get32(wire)==0x524d4143&&get32(wire+4)==1&&get32(wire+8)==(mode==2?3:mode==1?1:2),13);
 check(get32(wire+16)==(mode?0:4096)&&get32(wire+20)==CAMERA_FRAME_BYTES,14);
 check(get32(wire+24)==7&&get32(wire+32)==8,15);
 uint8_t digest[32];storage_digest_bytes(&app.session,sizeof(app.session),digest);
 check(!memcmp(wire+40,digest,32),16);
 if(!mode)for(unsigned i=0;i<4096;++i)check(wire[72+i]==(uint8_t)((307199+i)*17),17);
 tud_vendor_tx_cb(0,expected-1);check(response_bytes==expected,18);
 tud_vendor_tx_cb(0,1);check(!response_bytes,19);
 if(mode==2)check(atomic_load(&app.cancel)==1,20);
 camera_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
