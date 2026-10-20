/* Four adapter/core fault cases only. Accepted controller setup is reused;
 * RTOS/mutex and liveness boundaries are modeled. No device operation runs. */
#if defined(RETENTION_CAMERA)
#include "a733_media_clock.h"
#else
#include "a733_thermal.h"
#endif
#define now fixture_now
#define enter fixture_enter
#define leave fixture_leave
#define retain fixture_retain
#define authorize fixture_authorize
#define main unused_existing_matrix
#if defined(RETENTION_CAMERA)
#include "a733-media-clock.c"
#else
#include "a733-thermal.c"
#endif
#undef main
#undef authorize
#undef retain
#undef leave
#undef enter
#undef now
#if defined(RETENTION_CAMERA)
#include "../apps/camera/capture.c"
struct camera_application app;
#else
#include "../apps/monitor/native.c"
#endif

static struct model *active;
static unsigned healthy,held_fault,context_ok,enter_calls,retain_calls,platform_retains,holder_queries;
static TaskHandle_t task,holder;
static unsigned task_marker,mutex_marker;
#define OWNER ((TaskHandle_t)&task_marker)
#define MUTEX ((SemaphoreHandle_t)&mutex_marker)
TaskHandle_t xTaskGetCurrentTaskHandle(void){return task;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t queue)
{assert(context_ok&&queue==MUTEX);++holder_queries;return holder;}
int a733_platform_lock_enter(struct a733_platform_locks *locks,enum a733_platform_lock which,uint64_t deadline)
{
 assert(locks->initialized&&which==A733_PLATFORM_REGISTER_LOCK&&context_ok&&!holder&&deadline>active->time);
 holder=task;active->held=1;
 if(held_fault)healthy=0; /* Authority fails after the real adapter acquired. */
 return 1;
}
int a733_platform_lock_retain(struct a733_platform_locks *locks,enum a733_platform_lock which)
{
 ++platform_retains;
 if(!locks||!locks->initialized||which!=A733_PLATFORM_REGISTER_LOCK||!context_ok||!task)return 0;
 if(xQueueGetMutexHolder(locks->mutex[which])!=task)return 0;
 assert(!active->retained);active->retained=1;atomic_fetch_or(&locks->retained,1);return 1;
}
#if defined(RETENTION_CAMERA)
int camera_live(void *v){(void)v;return healthy;}
int a733_counter_clock_now(void *v,uint64_t *out)
{assert(v==&app.shared.counter);return fixture_now(active,out);}
static void *cookie(void){return &app;}
static void bind(void)
{
 memset(&app,0,sizeof(app));app.handles[2]=OWNER;app.shared.locks.initialized=1;
 app.shared.locks.mutex[A733_PLATFORM_REGISTER_LOCK]=MUTEX;
}
#else
int monitor_live(void){return healthy;}
static int native_current(void *v)
{struct a733_adc_native *n=v;return n->initialized&&context_ok&&task&&n->owner_task==task;}
static int native_now(void *v,uint64_t *out)
{assert(v==&monitor.owners[1].native);return fixture_now(active,out);}
static int native_enter(void *v,uint64_t deadline)
{struct a733_adc_native *n=v;return native_current(v)?a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline):0;}
static void native_retain(void *v)
{struct a733_adc_native *n=v;assert(native_current(v));++retain_calls;assert(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);}
static struct a733_platform_locks locks;
static void *cookie(void){return &monitor.owners[1];}
static void bind(void)
{
 memset(&monitor,0,sizeof(monitor));memset(&locks,0,sizeof(locks));locks.initialized=1;locks.mutex[A733_PLATFORM_REGISTER_LOCK]=MUTEX;
 struct monitor_owner *o=cookie();o->handle=OWNER;o->in_call=1;o->kind=1;
 o->native.initialized=1;o->native.owner_task=OWNER;o->native.locks=&locks;
 o->native.operations.current=native_current;o->native.operations.now_us=native_now;
 o->native.operations.enter=native_enter;o->native.operations.retain=native_retain;
}
#endif
static int adapter_now(void *v,uint64_t *out)
{assert(v==active);return now(cookie(),out);}
static int adapter_enter(void *v,uint64_t deadline)
{assert(v==active);++enter_calls;if(!held_fault)healthy=0;return enter(cookie(),deadline);}
static void adapter_retain(void *v)
{assert(v==active);retain(cookie());}
int main(void)
{
 for(unsigned uncertain=0;uncertain<2;++uncertain){
  struct model model;active=&model;healthy=1;held_fault=uncertain;context_ok=1;task=OWNER;holder=NULL;
  enter_calls=retain_calls=platform_retains=holder_queries=0;
#if defined(RETENTION_CAMERA)
  setup(&model,A733_MEDIA_VI);
#else
  setup(&model,0,0);
#endif
  bind();
  /* Uninitialized/bad identity and wrong-context guards must not query a mutex. */
  retain(NULL);retain((void *)(uintptr_t)1);
#if defined(RETENTION_CAMERA)
  app.held=1;app.shared.locks.initialized=0;retain(&app);assert(app.held&&!model.retained);
  app.shared.locks.initialized=1;context_ok=0;retain(&app);assert(app.held&&!model.retained);
  context_ok=1;app.held=0;assert(!holder_queries);platform_retains=0;
#else
  struct monitor_owner *o=cookie();o->native.initialized=0;retain(o);o->native.initialized=1;
  o->native.locks=NULL;retain(o);o->native.locks=&locks;
  o->native.owner_task=(void *)(uintptr_t)1;retain(o);o->native.owner_task=OWNER;
  locks.initialized=0;retain(o);locks.initialized=1;
  locks.mutex[0]=NULL;retain(o);locks.mutex[0]=MUTEX;
  context_ok=0;retain(o);context_ok=1;
#endif
  assert(!platform_retains&&!holder_queries&&!retain_calls);
  model.driver.ops.now_us=adapter_now;model.driver.ops.enter=adapter_enter;model.driver.ops.retain=adapter_retain;
#if defined(RETENTION_CAMERA)
  assert(a733_media_clock_poll(&model.driver)==A733_MEDIA_PENDING);
  assert(a733_media_clock_poll(&model.driver)==A733_MEDIA_PENDING);
  assert(a733_media_clock_poll(&model.driver)==A733_MEDIA_RETAINED);
#else
  assert(a733_thermal_poll(&model.driver)==A733_THERMAL_PENDING);
  assert(a733_thermal_poll(&model.driver)==A733_THERMAL_RETAINED);
#endif
  assert(enter_calls==1&&model.driver.fault&&!model.driver.held&&!model.reads&&!model.writes);
  assert(platform_retains==uncertain&&model.retained==uncertain&&holder==(uncertain?OWNER:NULL));
#if defined(RETENTION_CAMERA)
  assert(!app.held);
#else
  assert(retain_calls==uncertain);
#endif
  unsigned retained=platform_retains,queries=holder_queries;
#if defined(RETENTION_CAMERA)
  assert(a733_media_clock_poll(&model.driver)==A733_MEDIA_RETAINED);
#else
  assert(a733_thermal_poll(&model.driver)==A733_THERMAL_RETAINED);
#endif
  assert(!model.reads&&!model.writes&&retained==platform_retains&&queries==holder_queries);
 }
#if defined(RETENTION_CAMERA)
 puts("PASS camera/media: unowned denial skips retention; acquired uncertainty retains once; no later MMIO");
#else
 puts("PASS monitor/thermal: unowned denial skips native retention; acquired uncertainty retains once despite failed health; no later MMIO");
#endif
 return 0;
}
