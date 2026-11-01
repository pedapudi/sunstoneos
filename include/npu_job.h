#ifndef SUNSTONE_NPU_JOB_H
#define SUNSTONE_NPU_JOB_H
#include "npu_runtime.h"

/* One NPU job from an application task, on the image's NPU runtime. The
 * calling task must be the client task of one runtime slot
 * (npu_runtime_bind). On its first call the helper sends OPEN for that slot;
 * it sends LOAD whenever the slot does not hold the requested model; it then
 * copies the input, sends SUBMIT and copies the output. Each runtime request
 * and its reply are bounded by the wait given to npu_job_attach.
 *
 * model is an index into the models given to npu_runtime_prepare, which an
 * image takes from its applications' model lists. in holds every input tensor
 * of the model, in model order and without padding; in_bytes must equal the
 * sum of their logical sizes. out and out_bytes hold the output tensors in the
 * same way. deadline_us is the job deadline in microseconds, the SUBMIT
 * timeout; the first call also gives it to OPEN as the bootstrap deadline.
 * The helper copies each tensor once in each direction; an application that
 * fills or reads the tensor windows in place uses npu_runtime_tensor itself.
 *
 * Returns 1 after a completed job whose output was copied. Returns 0 when the
 * call was refused before any job was queued: the task owns no slot, a size
 * or index is wrong, or the runtime refused the LOAD (the slot is then
 * empty). Returns -1 when a request failed, faulted or did not reply within
 * the wait; the slot and its outstanding request stay retained and every
 * later call for the slot returns -1. Progress points go to sunstone_crumb
 * with the NPU_JOB_CRUMB_* stages of npu_crumb.h. */
int npu_job_attach(struct npu_runtime *, TickType_t wait);
int npu_job_run(unsigned model, const void *in, size_t in_bytes, void *out, size_t out_bytes,
                uint64_t deadline_us);
/* CLOSE for the calling task's slot, after its last job. Returns 1 once the
 * slot is closed, 0 if the task never opened a slot, and -1 as npu_job_run. */
int npu_job_close(void);
#endif
