/* New application boundaries only. Device behavior remains covered by the
 * preserved HDMI native/controller evidence. These fixtures supply modeled
 * launch evidence and scripted leaf completion; none is physical authority. */
#include "../apps/hdmi/application.h"
#include "a733_storage_digest.h"
#include <string.h>
volatile unsigned hdmi_check_result;
static void check(int yes,unsigned code)
{if(!yes){hdmi_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
#if defined(HDMI_CHECK_OUTPUT)
struct hdmi_application app;
static uint64_t clock_now=100;
static unsigned prepares,starts,services,stops,retains,health_error,stop_error;
static TaskHandle_t caller=(TaskHandle_t)1;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return caller;}
int hdmi_live(void *unused){(void)unused;return !atomic_load(&app.fatal)&&clock_now<app.deadline;}
int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=clock_now;return 1;}
int a733_hdmi_fail(struct a733_hdmi_io *io){(void)io;++retains;return -1;}
int a733_hdmi_prepare(struct a733_hdmi *s,uint64_t d)
{check(d==app.deadline,10);++prepares;s->state=A733_HDMI_ACQUIRE;return 1;}
int a733_hdmi_start(struct a733_hdmi *s,uint64_t d)
{check(d==app.deadline&&s->state==A733_HDMI_PREPARED,11);++starts;s->state=A733_HDMI_STARTING;return 1;}
int a733_hdmi_stop(struct a733_hdmi *s,uint64_t d)
{check(d==app.deadline,12);++stops;s->state=A733_HDMI_STOPPING;return stop_error?-1:1;}
int a733_hdmi_service(struct a733_hdmi *s,uint64_t d)
{check(s->state==A733_HDMI_ACTIVE&&d==app.deadline,13);++services;return health_error?-1:1;}
int a733_hdmi_poll(struct a733_hdmi *s)
{
 if(s->state==A733_HDMI_ACQUIRE)s->state=A733_HDMI_PREPARED;
 else if(s->state==A733_HDMI_STARTING)s->state=A733_HDMI_ACTIVE;
 else if(s->state==A733_HDMI_STOPPING)s->state=A733_HDMI_STOPPED_RETAINED;
 else check(0,14);
 return 1;
}
void hdmi_check_run(unsigned mode)
{
 app.handles[1]=(TaskHandle_t)1;app.deadline=20000000;app.stop_at=18000000;
 if(mode==4){caller=(TaskHandle_t)2;check(hdmi_output_step()==-1&&!retains&&!prepares,15);goto done;}
 check(hdmi_output_step()==0&&!prepares,16); /* PMIC has not completed. */
 atomic_store(&app.pmic_ready,1);
 check(hdmi_output_step()==0&&prepares==1&&!starts,17);
 check(hdmi_output_step()==0&&starts==1&&atomic_load(&app.state)==HDMI_APP_PENDING,18);
 check(hdmi_output_step()==1&&atomic_load(&app.state)==HDMI_APP_ACTIVE,19);
 check(hdmi_output_step()==1&&services==1,20);
 if(mode==1)clock_now=app.deadline;
 else if(mode==2)health_error=1;
 else if(mode==3){clock_now=app.stop_at;stop_error=1;}
 else if(mode==5)clock_now=99;
 else clock_now=app.stop_at;
 if(mode==0){
  check(hdmi_output_step()==1&&stops==1&&atomic_load(&app.state)==HDMI_APP_STOPPED_RETAINED,21);
  check(hdmi_output_step()==1&&stops==1&&!retains,22);
 }else{
  check(hdmi_output_step()==-1&&atomic_load(&app.state)==HDMI_APP_FAULT_RETAINED&&retains==1,23);
  unsigned activity=prepares+starts+services+stops;
  check(hdmi_output_step()==-1&&retains==2&&activity==prepares+starts+services+stops,24);
 }
 done:hdmi_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
#elif defined(HDMI_CHECK_ADMISSION)
const unsigned char a733_smp_build_id[32]={1};
void hdmi_check_run(unsigned unused)
{
 (void)unused;struct hdmi_session *s=(void *)A733_STORAGE_SESSION_ADDRESS;
 *s=(struct hdmi_session){.magic=HDMI_SESSION_MAGIC,.version=1,.bytes=sizeof(*s),
  .epoch=1,.generation=2,.lease=3,.image={1},.evidence={2},.analog={3},.power={4},
  .supervision={5},.supervisor=4,.owner=5,.request={.mode=A733_HDMI_720P60,
  .range=A733_HDMI_LIMITED,.initial={.id=1,.generation=2}},.pmic={.owner=6,.identity=2}};
 struct a733_smp_contract c={.magic=HDMI_CONTRACT_MAGIC,.version=3,.bytes=sizeof(c),
  .epoch=1,.generation=2,.inverse_generation=~UINT64_C(2),.lease=3,.build_id={1},
  .armed_at=24,.startup_deadline=24+24000000,.diagnostic_deadline=24+HDMI_WINDOW_TICKS,
  .expires_at=24+HDMI_WINDOW_TICKS,.reserved={4}};
 storage_digest_bytes(s,sizeof(*s),(void *)&c.reserved[2]);
 check(a733_smp_contract_valid(&c,48),30);
 ++s->request.initial.words[0];check(!a733_smp_contract_valid(&c,48),31);--s->request.initial.words[0];
 c.watchdog_mode=1;check(!a733_smp_contract_valid(&c,48),32);c.watchdog_mode=0;
 check(!a733_smp_contract_valid(&c,c.startup_deadline),33);
 s->supervisor=0;storage_digest_bytes(s,sizeof(*s),(void *)&c.reserved[2]);
 check(!a733_smp_contract_valid(&c,48),34);s->supervisor=4;s->pmic.startup_deadline=1;
 storage_digest_bytes(s,sizeof(*s),(void *)&c.reserved[2]);check(!a733_smp_contract_valid(&c,48),35);
 s->pmic.startup_deadline=0;memset(s->analog,0,32);
 storage_digest_bytes(s,sizeof(*s),(void *)&c.reserved[2]);check(!a733_smp_contract_valid(&c,48),36);
 hdmi_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
#elif defined(HDMI_CHECK_NATIVE)
/* Constructor admission only: actual linked native driver, controller and
 * scheduler-state observer. Provider metadata is explicitly modeled; no MMIO
 * callback is invoked and no resource lease is fabricated or acquired. */
static struct a733_hdmi_native n;
static struct a733_hdmi_native_config c;
static struct a733_counter_clock counter;
static struct a733_platform_locks locks;
static struct a733_clock_leases clocks;
static struct a733_rail_leases rails;
static struct a733_gpu_resource_manager pck;
static struct a733_pmic pmic;
static int outside(void *v,const void *p,size_t bytes)
{(void)v;uintptr_t x=(uintptr_t)p,b=(uintptr_t)&pmic;return x+bytes<=b||b+sizeof(pmic)<=x;}
void hdmi_check_run(unsigned mode)
{
 locks.initialized=1;locks.now=a733_counter_clock_now;locks.context=&counter;
 clocks.now=locks.now;clocks.context=&counter;rails.binding.clocks=&clocks;rails.binding.gpu=&pck;
 pmic.self=&pmic;pmic.clocks=&clocks;pmic.rails=&rails;pmic.ops.outside=outside;
 c=(struct a733_hdmi_native_config){.clocks=&clocks,.rails=&rails,.pmic=&pmic,.locks=&locks,
  .pck=&pck,.task=(void *)1,.owner=1,.epoch=2,.recovery=mode==0||mode==2?3:0,
  .request={.mode=mode==4?A733_HDMI_1080P60:A733_HDMI_720P60,.range=A733_HDMI_LIMITED,
    .initial={.id=1,.generation=1}},.recovery_mode=mode?A733_HDMI_SUPERVISED_PATTERN:0,
  .supervision=mode==1||mode==2||mode==4?4:0};
 int r=a733_hdmi_native_init(&n,&c);
 if(mode<2)check(r==1&&n.grant.recovery_epoch==c.recovery&&n.grant.supervision_epoch==c.supervision,50);
 else check(r==0&&!n.self,51);
 hdmi_check_result=1;__asm__ volatile("brk #0");for(;;){}
}

#else
static unsigned mode,retained,io_count;
static int now(void *v,uint64_t *t){(void)v;*t=1;return 1;}
static int acquire(void *v,const struct a733_hdmi_request *q,uint64_t d,struct a733_hdmi_reservation *r)
{
 (void)v;(void)q;(void)d;
 *r=(struct a733_hdmi_reservation){.clock={.id=1,.owner=2,.exclusive=A733_CLOCK_VO1,.shared=A733_CLOCK_OSC},
  .generation=1,.rail_owner=2,.rail_generation=1,.system_owner=2,.system_generation=1,.inactive_epoch=1,
  .recovery_epoch=mode==0||mode==2?1:0,.supervision_epoch=mode==1||mode==2||mode==4?2:0};return 1;
}
static int validate(void *v,const struct a733_hdmi_request *q,const struct a733_hdmi_reservation *r,uint64_t d)
{(void)v;(void)q;(void)r;(void)d;return 1;}
static int retain(void *v,const struct a733_hdmi_reservation *r){(void)v;(void)r;++retained;return 1;}
static int read8(void *v,uintptr_t a,uint8_t *o){(void)v;(void)a;(void)o;++io_count;return -1;}
static int write8(void *v,uintptr_t a,uint8_t o){(void)v;(void)a;(void)o;++io_count;return -1;}
static int read32(void *v,uintptr_t a,uint32_t *o){(void)v;(void)a;(void)o;++io_count;return -1;}
static int write32(void *v,uintptr_t a,uint32_t o){(void)v;(void)a;(void)o;++io_count;return -1;}
static int barrier(void *v){(void)v;++io_count;return -1;}
void hdmi_check_run(unsigned selected)
{
 mode=selected;struct a733_hdmi h={0};
 struct a733_hdmi_config c={.ops={now,acquire,validate,read8,write8,read32,write32,barrier,retain},
  .context=&mode,.request={.mode=selected==4?A733_HDMI_1080P60:A733_HDMI_720P60,
  .range=A733_HDMI_LIMITED,.initial={.id=1,.generation=1}}};
 check(a733_hdmi_init(&h,&c)==1&&a733_hdmi_prepare(&h,100)==1,40);
 int r=a733_hdmi_poll(&h);
 if(mode<2)check(r==0&&h.state==A733_HDMI_PREFLIGHT&&!retained&&!io_count,41);
 else check(r<0&&h.state==A733_HDMI_FAULT_RETAINED&&retained&&!io_count,42);
 hdmi_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
#endif
