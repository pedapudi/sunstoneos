#include "a733_adc_native.h"
#include "a733_platform_locks.h"
#include "task.h"
#include "a733_mmio.h"
#include "a733_task_context.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "ADC native binding requires scheduler/task/mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_adc_native *n=cookie;
    /* Never admit ISR or critical code, or any task but the owner. */
    return n && n->initialized && a733_task_context_owner_el1((TaskHandle_t)n->owner_task);
}
static int now(void *cookie,uint64_t *v)
{
    struct a733_adc_native *n=cookie;
    return n->locks->now(n->locks->context,v);
}
static int authorize(void *cookie,const struct a733_adc_config *c,const struct a733_clock_token *t)
{struct a733_adc_native *n=cookie;return current(n)?n->admit(n->admission_cookie,c,t):-1;}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_adc_native *n=cookie;
    return current(n) && a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void leave(void *cookie)
{
    struct a733_adc_native *n=cookie;
    a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);
}
static void retain(void *cookie)
{
    struct a733_adc_native *n=cookie;
    configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
static int readable_gpadc(uintptr_t);
static int readable(struct a733_adc_native *n,uintptr_t a)
{
 if(n->lradc){
  switch(a){case 0x07090000:case 0x07090004:case 0x07090008:case 0x0709000c:
  case 0x07090160:case 0x02002000:case 0x020020a0:case 0x020020a8:case 0x020020ac:
  case 0x02002518:case 0x02003024:case 0x02003908:case 0x02003988:
  case 0x03400000:case 0x03400108:case 0x03400208:case 0x03400308:
  case 0x02524000:case 0x02524004:case 0x02524008:case 0x0252400c:return 1;default:return 0;}
 }
 return readable_gpadc(a);
}
static int readable_gpadc(uintptr_t a)
{
 switch(a){
 case 0x07090160:case 0x02002000:case 0x020020a0:case 0x020020a8:case 0x020020ac:
 case 0x02002518:case 0x02002fc0:case 0x02002fc4:case 0x02003908:case 0x02003988:
 case 0x03400000:case 0x03400108:case 0x03400208:case 0x03400308:return 1;
 default:break;}
 if(a<0x02521000||a>0x025210fc||(a&3))return 0;
 switch(a-0x02521000){case 0:case 4:case 8:case 0xc:case 0x10:case 0x20:case 0x24:case 0x28:
 case 0x30:case 0x34:case 0x38:case 0x80:case 0x84:case 0x88:case 0x8c:case 0xd0:case 0xdc:case 0xfc:return 1;default:return 0;}
}
static int allowed(struct a733_adc_native *n)
{return current(n)&&!(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u)&&
 xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;}
static int read_register(void *cookie,uintptr_t a,uint32_t *out)
{
 struct a733_adc_native *n=cookie;if(!out||!readable(n,a)||!allowed(n))return 0;
 *out=a733_mmio_read32_dsb_sy_both(a);return 1;
}
static int write_register(void *cookie,uintptr_t a,uint32_t value)
{
 struct a733_adc_native *n=cookie;if(!allowed(n))return 0;uint32_t mask;
 if(n->lradc){
  switch(a){
  case 0x02003024:mask=0x10001;break;
  case 0x02524000:mask=0xffcf3ffd;if((value&mask)!=0x0100004c&&(value&mask)!=0x0100004d)return 0;break;
  case 0x02524004:mask=0x1f1f;if(value&mask&~1u)return 0;break;
  case 0x02524008:
   if(value!=1)return 0;
   a733_mmio_write32_dsb_sy_both(a,value);return 1;
  default:return 0;
  }
  a733_mmio_dsb_sy();uint32_t old=a733_mmio_read32(a);
  if((old^value)&~mask)return 0;
  if(a==0x02003024&&(old&~value&mask))return 0;
  a733_mmio_write32(a,value);a733_mmio_dsb_sy();return 1;
 }

 switch(a){
 case 0x02002fc0:mask=0x8700001f;if((value&mask)!=0x81000000&&(value&mask)!=0x01000000&&(value&mask)!=0x80000000&&(value&mask)!=0)return 0;break;
 case 0x02002fc4:mask=0x10001;break;
 case 0x02521000:if(value!=0x5dbf095f)return 0;mask=UINT32_MAX;break;
 case 0x02521004:mask=0x000f0000;if((value&mask)!=0x80000&&(value&mask)!=0x90000)return 0;break;
 case 0x02521008:mask=0x007f007f;if((value&mask)!=15&&(value&mask)!=0)return 0;break;
 case 0x02521028:mask=127;if((value&mask)!=15&&(value&mask)!=0)return 0;break;
 case 0x025210d0:mask=1;if(!(value&1))return 0;break;
 case 0x02521038:
  if(!value||(value&~15u))return 0;
  a733_mmio_write32_dsb_sy_both(a,value);return 1;
 default:return 0;
 }
 a733_mmio_dsb_sy();uint32_t old=a733_mmio_read32(a);
 if((old^value)&~mask)return 0;
 if(a==0x02002fc0&&(old&0x80000000u)&&((old^value)&mask))return 0;
 if(a==0x02002fc4&&(old&~value&mask))return 0;
 a733_mmio_write32(a,value);a733_mmio_dsb_sy();return 1;
}
int a733_adc_native_init(struct a733_adc_native *n,struct a733_platform_locks *locks,void *task,
 int (*admit)(void *,const struct a733_adc_config *,const struct a733_clock_token *),void *cookie)
{
 if(!n||n->initialized||!locks||!locks->initialized||!task||!admit)return 0;
 n->locks=locks;n->owner_task=task;n->admit=admit;n->admission_cookie=cookie;
 n->lradc=0;
 n->operations=(struct a733_adc_ops){now,current,authorize,enter,leave,retain,read_register,write_register};n->initialized=1;return 1;
}

int a733_lradc_native_init(struct a733_adc_native *n,struct a733_platform_locks *locks,void *task,
 int (*admit)(void *,const struct a733_adc_config *,const struct a733_clock_token *),void *cookie)
{
 if(!a733_adc_native_init(n,locks,task,admit,cookie))return 0;
 n->lradc=1;return 1;
}
