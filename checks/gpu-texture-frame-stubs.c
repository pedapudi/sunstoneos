/* Boundary stubs of checks/gpu-texture-frame.c: the GPU controller, firmware and
 * compute modules that drivers/gpu/pvr_gpu_service.c calls and that a ready session
 * running a render never reaches, and the RTOS calls the worker makes at the kernel
 * boundary. Reaching an unreachable stub fails the fixture with code 40. */
#include <stddef.h>
#include <stdint.h>
extern void frame_stub_reached(void);
int a733_gpu_graphics_memory_abort(void) { frame_stub_reached(); return 0; }
int a733_gpu_graphics_memory_begin(void) { frame_stub_reached(); return 0; }
int a733_gpu_graphics_memory_cleanup_records(void) { frame_stub_reached(); return 0; }
int a733_gpu_graphics_memory_poll(void) { frame_stub_reached(); return 0; }
int a733_gpu_graphics_memory_validate(void) { frame_stub_reached(); return 0; }
int pvr_ccb_abort(void) { frame_stub_reached(); return 0; }
int pvr_ccb_collect(void) { frame_stub_reached(); return 0; }
int pvr_ccb_init(void) { frame_stub_reached(); return 0; }
int pvr_ccb_submit(void) { frame_stub_reached(); return 0; }
int pvr_compute_job_result_words(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_abort(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_begin(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_cleanup_begin(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_cleanup_poll(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_event(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_poll(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_prepare(void) { frame_stub_reached(); return 0; }
int pvr_compute_service_replace(void) { frame_stub_reached(); return 0; }
int pvr_free_lists_event(void) { frame_stub_reached(); return 0; }
int pvr_free_lists_init(void) { frame_stub_reached(); return 0; }
int pvr_free_lists_poll(void) { frame_stub_reached(); return 0; }
int pvr_fw_requirements(void) { frame_stub_reached(); return 0; }
int pvr_fw_state_layout(void) { frame_stub_reached(); return 0; }
int pvr_fw_state_prepare(void) { frame_stub_reached(); return 0; }
int pvr_gpu_control_abort(void) { frame_stub_reached(); return 0; }
int pvr_gpu_control_count_executed(void) { frame_stub_reached(); return 0; }
int pvr_gpu_control_init(void) { frame_stub_reached(); return 0; }
int pvr_gpu_control_steps(void) { frame_stub_reached(); return 0; }
int pvr_gpu_events_dispatch(void) { frame_stub_reached(); return 0; }
int pvr_gpu_events_init(void) { frame_stub_reached(); return 0; }
int pvr_gpu_flush_init(void) { frame_stub_reached(); return 0; }
int pvr_gpu_flush_poll(void) { frame_stub_reached(); return 0; }
int pvr_gpu_start_begin(void) { frame_stub_reached(); return 0; }
int pvr_gpu_start_cancel(void) { frame_stub_reached(); return 0; }
int pvr_gpu_start_init(void) { frame_stub_reached(); return 0; }
int pvr_gpu_start_poll(void) { frame_stub_reached(); return 0; }
int pvr_gpu_stop_init(void) { frame_stub_reached(); return 0; }
int pvr_gpu_stop_init_unacknowledged(void) { frame_stub_reached(); return 0; }
int pvr_gpu_stop_poll(void) { frame_stub_reached(); return 0; }
int pvr_mips_prepare(void) { frame_stub_reached(); return 0; }
/* A ready session's controller health check reports OK. */
int pvr_gpu_control_poll(void) { return 0; }
/* The worker runs alone on one modeled core: critical sections and masks are no-ops. */
uint64_t uxPortSetInterruptMaskFromISR(void) { return 0; }
void vPortClearInterruptMaskFromISR(uint64_t mask) { (void)mask; }
void vPortEnterCritical(void) {}
void vPortExitCritical(void) {}
int xTaskGetSchedulerState(void) { return 2; }
void *xTaskGetCurrentTaskHandle(void) { return (void *)1; }
void platform_panic(const char *file, unsigned line) { (void)file; (void)line; frame_stub_reached(); for (;;) {} }
/* Reached only by service initialization and replies, which the fixture bypasses. */
void *xQueueGenericCreateStatic(void) { frame_stub_reached(); return NULL; }
int xQueueGenericSend(void) { frame_stub_reached(); return 0; }
int xQueueGenericSendFromISR(void) { frame_stub_reached(); return 0; }
void *xTaskCreateStatic(void) { frame_stub_reached(); return NULL; }
