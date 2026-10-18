/* Actual app parent preparation, lease adapter and native MMIO adapter.
 * Register storage, task identity and stage-1 translation are modeled. */
#include "../apps/i2s/main.c"
volatile unsigned i2s_check_result;
static TaskHandle_t current=(TaskHandle_t)1;
static uint64_t instant=100;
static unsigned held;
uint64_t a733_smp_counter(void){return instant*24;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=++instant;return 1;}
int a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock kind,uint64_t d)
{(void)p;(void)kind;if(held||instant>=d)return 0;held=1;return 1;}
void a733_platform_lock_leave(struct a733_platform_locks *p,enum a733_platform_lock kind)
{(void)p;(void)kind;held=0;}
int aarch64_device_translate(uintptr_t a,int write,struct aarch64_translation *t)
{(void)write;t->physical=a;t->attributes=0;return 1;}
_Noreturn void platform_panic(const char *s,unsigned line)
{(void)s;i2s_check_result=100+line;__asm__ volatile("brk #0");for(;;){}}
static void check(int ok,unsigned code){if(!ok){i2s_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
void i2s_check_run(unsigned mode)
{
 (void)mode;app.deadline=100000;app.handles[2]=(TaskHandle_t)1;app.dma.service.worker=(TaskHandle_t)2;
 *(volatile uint32_t *)0x02002000=0xf8000000u|(99u<<8)|(99u<<16);
 *(volatile uint32_t *)0x02002280=0xf8047f00;
 *(volatile uint32_t *)0x0200391c=0x60006;*(volatile uint32_t *)0x0200399c=0x60000;
 check(a733_clock_leases_init(&app.shared.clocks,a733_counter_clock_now,&app.shared.counter),10);
 struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=now,.read=read_reg,.write=write_reg};
 check(a733_audio_leases_init(&app.leases,&app.shared.clocks,7,&backend,&app),11);
 check(a733_audio_parent_init(&app.parent,&app.leases.operations,&app.leases,0,app.deadline),12);
 int r=0;for(unsigned n=0;n<20&&!r;++n)r=i2s_prepare_audio();
 check(r==1&&a733_audio_parent_ready(&app.parent)&&atomic_load(&app.audio_ready),13);
 uint64_t t;check(!now(&app,&t),14);current=(TaskHandle_t)2;check(now(&app,&t),15);
 check(enter(&app,app.deadline),16);
 unsigned offsets[]={12,40,52,68,72};uint32_t value;
 for(unsigned i=0;i<5;++i){uint32_t v=i?0:6;
  check(write_reg(&app,0x02532000+offsets[i],v),20+i);
  check(read_reg(&app,0x02532000+offsets[i],&value)&&value==v,30+i);
 }
 check(read_reg(&app,0x02532018,&value),40);
 check(!write_reg(&app,0x02532018,0)&&!write_reg(&app,0x0253200c,0x80),41);
 check(!write_reg(&app,0x02532020,0)&&!read_reg(&app,0x02532020,&value),42);
 check(write_reg(&app,0x0253200c,0x60),43);leave(&app);
 i2s_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
