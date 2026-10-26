/* Application publication and retained cancellation boundary. Native controller
 * execution is covered by the unchanged camera-native component fixture. */
#include "../apps/camera/application.h"
#include "a733_media_clock_native.h"
#include "aarch64_device_memory.h"
#include <string.h>
struct camera_application app;
volatile unsigned camera_check_result;
static unsigned copies,fail_copy,polled,admitted,busy_resource;
static uint8_t pixels[CAMERA_FRAME_BYTES],chunk[4096];
static uint64_t ticks=240;
uint64_t a733_smp_counter(void){return ticks;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)1;}
int camera_live(void *v){(void)v;return !atomic_load(&app.fatal)&&ticks/24<app.session.deadline_us;}
void camera_fail(void *v){(void)v;atomic_store(&app.fatal,1);}
int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=ticks/24;return 1;}
int camera_session_valid(const struct camera_session *s,uint64_t n){(void)s;(void)n;return admitted;}
#ifdef SUNSTONE_CAMERA_STREAM
static unsigned stream_stopping,stream_mode;
#define COPY_STEPS ((CAMERA_FRAME_BYTES+4095)/4096)
int a733_camera_native_poll(struct a733_camera_native *n)
{
 ++polled;
 if(stream_stopping){n->capture.sensor.state=stream_mode==1?OV5640_STREAMING:OV5640_READY;return -1;}
 return 1;
}
int a733_camera_native_stream_copy(struct a733_camera_native *n,struct device_memory_lease l)
{
 (void)n;(void)l;
 if(polled!=copies+1 || copies>=COPY_STEPS)return -1;
 unsigned offset=4096*copies,count=CAMERA_FRAME_BYTES-offset;if(count>4096)count=4096;
 memset(pixels+offset,0x41,count);++copies;
 return stream_mode==2&&copies==2?-1:copies==COPY_STEPS;
}
int a733_camera_native_stream_stop(struct a733_camera_native *n)
{(void)n;if(copies!=COPY_STEPS)return -1;stream_stopping=1;return 1;}
#else
int a733_camera_native_poll(struct a733_camera_native *n){(void)n;++polled;return 1;}
#endif
int a733_camera_native_copy(struct a733_camera_native *n,unsigned plane,unsigned offset,
 struct device_memory_lease l,size_t destination,size_t bytes)
{
 (void)n;(void)l;++copies;if(offset||plane>1||destination!=plane*A733_CAMERA_NATIVE_PLANE_BYTES||bytes!=A733_CAMERA_NATIVE_PLANE_BYTES)return -1;
 memset(pixels+destination,(int)(plane+0x41),bytes);
 return copies==fail_copy?-1:1;
}
enum a733_rail_result a733_rail_acquire(struct a733_rail_leases *a,uint64_t b,a733_rail_mask_t c,
 a733_rail_mask_t d,uint64_t e,struct a733_rail_token *f)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return A733_RAIL_INVALID;}
enum a733_rail_result a733_rail_poll(struct a733_rail_leases *a,const struct a733_rail_token *b)
{(void)a;(void)b;return A733_RAIL_INVALID;}
enum a733_rail_result a733_rail_status(struct a733_rail_leases *a,const struct a733_rail_token *b)
{(void)a;(void)b;return busy_resource==3?A733_RAIL_BUSY:A733_RAIL_READY;}
enum a733_gpu_resource_result a733_gpu_resources_acquire(struct a733_gpu_resource_manager *a,
 unsigned b,uint32_t c,uint64_t d,uint64_t *e,struct a733_gpu_power_resources *f)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return A733_GPU_RESOURCE_INVALID;}
enum a733_gpu_resource_result a733_gpu_resources_status(struct a733_gpu_resource_manager *a,
 unsigned b,const struct a733_gpu_resource_lease *c)
{(void)a;(void)b;(void)c;return busy_resource==2?A733_GPU_RESOURCE_BUSY:A733_GPU_RESOURCE_OK;}
enum a733_clock_result a733_clock_token_status(struct a733_clock_leases *a,const struct a733_clock_token *b)
{(void)a;(void)b;return busy_resource==1?A733_CLOCK_BUSY:A733_CLOCK_OK;}
int a733_media_clock_init(struct a733_media_clock *a,const struct a733_media_clock_config *b,
 const struct a733_media_clock_ops *c,void *d,struct a733_clock_leases *e,
 struct a733_gpu_resource_manager *f,unsigned g,const struct a733_gpu_resource_lease *h)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0;}
