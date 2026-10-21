#include "a733_pwm_native.h"
#include "a733_platform_locks.h"
#include "task.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error PWM native binding requires scheduler and mutex identity
#endif
static int current(void *v)
{
 struct a733_pwm_native *n=v;uint64_t el,flags,nesting,critical;
 __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif":"=r"(el),"=r"(flags));
 if(!n||!n->initialized||el!=4||(flags&0x80))return 0;
 __asm__ volatile("msr daifset, #2":::"memory");
#if configNUMBER_OF_CORES > 1
 extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
 uint64_t core;__asm__ volatile("mrs %0, tpidr_el1":"=r"(core));
 nesting=core<configNUMBER_OF_CORES?ullPortInterruptNestings[core]:1;
 critical=core<configNUMBER_OF_CORES?ullCriticalNestings[core]:1;
#else
 extern uint64_t ullPortInterruptNesting;extern volatile uint64_t ullCriticalNesting;
 nesting=ullPortInterruptNesting;critical=ullCriticalNesting;
#endif
 __asm__ volatile("msr daif, %0"::"r"(flags):"memory");
 return !nesting&&!critical&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->task;
}
static int now(void *v,uint64_t *n)
{struct a733_pwm_native *p=v;return p->locks->now(p->locks->context,n);}
static int admit(void *v,const struct a733_pwm_config *c,const struct a733_clock_token *t)
{struct a733_pwm_native *n=v;return current(n)?n->admit(n->admission_cookie,c,t):-1;}
static int enter(void *v,uint64_t deadline)
{struct a733_pwm_native *n=v;return current(n)?a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline):-1;}
static void leave(void *v)
{struct a733_pwm_native *n=v;a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v)
{struct a733_pwm_native *n=v;configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);}
static int held(struct a733_pwm_native *n)
{return current(n)&&!(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u)&&xSemaphoreGetMutexHolder(n->locks->mutex[0])==(TaskHandle_t)n->task;}
static int readable(uintptr_t a)
{
 switch(a){
 case 0x0701000c:case 0x02527000:case 0x02527004:case 0x02527010:case 0x02527014:case 0x02527028:
 case 0x02527040:case 0x02527068:case 0x02527080:case 0x02527090:case 0x02527094:
 case 0x02527098:case 0x0252709c:case 0x025270c0:case 0x02527240:case 0x02527244:
 case 0x02527248:case 0x0252724c:case 0x02002784:case 0x07090160:case 0x02002000:
 case 0x02002510:case 0x020020a0:case 0x020020a8:case 0x020020ac:case 0x02003908:
 case 0x02003988:case 0x02000040:case 0x02000048:case 0x03400000:case 0x03400108:
 case 0x03400208:case 0x03400308:case 0x03400088:case 0x03400d08:case 0x03400c10:
 case 0x03400440:case 0x03406218:case 0x0340621c:return 1;
 default:return 0;
 }
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
 if(!out||!readable(a)||!held(v))return 0;
 __asm__ volatile("dsb sy":::"memory");*out=*(volatile const uint32_t *)a;__asm__ volatile("dsb sy":::"memory");return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
 struct a733_pwm_native *n=v;if(!held(n))return 0;
 switch(a){
 case 0x02002784:{uint32_t old=*(volatile const uint32_t *)a;if((old^value)&~UINT32_C(0x10001))return 0;break;}
 case 0x02527000:case 0x02527040:case 0x02527080:if(value&&value!=(1u<<5))return 0;break;
 case 0x02527004:if(value!=(1u<<5))return 0;break;
 case 0x02527240:if(value!=n->plan.pcr&&(!n->plan.duration_us||value!=(n->plan.pcr|(1u<<10))))return 0;break;
 case 0x02527244:if(n->plan.duration_us?value!=n->plan.ppr:((value>>16)!=n->plan.ticks-1||(value&65535)>n->plan.ticks))return 0;break;
 case 0x03400108:case 0x03400188:case 0x03400288:if(n->plan.duration_us||value!=8)return 0;break;
 case 0x02527248:if(value)return 0;break;
 default:return 0;
 }
 __asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)a=value;__asm__ volatile("dsb sy":::"memory");return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
static int outside(void *v,const void *p,size_t bytes)
{struct a733_pwm_native *n=v;return n&&separate(n,sizeof(*n),p,bytes)&&separate(n->locks,sizeof(*n->locks),p,bytes);}
int a733_pwm_native_init(struct a733_pwm_native *n,struct a733_platform_locks *locks,void *task,
 const struct a733_pwm_config *c,int (*admission)(void *,const struct a733_pwm_config *,const struct a733_clock_token *),void *cookie)
{
 struct a733_pwm_plan plan;a733_clock_mask_t shared;
 if(!n||n->initialized||!locks||!locks->initialized||!task||!admission||!a733_pwm_plan(c,&plan,&shared))return 0;
 *n=(struct a733_pwm_native){.locks=locks,.task=task,.admit=admission,.admission_cookie=cookie,.plan=plan,
 .operations={now,current,admit,enter,leave,retain,read32,write32,outside},.initialized=1};return 1;
}
