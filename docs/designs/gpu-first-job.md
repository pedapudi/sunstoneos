# The first GPU job on the A733

Status: **implemented, host-checked and frozen for port 3-1; not yet run
on a board.** The firmware lifecycle passed on port 3-1
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

## What SunstoneOS changes

**The KICK.** The board showed that the open firmware executes a command
without writing its return word when no caller waits on it (the health
check, board-session-20261025-18). The client ring waited for the KICK's
return word, which upstream never requests. It now settles the KICK
(`pvr_ccb_settle`): accepted once the kernel CCB's read offset has passed
it, or with a return word of 1, single and paired. The job's completion
stays its timeline word. The cleanup keeps its return-word wait, because
upstream waits on it.

**The MMU cache.** The workload VM's tables are written and cleaned before
the firmware starts, and nothing changes them while the firmware runs, so
the GPU's MMU caches are cold when the job runs. No `MMUCACHE` command is
needed for this job; one is needed when a running firmware sees a mapping
change.

**The output and the system-level cache.** The output page is mapped with
the GPU's system-level-cache bypass, so the GPU's writes reach DRAM, where
the CPU reads them after invalidating its own cache. Upstream offers the
same bypass to buffers the host reads (`DRM_PVR_BO_BYPASS_DEVICE_CACHE`,
`pvr_mmu.c:2602`).

## The image

The first-job variant (`apps/gpu-power-id/fw-start.c` built with
`FWS_STOP` and `FWS_JOB`, the job in `fw-job.inc`; build and check option
`--fw-job`; bundle row 27) is the firmware-stop image with the job between
the health check and the stop:

1. **Before any power change**, in the image's own memory: the six firmware
   objects (compute context, register state, client CCB control and
   1,024-byte ring, VM record, timeline) in the six Normal-NC pages after
   the firmware's shared resources, mapped for the firmware at
   `0xc0800000 + n·0x10000`; the context programs and the job's programs,
   control stream, input, output and border table in 88 write-back pages,
   each mapped once in the workload VM; the VM's tables in write-back image
   RAM. Every page's translation is checked first. The job's GPU addresses
   lie in the PDS, USC and general heaps that system init names.
2. **After the health check**: the workload pages are cleaned and the tables
   published; the client CCB control is formatted; the job publishes its
   output guards, checks the timeline is 0, writes the CDM command and the
   UPDATE of the timeline to 1, and kicks.
3. **The wait** ends at 200 ms. The image checks the 32 words and both guards
   itself when the timeline reaches 1, and lets the job read them only when
   they are correct. A timeline that stays 0, or a wrong word, ends the job
   without failing its transport: a failed transport would fail the shared
   kernel CCB, and with it the stop.
4. **Cleanup**: `CLEANUP` (106) of the context, with its return word.
5. **The stop and OFF** of the firmware-stop image, attempted whatever the
   job's result. After a failed job the firmware may refuse or not finish
   the power requests with the context live, and then the GPU stays on.

Firmware events during the job: a debug dump, statistics or a core clock
notice (upstream types 106–108) are counted; any other event, such as a
context-reset notice, fails the control, and then the stop cannot run.

The job runs on the single-core profile that the A733 user manual
documents (MC1). The image does not read the core-count registers
(`0xf308`, `0xf300`) that the service's `pvr_compute_mode` admission reads;
no board has read them yet.

## The board step

Step (d) runs in the lifetime of set `board-bundles-20261025-25`, after its
step (c) passed (bundle 26), with no power removal: set
`board-bundles-20261025-27` holds bundle 27 alone (it replaces set 26, whose admission
said that no firmware had run in the lifetime), with that lifetime's
earlier runs as its warm-return chain. The job's `seed` is 0x12345678 and
its `factor` 0x9e3779b1.

Predictions:

- the health check as before, then the KICK consumed without a return word
  (`kick_status=0x0`);
- the timeline word from 0 to 1 within milliseconds;
- `output[0]` = (0x12345678 + 0x9e3779b1) XOR 0x9e3779b9 = `0x2e5ca990`,
  `output[1]` = `0xd09b3063`, `output[31]` = `0x4775f521`, every word by the
  formula, both 64-byte guards `0x6d35a7c1`;
- the cleanup's return word 1, `kccb_done=0x3` after the cleanup (health,
  KICK, cleanup), and the stop and OFF as in set 23;
- `COMPLETE GPU first job: 32 words as computed, context cleaned up,
  firmware stopped, OFF and provider parked`.

A wrong output with intact guards would mean the program ran with wrong
inputs or state; an unchanged timeline with the KICK consumed would mean
the firmware could not run the command. In both cases the image still
attempts the stop and the OFF, but the lifetime continues only if the
firmware acknowledges the forced-idle and power-off requests. With the
job's context still live, and possibly still running, the firmware may
not reach FORCED_IDLE or OFF within the stop's 300 ms; the stop then fails
before the power-off acknowledgement, the GPU stays on, and a power removal
follows.

The USC program is first executed on the board. The host checks compare
its bytes with the pinned compiler's output and model the GPU's writes,
but no check executes it.

## Design gate

**Cost.** One job per run; it adds two KCCB commands (the KICK and the
cleanup), 88 page cleans, the table publication and a 200 ms bound to the
firmware-stop image, inside the 1.5 s session with the monitor's 500 ms
margin. The image's log for a complete run in the host check is 10,813 of
16,128 bytes.

**Checks.**

- `checks/pvr-cccb.py`: the ring's reference bytes and the KICK consumed
  without a return word.
- `apps/gpu-power-id/check.py --fw-job`: eight cases on the linked image,
  with a model of the firmware and GPU that runs the client CCB's CDM and
  UPDATE commands. Covered: the complete job, also with a KICK return word
  and with a statistics event; a timeline that never moves; a wrong word;
  a broken guard; a cleanup without a return word; a context-reset event.
  Every case's start is the firmware-start sequence access for access, and
  every later GPU access is an interrupt read or clear, a kick and its
  read-back, or the next stop step.
- The firmware-stop image's loadable bytes are unchanged (`c88ba19c…`).
