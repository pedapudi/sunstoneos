/* The device-facing callees that apps/os/gpu-timing.c wraps (submission, command ring, cache
 * maintenance) and the backend register reads, modeled at their boundary for
 * checks/gpu-job-timing.c. They are defined here so that the link wraps apply to
 * the fixture's calls. */
#include <assert.h>
#include <string.h>
#include "gpu_queue_timing.h"
#include "pvr_compute_queue.h"
#include "pvr_gpu_service.h"
#include "aarch64_device_memory.h"
extern struct pvr_compute_queue timing_queue;
unsigned cache_calls, expanded_count, cr_samples, state_samples, complete_during_sample;
struct pvr_cccb_command expanded[4];
#ifdef SUNSTONE_GPU_NSI_PMU
unsigned nsi_calls[3];
void a733_gpu_backend_nsi_sample(unsigned position,struct a733_gpu_nsi_record *r)
{ assert(position<3);++nsi_calls[position];r->samples|=1u<<position; }
#endif
void a733_gpu_backend_timing_sample(unsigned p) { ++cr_samples;gpu_queue_timing_cr(p,100+p,10+p,20+p); }
int a733_gpu_backend_timing_state(struct gpu_queue_timing_state *s)
{ ++state_samples;s->valid=127;s->pll_raw=0xe8105300;s->clock_raw=0x80000000;s->core_status_raw=8;
  s->fw_power_raw=1;s->fw_core_hz=1008000000;s->pmic_selector_raw=0x2e;s->pmic_programmed_uv=960000;
  if(complete_during_sample)((uint64_t *)timing_queue.firmware[5].storage.cpu)[2]=123;
  s->pmic_retained=1;s->pmic_rail_id=3;s->pmic_request_id=4;return 1; }
int gpu_queue_submit(struct gpu_queue_context c,const struct gpu_queue_job *j)
{ (void)c;(void)j;return GPU_QUEUE_OK; }
int gpu_queue_wait(struct gpu_queue_context c,struct gpu_queue_record *r,uint32_t wait)
{ (void)c;(void)wait;*r=(struct gpu_queue_record){.sequence=7,.result=GPU_QUEUE_OK,.kick_to_done_us=123};return GPU_QUEUE_OK; }
int pvr_cccb_begin(struct pvr_cccb *s,const struct pvr_cccb_command *c,unsigned n,const uint32_t *clean,unsigned nc,uint64_t end)
{ (void)s;(void)clean;(void)nc;(void)end;expanded_count=n;memcpy(expanded,c,n*sizeof(*c));return 1; }
enum pvr_cccb_result pvr_cccb_poll(struct pvr_cccb *s)
{ s->phase=PVR_CCCB_IDLE;return PVR_CCCB_OK; }
enum pvr_ccb_result pvr_ccb_submit(struct pvr_ccb *qq,const void *c,uint64_t t,uint64_t d,struct pvr_ccb_token *token)
{ (void)qq;(void)c;(void)t;(void)d;(void)token;
  gpu_queue_timing_stamp(GPU_TIMING_KICK_BEGIN);gpu_queue_timing_stamp(GPU_TIMING_KICK_END);return PVR_CCB_OK; }
int aarch64_device_cache(enum aarch64_cache_op op,uintptr_t addr,size_t bytes)
{ (void)op;(void)addr;(void)bytes;++cache_calls;return 1; }

