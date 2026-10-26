#include "application.h"
#include "a733_media_clock_native.h"
#include "a733_memory_reservations.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include <string.h>
static int current(void)
{return xTaskGetCurrentTaskHandle()==app.handles[2]&&camera_live(NULL)&&
 !atomic_load_explicit(&app.cancel,memory_order_acquire);}
static int now(void *v,uint64_t *out)
{return v==&app&&current()&&a733_counter_clock_now(&app.shared.counter,out)==1;}
static int authorize(void *v,const struct a733_media_clock_config *c,
 const struct a733_clock_token *t,const struct a733_gpu_resource_lease *p)
{
 if(v!=&app||!current()||c!=&app.media.config||
    memcmp(c,&app.session.media,sizeof(*c))||
    memcmp(&app.camera.boot,&app.session.camera,sizeof(app.camera.boot))||
    !camera_session_valid(
#ifdef SUNSTONE_CAMERA_SUPERVISED
     &app.admitted,
#else
     &app.session,
#endif
     a733_smp_counter())||
    t->owner!=app.session.camera.owner)return -1;
 enum a733_clock_result clock=a733_clock_token_status(&app.shared.clocks,t);
 enum a733_gpu_resource_result pck=a733_gpu_resources_status(&app.shared.gpu,app.session.pck_client,p);
 enum a733_rail_result rail=a733_rail_status(&app.shared.rails,&app.rail);
 if(clock==A733_CLOCK_BUSY||pck==A733_GPU_RESOURCE_BUSY||rail==A733_RAIL_BUSY)return 0;
 if(clock!=A733_CLOCK_OK||pck!=A733_GPU_RESOURCE_OK||rail!=A733_RAIL_READY)return -1;
 /* These physical conditions are admitted by the protected session, bound to
  * this build/epoch/lease. MMIO cannot prove inherited drain or external reset. */
 return 1;
}
static int enter(void *v,uint64_t deadline)
{
 if(v!=&app||!current()||app.held)return -1;
 int r=a733_platform_lock_enter(&app.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline);
 if(r==1)app.held=1;return r;
}
static void leave(void *v)
{configASSERT(v==&app&&app.held);a733_platform_lock_leave(&app.shared.locks,A733_PLATFORM_REGISTER_LOCK);app.held=0;}
static void retain(void *v)
{
 /* A denied enter can be unowned. The provider checks normal task context and
  * actual mutex ownership independently of the health predicate that failed. */
 if(v==&app&&app.held&&
    a733_platform_lock_retain(&app.shared.locks,A733_PLATFORM_REGISTER_LOCK)==1)app.held=0;
}
static int mapped(uintptr_t a)
{
 /* Device identity mapping for reads and writes, any shareability. */
 static const struct a733_mapping_rule rule={A733_MAPPING_READ_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
 return a733_mapping_check(aarch64_device_translate,a,a,&rule);
}
static int read_word(void *v,uintptr_t a,uint32_t *out)
{return v==&app&&app.held&&current()&&mapped(a)&&a733_media_clock_native_read(NULL,a,out)==1;}
static int write_word(void *v,uintptr_t a,uint32_t value)
{return v==&app&&app.held&&current()&&mapped(a)&&a733_media_clock_native_write(NULL,a,value)==1;}
static int fault(void)
{
 /* No close/release operation proves drain. Retain every allocation and lease.
  * Cancellation stops producer advancement without attempting unsafe bus STOP. */
#ifdef SUNSTONE_CAMERA_STREAM
 if(app.output_ready){
  app.retained_status=4u|(app.camera.capture.sensor.state==OV5640_READY?1u:0u);
  atomic_store_explicit(&app.result,4,memory_order_release);return -1;
 }
#endif
 atomic_store_explicit(&app.result,3,memory_order_release);return -1;
}
int camera_capture_step(void)
{
 if(atomic_load_explicit(&app.result,memory_order_acquire)>=3)return -1;
 if(!current())return fault();
 if(atomic_load_explicit(&app.result,memory_order_acquire)==2)return 1;
 if(!atomic_load_explicit(&app.shared.pmic_ready,memory_order_acquire))return 0;
 uint64_t owner=app.session.camera.owner,deadline=app.session.deadline_us;
 switch(app.stage){
 case 0:{
  a733_rail_mask_t mask=0;
  for(unsigned i=0;i<3;++i){if((unsigned)app.session.camera.rails[i]>=A733_RAIL_COUNT)return fault();mask|=A733_RAIL_BIT(app.session.camera.rails[i]);}
  enum a733_rail_result r=a733_rail_acquire(&app.shared.rails,owner,mask,0,deadline,&app.rail);
  if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_PENDING)return fault();
  ++app.stage;return 0;}
 case 1:{
  enum a733_rail_result r=a733_rail_poll(&app.shared.rails,&app.rail);
  if(r==A733_RAIL_PENDING||r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return fault();
  ++app.stage;return 0;}
 case 2:{
  uint64_t token;struct a733_gpu_power_resources ready;
  enum a733_gpu_resource_result r=a733_gpu_resources_acquire(&app.shared.gpu,app.session.pck_client,
   A733_GPU_RESOURCE_PCK,deadline,&token,&ready);
  if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return fault();
  struct a733_gpu_resource_lease pck=app.shared.gpu.leases[app.session.pck_client];
  struct a733_media_clock_ops ops={now,authorize,enter,leave,retain,read_word,write_word};
  if(!a733_media_clock_init(&app.media,&app.session.media,&ops,&app,&app.shared.clocks,
     &app.shared.gpu,app.session.pck_client,&pck))return fault();
  atomic_store_explicit(&app.result,1,memory_order_release);++app.stage;return 0;}
 case 3:{
  int r=a733_camera_native_poll(&app.camera);
  if(r<0)return fault();if(!r)return 0;
#ifdef SUNSTONE_CAMERA_STREAM
  r=a733_camera_native_stream_copy(&app.camera,app.output);
  if(r<0)return fault();if(!r)return 0;
  app.output_ready=1;
  if(a733_camera_native_stream_stop(&app.camera)!=1)return fault();
  app.stage=7;return 0;
#else
  ++app.stage;return 0;
#endif
 }
#ifdef SUNSTONE_CAMERA_STREAM
 case 7:
  /* Sensor stop advances, then the absent packet/drain proof retains. */
  if(a733_camera_native_poll(&app.camera)<0)return fault();
  return 0;
#endif
 case 4:case 5:{
  unsigned plane=app.stage-4;
  if(a733_camera_native_copy(&app.camera,plane,0,app.output,
      plane*A733_CAMERA_NATIVE_PLANE_BYTES,A733_CAMERA_NATIVE_PLANE_BYTES)!=1)return fault();
  ++app.stage;return 0;}
 case 6:
  if(!current())return fault();
  atomic_store_explicit(&app.result,2,memory_order_release);return 1;
 default:return fault();
 }
}
void camera_capture_task(void *unused)
{(void)unused;for(;;){(void)camera_capture_step();vTaskDelay(1);}}
int camera_output(unsigned offset,void *out,unsigned bytes)
{
 if(!out||bytes>4096||offset>CAMERA_FRAME_BYTES||bytes>CAMERA_FRAME_BYTES-offset||
    !camera_live(NULL)||atomic_load_explicit(&app.cancel,memory_order_acquire)||
    (atomic_load_explicit(&app.result,memory_order_acquire)!=2 &&
     atomic_load_explicit(&app.result,memory_order_acquire)!=4))return 0;
 /* Output is CPU-owned and immutable; the USB task does not access allocator
  * metadata concurrently with the owner. Publication follows both plane copies. */
 memcpy(out,(const void *)(app.pixels.cpu+offset),bytes);
 return camera_live(NULL)&&!atomic_load_explicit(&app.cancel,memory_order_acquire)&&
  (atomic_load_explicit(&app.result,memory_order_acquire)==2 ||
   atomic_load_explicit(&app.result,memory_order_acquire)==4);
}