int a733_platform_lock_enter(struct a733_platform_locks *a,enum a733_platform_lock b,uint64_t c)
{(void)a;(void)b;(void)c;return 0;}
void a733_platform_lock_leave(struct a733_platform_locks *a,enum a733_platform_lock b){(void)a;(void)b;}
int a733_platform_lock_retain(struct a733_platform_locks *a,enum a733_platform_lock b){(void)a;(void)b;return 1;}
int aarch64_device_translate(uintptr_t a,int b,struct aarch64_translation *c)
{(void)a;(void)b;(void)c;return 0;}
int a733_media_clock_native_read(void *a,uintptr_t b,uint32_t *c){(void)a;(void)b;(void)c;return 0;}
int a733_media_clock_native_write(void *a,uintptr_t b,uint32_t c){(void)a;(void)b;(void)c;return 0;}
_Noreturn void platform_panic(const char *a,unsigned b){(void)a;camera_check_result=100+b;__asm__ volatile("brk #0");for(;;){}}
#include "../apps/camera/capture.c"
static void check(int yes,unsigned code){if(!yes){camera_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
void camera_check_run(unsigned mode)
{
 app.handles[2]=(TaskHandle_t)1;app.session.deadline_us=100000;
 app.pixels.cpu=(uintptr_t)pixels;app.pixels.bytes=sizeof(pixels);app.stage=3;
 atomic_store(&app.shared.pmic_ready,1);atomic_store(&app.result,1);
#ifdef SUNSTONE_CAMERA_STREAM
 stream_mode=mode;
 if(mode==3)atomic_store(&app.cancel,1);
 for(unsigned i=0;i<COPY_STEPS+2 && atomic_load(&app.result)<3;++i){
  check(!camera_output(0,chunk,1),50);
  (void)camera_capture_step();
 }
 if(mode>=2){
  check(atomic_load(&app.result)==3&&!camera_output(0,chunk,1),51);
  check(copies==(mode==2?2u:0u),52);
 }else{
  check(copies==COPY_STEPS && polled==COPY_STEPS+1 && atomic_load(&app.result)==4,53);
  check(app.retained_status==(mode==1?4u:5u),54);
  check(camera_output(0,chunk,sizeof(chunk))&&chunk[0]==0x41&&chunk[4095]==0x41,55);
  check(camera_capture_step()==-1&&polled==COPY_STEPS+1&&copies==COPY_STEPS,56);
 }
 camera_check_result=1;__asm__ volatile("brk #0");for(;;){}
#endif
 if(mode==4){
  admitted=1;app.session.camera.owner=6;app.camera.boot=app.session.camera;
  struct a733_clock_token token={.owner=6};struct a733_gpu_resource_lease lease={0};
  for(busy_resource=1;busy_resource<=3;++busy_resource)
   check(authorize(&app,&app.media.config,&token,&lease)==0,30+busy_resource);
  busy_resource=0;check(authorize(&app,&app.media.config,&token,&lease)==1,34);
  camera_check_result=1;__asm__ volatile("brk #0");for(;;){}
 }
 if(mode==1)fail_copy=2;
 if(mode==2)atomic_store(&app.cancel,1);
 if(mode==3)ticks=app.session.deadline_us*24;
 check(!camera_output(0,chunk,sizeof(chunk)),10);
 int r=camera_capture_step();
 if(mode>=2){
  check(r==-1&&!copies&&!polled&&atomic_load(&app.result)==3,11);
  check(camera_capture_step()==-1&&!copies&&!polled,12);
 }else{
  check(r==0&&polled==1&&!copies&&atomic_load(&app.result)==1,13);
  check(camera_capture_step()==0&&copies==1&&!camera_output(0,chunk,sizeof(chunk)),14);
  r=camera_capture_step();
  if(mode==1){
   check(r==-1&&copies==2&&atomic_load(&app.result)==3,15);
   check(!camera_output(A733_CAMERA_NATIVE_PLANE_BYTES,chunk,sizeof(chunk))&&camera_capture_step()==-1&&copies==2,16);
  }else{
   check(r==0&&copies==2&&!camera_output(0,chunk,sizeof(chunk)),17);
   check(camera_capture_step()==1&&atomic_load(&app.result)==2,18);
   check(camera_output(0,chunk,sizeof(chunk))&&chunk[0]==0x41&&chunk[4095]==0x41,19);
   check(camera_output(A733_CAMERA_NATIVE_PLANE_BYTES,chunk,sizeof(chunk))&&chunk[0]==0x42,20);
   check(!camera_output(CAMERA_FRAME_BYTES-1,chunk,2)&&!camera_output(0,chunk,4097),21);
   atomic_store(&app.cancel,1);check(!camera_output(0,chunk,1)&&camera_capture_step()==-1,22);
  }
 }
 camera_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
