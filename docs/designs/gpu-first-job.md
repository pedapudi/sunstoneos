# The first GPU job on the A733

Status: **failed on port 3-1 in set 27; the post-mortem capture found the
cause, a swapped pointer constant (see "Set 27 post-mortem"); fixed and
host-checked.** The firmware lifecycle passed on port 3-1
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

**The MMU cache.** Set 27 skipped `MMUCACHE`, reasoning that the tables
were written before the firmware started. Neither reference ever skips it:
upstream flushes the requested MMU caches before the first submission
(`pvr_job.c:806`, `pvr_mmu.c:124–158`), and the vendor DDK invalidates
before every kick (`RGXPreKickCacheCommand`, `rgxfwutils.c:4065`). The image
now sends `MMUCACHE` (102) with the flags a new page catalogue requests (PT,
PD, PC, TLB and the interrupt, `0x400001f`, `pvr_mmu.c:71–75`), the MMU sync
object at `0xc0109000` and update value 0, and before the KICK waits, as for
the health check, until the firmware's executed count has passed it.

**Access bits.** Set 27 mapped the programs, control stream, input and
border table GPU read-only. Upstream sets neither the read-only nor the
cache-coherent bit on any job mapping (`pvr_mmu.c:2601–2603`); the image now
maps every page without restriction, and only the output keeps its
system-level-cache bypass.

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
   published; `MMUCACHE` is sent and consumed; the client CCB control is
   formatted; the job publishes its
   output guards, checks the timeline is 0, writes the CDM command and the
   UPDATE of the timeline to 1, and kicks.
3. **The wait** ends at 150 ms, leaving 50 ms for the cleanup. The image
   checks the 32 words and both guards itself when the timeline reaches 1,
   and lets the job read them only when they are correct. A timeline that stays 0, or a wrong word, ends the job
   without failing its transport: a failed transport would fail the shared
   kernel CCB, and with it the stop.
4. **Cleanup**: `CLEANUP` (106) of the context, with its return word, once
   the firmware has finished with the context: the job complete, its
   timeline reached, or the context reset.
5. **The stop and OFF** of the firmware-stop image, whatever the job's
   result. When the control has failed, or the firmware acknowledges
   neither power request, the stop runs without the acknowledgements: the
   register sequence, and when every idle poll matches, upstream's final
   soft reset and the OFF. Only a GPU that never reports idle stays
   powered.

Firmware events during the job: every event of types 101–108 is recorded
and none fails the control, as upstream processes the firmware CCB without
failing on a type it does not handle (`pvr_ccb.c:139–160`). A context-reset
notice (105) ends the job as failed and its words are printed. A restart
request (109) or a firmware page fault (112) fails the kernel CCB; the stop
then runs without the firmware's acknowledgements (below).

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

## Set 27 on port 3-1

Set `board-bundles-20261025-27` ran in the lifetime of set 25
(`board-session-20261025-22/port-3-1/s22-gfj-run`). The start and the health
check passed as before. Then:

```text
GPU FWJ job complete=0 outcome=4 stage=4 phase=4 error=0 client=7 client_error=0 kick_status=0x0 timeline=0x1
GPU FWJ output w0=0xd1a3566f w1=0x2f64cf9c w31=0xb88a0ade guard=0x6d35a7c1 expected0=0x2e5ca990 expected31=0x4775f521
GPU FWJ cleanup status=0xffffffff events=0 kccb_done=0x2 pow_state=0x3
FAIL GPU firmware stop refused, GPU left running
```

- Stage 4 means the job had started; phase 4 is `PVR_JOB_KICK`, and client
  7 is `PVR_CCCB_WAIT`: the KICK had been sent and was waiting to be
  settled. Outcome 4 is the transport: the loop ended because the control
  failed, not on the timeline or the output.
- The firmware consumed the KICK: the executed count went from 1 to 2. It
  set the timeline to 1, the value of the job's UPDATE command, and raised
  its interrupt; the image read the interrupt and cleared it (accesses 104
  and 105).
- The output is the image's own fill: before the KICK it writes the
  complement of each expected word, and w0, w1 and w31 are each the exact
  complement (`0xd1a3566f XOR 0x2e5ca990 = 0xffffffff`, likewise for w1 and
  w31); the guards are intact. No GPU store reached those words.
