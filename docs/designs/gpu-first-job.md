# The first GPU job on the A733

Status: **planned; not built.** The firmware lifecycle passed on port 3-1
([firmware stop design](gpu-firmware-stop.md)): start, one health check,
forced idle, power off, idle checks, OFF. This note plans the first job that
makes the GPU compute something the host can check, from the upstream Linux
driver of the open firmware (`imagination/` sources at orangepi
`linux-orangepi` `2ac08e8c`) and the vendor DDK where the platform is
concerned. The firmware binary is never disassembled.

## The job: the fixed numerical compute workload

The smallest job with a verifiable output that SunstoneOS can already
produce is the [fixed numerical compute job](../gpu-compute-job.md): one
workgroup of 32 lanes runs a Mesa-compiled USC program that reads `seed`
and `factor` and writes

```text
output[i] = (seed + (i + 1) * factor) XOR (0x9e3779b9 + (i << 16)),  i = 0..31
```

into 128 bytes between two 64-byte guards. Its producer, context builder,
context programs, workload VM and client ring are implemented and checked
against source bytes and host models. A fill or copy through the transfer
queue would need a transfer context and transfer programs that SunstoneOS
does not have, so it is not smaller.

## What upstream does for one compute job

1. **Memory and translations.** The job's programs, control stream, input
   and output are mapped in a GPU virtual address space whose page tables
   the firmware context names. After changing mappings the host sends
   `ROGUE_FWIF_KCCB_CMD_MMUCACHE` (102) and waits for its return slot
   (`pvr_mmu.c:140–185`).
2. **Context.** A firmware common context for the compute data master and
   its client CCB (CCCB) live in firmware memory.
3. **Submission.** The host writes a CDM command (205) and a fence update
   (`ROGUE_FWIF_CCB_CMD_TYPE_UPDATE`, 213, `pvr_rogue_fwif.h:1532`) into the
   CCCB, then sends `ROGUE_FWIF_KCCB_CMD_KICK` (101) through the kernel CCB
   **without a return slot** (`pvr_cccb_send_kccb_kick`,
   `pvr_cccb.c:225–238`).
4. **Completion.** The firmware runs the command, writes the fence value to
   the timeline word, and raises its interrupt; the host checks the queue's
   fences (`pvr_device.c:193–209`) and processes the firmware CCB for
   firmware-initiated requests (`pvr_device.c:265–279`). Normal completion
   is the timeline word, not a firmware CCB message.
5. **Destruction.** `ROGUE_FWIF_KCCB_CMD_CLEANUP` (106) with a return slot;
   `CLEANUP_BUSY` (bit 1) means retry (`pvr_fw.c:1127–1158`).

## What must change before the board step

The board showed that the open firmware executes a command without writing
its return word when no caller waits on it (the health check,
board-session-20261025-18). SunstoneOS's client ring waits for the KICK's
return word (`pvr_cccb.c`, `pvr_ccb_collect` after the kick), which
upstream never requests. The kick must be judged as the health check now
is: the executed count advanced and the kernel CCB read offset past the
command (`pvr_ccb_settle`), with the job's completion taken from its
timeline word. The MMU cache and cleanup commands keep their return-word
waits, because upstream waits on them.

## The image and the board step

The job joins the firmware-stop image: after the health check, it prepares
the workload VM, context and job in the image's own memory (the job needs
267,136 bytes of private allocations and the context and VM records, which
fit the image's write-back RAM and Normal-NC block), maps them for the
firmware, flushes the GPU MMU cache, kicks the job, waits for its timeline,
checks the 32 words and both guards, cleans the context up, and then stops
the firmware and powers the GPU off as now. The GPU's compute cores must be
on: the job runs on the single-core profile that `pvr_compute_mode` admits
from the post-power observation.

Step (d), in a fresh port 3-1 lifetime after the provider-readiness
checkpoint: start, health check, one compute job with `seed` 0x12345678 and
`factor` 0x9e3779b1, cleanup, stop, OFF and park.

Predictions:

- the MMU cache command returns with bit 0 set;
- the KICK is consumed (executed count +1, read offset past it) without a
  return word;
- the timeline word goes from 0 to 1 within milliseconds;
- `output[0]` = (0x12345678 + 0x9e3779b1) XOR 0x9e3779b9, and every other
  word follows the formula, with both 64-byte guards intact;
- the cleanup returns with bit 0 set, and the stop and OFF pass as in set 23.

A wrong output with intact guards would mean the program ran with wrong
inputs or state; an unchanged timeline with the KICK consumed would mean
the firmware could not run the command, and its firmware CCB or fault
records then say why.
