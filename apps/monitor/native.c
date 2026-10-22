#include "monitor.h"
#include "boot.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include <string.h>
struct monitor_application monitor;
static int access(struct monitor_owner *o)
{return (o==&monitor.owners[0]||o==&monitor.owners[1])&&o->in_call&&monitor_live()&&o->native.operations.current(&o->native)==1;}
static int now(void *v,uint64_t *out)
{struct monitor_owner *o=v;return access(o)&&o->native.operations.now_us(&o->native,out)==1;}
static int current(void *v){return access(v);}
static int enter(void *v,uint64_t d)
{struct monitor_owner *o=v;return access(o)?o->native.operations.enter(&o->native,d):-1;}
static void leave(void *v)
{struct monitor_owner *o=v;o->native.operations.leave(&o->native);}
static void retain(void *v)
{
 struct monitor_owner *o=v;
 if((o!=&monitor.owners[0]&&o!=&monitor.owners[1])||!o->handle||
    !o->native.initialized||o->native.owner_task!=o->handle||!o->native.locks||
    !o->native.locks->initialized||!o->native.operations.current||
    !o->native.operations.retain||o->native.operations.current(&o->native)!=1)return;
 /* Native current establishes normal task context before the holder query.
  * Do not require access(): health may fail while this task still owns it. */
 SemaphoreHandle_t mutex=o->native.locks->mutex[A733_PLATFORM_REGISTER_LOCK];
 if(mutex&&xSemaphoreGetMutexHolder(mutex)==o->handle)o->native.operations.retain(&o->native);
}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{
 struct monitor_owner *o=v;if(!access(o))return 0;
 int ok=o->kind?(monitor.thermal.held&&xSemaphoreGetMutexHolder(providers.shared.locks.mutex[A733_PLATFORM_REGISTER_LOCK])==o->handle&&a733_thermal_native_read(NULL,a,out)):
 o->native.operations.read(&o->native,a,out);
 return ok==1&&access(o);
}
static int write_register(void *v,uintptr_t a,uint32_t value)
{
 struct monitor_owner *o=v;if(!access(o)||a==0x02002fc0)return 0;
 int ok=o->kind?(monitor.thermal.held&&xSemaphoreGetMutexHolder(providers.shared.locks.mutex[A733_PLATFORM_REGISTER_LOCK])==o->handle&&a733_thermal_native_write(NULL,a,value)):
 o->native.operations.write(&o->native,a,value);
 return ok==1&&access(o);
}
static int adc_admit(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{
 struct monitor_owner *o=v;
 return o==&monitor.owners[0]&&access(o)&&c==&monitor.adc.config&&
 c->owner==monitor.adc_config.owner&&c->handoff==monitor.adc_config.handoff&&
 c->startup_deadline_us==monitor.adc_config.startup_deadline_us&&c->apb1==monitor.adc_config.apb1&&
 c->apb1_hz==monitor.adc_config.apb1_hz&&!c->configure_adc_clock&&
 t->id==o->token.id&&t->owner==o->token.owner&&t->shared==o->token.shared&&t->exclusive==o->token.exclusive;
}
static int thermal_admit(void *v,const struct a733_thermal_config *c,const struct a733_clock_token *t)
{
 struct monitor_owner *o=v;
 return o==&monitor.owners[1]&&access(o)&&c==&monitor.thermal.config&&
 c->owner==monitor.thermal_config.owner&&c->handoff==monitor.thermal_config.handoff&&
 c->startup_deadline_us==monitor.thermal_config.startup_deadline_us&&c->apb1==monitor.thermal_config.apb1&&
 c->apb1_hz==monitor.thermal_config.apb1_hz&&!c->configure_adc_clock&&c->alarm_mask==31&&c->critical_mask==31&&
 !memcmp(c->limits,monitor.thermal_config.limits,sizeof(c->limits))&&
 t->id==o->token.id&&t->owner==o->token.owner&&t->shared==o->token.shared&&t->exclusive==o->token.exclusive;
}
static int native_admit(void *v,const struct a733_adc_config *c,const struct a733_clock_token *t)
{(void)v;(void)c;(void)t;return 0;} /* Used only for actual task/MMIO binding. */
static const struct a733_mapping_rule device_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int mappings(void)
{
 const uintptr_t pages[]={0x02002000,0x02003000,0x02521000,0x02522000,0x03006000,0x03400000,0x07090000};
 for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);++i)
  if(!a733_mapping_check(aarch64_device_translate,pages[i],pages[i],&device_rule))return 0;
 return 1;
}
int monitor_construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!mappings())return 0;
 atomic_init(&monitor.state,MONITOR_BOOT);atomic_init(&monitor.parent_ready,0);atomic_init(&monitor.active_mask,0);
 uint64_t d=board.session.deadline_us-10000;
 for(unsigned i=0;i<2;++i){struct monitor_owner *o=&monitor.owners[i];
  if(!o->handle||!a733_adc_native_init(&o->native,&providers.shared.locks,o->handle,native_admit,o))return 0;
  atomic_init(&o->pending,0);o->masked=1;o->irq=(struct gicv3_spi){0x03400000,i?71:70};}
 monitor.adc_config=(struct a733_adc_config){monitor.session.owner,monitor.session.authority,d,monitor.session.apb1,monitor.session.apb1_hz,0};
 monitor.thermal_config=(struct a733_thermal_config){.owner=monitor.session.thermal_owner,.handoff=monitor.session.authority,
  .startup_deadline_us=d,.apb1=monitor.session.apb1,.apb1_hz=monitor.session.apb1_hz,.alarm_mask=31,.critical_mask=31};
 for(unsigned i=0;i<5;++i)monitor.thermal_config.limits[i]=(struct a733_thermal_limits){monitor.session.limits[i][0],monitor.session.limits[i][1],monitor.session.limits[i][2]};
 monitor.adc_ops=(struct a733_adc_ops){now,current,adc_admit,enter,leave,retain,read_register,write_register};
 monitor.thermal_ops=(struct a733_thermal_ops){now,thermal_admit,enter,leave,retain,read_register,write_register};
 storage_digest_bytes(&monitor.session,sizeof(monitor.session),monitor.session_digest);return 1;
}