- The progress record ends at access 105, the interrupt clear. The next
  access of the control's poll, the second interrupt read, never came, so
  the control failed between them: on a firmware event it refused, on a
  restart request, or on a KICK return word with bits beyond 0–2. The host
  model reproduces every logged line with each of these
  (`check.py --fw-job --board-log`); the log cannot tell them apart. The
  firmware CCB in the image's Normal-NC pages, still in DRAM while the
  board waits in FEL, can.
- The cleanup was not sent (0xffffffff is the image's initial value): the
  image sent it only after a complete job. With the control failed, the stop
  refused to start, and the GPU stayed powered with the firmware running.

The completion check did not miss the timeline: the job never reached its
wait, and had it, the output check would have found the fill.

**Likely cause.** A job that runs normally, or never launches its USC
program, raises no firmware event. If the event was a context reset (105),
the GPU faulted in the job within about a millisecond, too soon for the
lockup detector (the context's limit is 30,000 ms): an MMU page fault or a
USC error. The firmware reset the context, skipped the CDM command and
still ran the UPDATE after it. No host source documents that last step; it
fits the timeline of 1 and the untouched output. A step-by-step comparison
of the kick path with upstream, the DDK and Mesa found the context, client
CCB commands, KICK, page-table entry formats, CDM stream, cleanup and CPU
cache maintenance matching, and these differences, most likely first:

1. The USC program has never executed on hardware.
2. No `MMUCACHE` before the first KICK; both references always send it.
3. Read-only bits on the program, stream, input and border pages, which
   upstream never sets.
4. The CDM command's flags `0x24` (prevent overlap, single core), which
   Mesa sets for barriers or atomics.

The pointer constants of the job's PDS program were stored high word
first, as Mesa's save-restore generator stores them, and the comparison
judged that order correct. The post-mortem capture showed it was the cause
(below).

**Changes.** The job image records events 101–108 without failing the
control; it settles a consumed KICK after the job, so its pending slot cannot
fail the kernel CCB; it sends the cleanup whenever the firmware has finished
with the context. When the control has failed or the firmware acknowledges
neither power request, the stop runs without the acknowledgements, as
upstream's hard reset does ([GPU stop](../gpu-stop.md)): the register
sequence and, when every idle poll matches, the OFF. Only a GPU that never
reports idle now stays powered. New lines report the control error, the
events, a context reset's words, the client CCB's read offset and the
KICK's return word, the MMU command's status and the firmware's first
recovery record. Differences 2 and 3 are removed (see "What SunstoneOS
changes"); 1 and 4 stay, because the program and the command bytes are
compared with Mesa's output.

## Set 27 post-mortem

After set 27 returned to FEL, with the GPU still powered and nothing run in
between, the coordinator read three regions with `sunxi-fel read`
(`board-session-20261025-22/port-3-1/s22-gfj-postmortem`): the firmware's
shared pages and the job's six firmware objects (`0x41c00000`, 51 pages),
the job payload (`0x41090000`, 88 pages) and the GPU page tables
(`0x410e8000`, 32 pages). The structures are decoded from the upstream
interface headers (`pvr_rogue_fwif.h`, `pvr_rogue_fwif_check.h`,
`pvr_rogue_cr_defs.h`) and the DDK's `rgxmhdefs_km.h`.

**The event that ended the control.** The firmware CCB holds one command
(write and read offset 1), `0x2abc0069`: 105, the context reset
notification. Its data (`struct rogue_fwif_fwccb_cmd_context_reset_data`)
is context 1, reason 1 (`GUILTY_LOCKUP`), data master 4 (CDM), job
reference 1, flags `0x1` (page fault), page catalogue `0x410e8000` (the
job's) and fault address `0x80`. This confirms the first of the three
causes: the image refused the context reset event, and the control failed.

**The recovery record** (`0x8000`, `struct rogue_fwif_hwrinfobuf`): count 1;
record 0 is type 3 (`BIF0FAULT`), data master 4, recovery flags `0x20`;
the CDM locked-up and recovered counts are each 1. Its BIF data:

- request status `0x0010100000000080`. The core has the XE memory hierarchy
  (36.0.104.183 feature word `0x000002181d51bf35`, bit 41), so the XE layout
  applies: bit 52 set, a read; tag ID 16, `MCU_USCA`, a USC data access;
  tag sideband 0; address bits 4–39, `0x80`;
- MMU status `0x1041`: fault, data type 2, 4 KB pages, catalogue base 1;
- page catalogue `0x410e8000`.

System data shows the power state IDLE (3) and the HWR state flag 1. The
kernel CCB's write and read offsets are 2 (the health check and the KICK
consumed, no return words). The job's client CCB control (`0x2f000`) shows
write, read and dependency offsets all `0xb8` and wrap mask `0x3ff`: the
firmware took both the CDM and the UPDATE command. The timeline is 1.

**The page tables are right; the address is not.** GPU address `0x80` falls
in catalogue entry 0, which is invalid, so the walk faults there. The job's
input `0x8001140000` walks catalogue entry 512, directory entry 8 and table
entry 320 to `0x410a6000`, the input page, and its output `0x8001190000`
walks to `0x410a7000` with the cache-bypass bit, as built.

**The address comes from the PDS constants.** The job's PDS data at
`0x410a3000` stores each pointer high word first: `0x80`, `0x01140000` for
the input and `0x80`, `0x01190040` for the output. A DOUTW writes data word
0 to the even register, and the program reads the low half from the even
register. The input pointer therefore became `0x0114000000000080`, whose
40-bit bus address is `0x80`, the fault address; the output pointer would
have given `0x4000000080`, so the fault was the program's first load, of
`seed` and `factor`. Mesa's clear-colour and end-of-tile DOUTWs put word 0
in the destination register (`pvr_pds.c:570–573`, `3238–3247`); only its
save-restore generator, whose layout the job's PDS reference followed,
stores the high word first (`pvr_pds.c:3664–3667`).

**The payload is unchanged.** The payload and the page tables are byte for
byte what the set-27 image composed, replayed in the host model with no GPU
write: the output page holds the image's fill, and no other byte of the 88
pages changed. In the shared pages only firmware-owned words differ: the
queue offsets, the recovery record, system and OS data counters, the client
CCB's offsets, one word of the context state and the VM record's
catalogue-register field (`0xffffffff` to 0). The trace buffers are
unchanged.

**Fix.** The job's PDS constants are stored low word first
(`pvr_compute_job.c`, `doutw_pair`), in the single-dispatch, grid and
multi-dispatch forms, and the tile pair keeps the first index in the low
half. The byte references (`checks/pvr-compute-job.c`,
`checks/pvr-compute-package.c`) pass Mesa's generator each value with its
halves exchanged, and `check.py --fw-job` checks the constants in the
linked image; the set-27 image fails that check. The context store and
resume programs still follow Mesa's save-restore order; the firmware runs
them only when it stores or resumes a compute context, which one job on a
new context does not do, so they are left unchanged and recorded as open.

## Design gate

**Cost.** One job per run; it adds three KCCB commands (`MMUCACHE`, the
KICK and the cleanup), 88 page cleans, the table publication and a 200 ms
bound to the firmware-stop image, inside the 1.5 s session with the
monitor's 500 ms margin; the stop keeps its 300 ms, of which the stop
without acknowledgements takes the last 100 ms. The image's log in the host
check is at most 11,311 of 16,128 bytes.

**Checks.**

- `checks/pvr-cccb.py`: the ring's reference bytes and the KICK consumed
  without a return word.
- `apps/gpu-power-id/check.py --fw-job`: thirteen cases on the linked image,
  with a model of the firmware and GPU that runs the client CCB's CDM and
  UPDATE commands. Covered: the complete job, also with a KICK return word
  and with a statistics event; a timeline that never moves; a wrong word;
  a broken guard; a cleanup without a return word; a context-reset event
  with and without the output; set 27's signature (`fj-board-1025`); a
  restart request; a bad KICK return word; no acknowledgement of the power
  requests, with and without an idle GPU. With `--board-log`, the
  set-27 image reproduces every logged line of set 27 under three of
  these models.
  Every case's start is the firmware-start sequence access for access, and
  every later GPU access is an interrupt read or clear, a kick and its
  read-back, or the next stop step.
- The firmware-stop image changes with the stop without acknowledgements;
  `check.py --fw-stop` passes ten cases, three of them new or changed.
- `checks/pvr-gpu-stop.c` (122 retained faults) and
  `checks/pvr-gpu-stop-reference.py` pass; the second compares the
  unacknowledged plan with upstream's `pvr_fw_stop`, final reset included.
