# The first GPU job on the A733

Status: qualified on the board (port 3-1) for the fixed 32-lane compute job between the firmware health check and the firmware stop. The job image is `apps/gpu-power-id/fw-start.c` built with `FWS_STOP` and `FWS_JOB`.

The job is the smallest one with a verifiable output that SunstoneOS can produce. It follows the upstream Linux driver of the open firmware (`imagination/` sources at Orange Pi `linux-orangepi` revision `2ac08e8c`) and the vendor DDK where the platform is concerned. The firmware binary is never disassembled. The firmware lifecycle that precedes it is in the [firmware stop design](gpu-firmware-stop.md).

## The job

The [fixed numerical compute job](../gpu-compute-job.md) runs one workgroup of 32 lanes with a Mesa-compiled USC program that reads `seed` and `factor` and writes

```text
output[i] = (seed + (i + 1) * factor) XOR (0x9e3779b9 + (i << 16)),  i = 0..31
```

into 128 bytes between two 64-byte guards. Its producer, context builder, context programs, workload VM and client ring are checked against source bytes and host models. A fill or copy through the transfer queue would need a transfer context and programs that SunstoneOS does not have.

## What upstream does for one compute job

1. **Memory and translations.** The programs, control stream, input and output are mapped in a GPU virtual address space whose page tables the firmware context names. After changing mappings the host sends `ROGUE_FWIF_KCCB_CMD_MMUCACHE` (102) and waits for its return slot (`pvr_mmu.c:140–185`).
2. **Context.** A firmware common context for the compute data master and its client CCB (CCCB) live in firmware memory.
3. **Submission.** The host writes a CDM command (205) and a fence update (`ROGUE_FWIF_CCB_CMD_TYPE_UPDATE`, 213, `pvr_rogue_fwif.h:1532`) into the CCCB, then sends `ROGUE_FWIF_KCCB_CMD_KICK` (101) through the kernel CCB without a return slot (`pvr_cccb_send_kccb_kick`, `pvr_cccb.c:225–238`).
4. **Completion.** The firmware runs the command, writes the fence value to the timeline word and raises its interrupt. The host checks the queue's fences (`pvr_device.c:193–209`) and processes the firmware CCB for firmware-initiated requests (`pvr_device.c:265–279`). Normal completion is the timeline word, not a firmware CCB message.
5. **Destruction.** `ROGUE_FWIF_KCCB_CMD_CLEANUP` (106) with a return slot; `CLEANUP_BUSY` (bit 1) means retry (`pvr_fw.c:1127–1158`).

## What the image does

**KICK.** The open firmware executes a command without writing its return word when no caller waits on it. The client ring settles the KICK (`pvr_ccb_settle`) when the kernel CCB's read offset has passed it, or with a return word of 1, single and paired. The job's completion is its timeline word. The cleanup keeps its return-word wait, as upstream does.

**MMU cache.** The image sends `MMUCACHE` (102) before the first KICK, as upstream (`pvr_job.c:806`, `pvr_mmu.c:124–158`) and the vendor DDK (`RGXPreKickCacheCommand`, `rgxfwutils.c:4065`) do. The flags are those a new page catalogue requests (PT, PD, PC, TLB and the interrupt, `0x400001f`, `pvr_mmu.c:71–75`), with the MMU sync object at `0xc0109000` and update value 0. The image waits, as for the health check, until the firmware's executed count has passed it.

**Access bits.** Upstream sets neither the read-only nor the cache-coherent bit on any job mapping (`pvr_mmu.c:2601–2603`). The image maps every page without restriction. Only the output page uses the GPU's system-level-cache bypass, so the GPU's writes reach DRAM, where the CPU reads them after invalidating its own cache; upstream offers the same bypass for buffers the host reads (`DRM_PVR_BO_BYPASS_DEVICE_CACHE`, `pvr_mmu.c:2602`).

**PDS pointer constants.** Each 64-bit pointer constant of the job's PDS data is stored low word first (`doutw_pair` in `pvr_compute_job.c`, in the single-dispatch, grid and multi-dispatch forms; the tile pair keeps the first index in the low half). A DOUTW writes data word 0 to the even register and the program reads the low half from the even register, so a high-word-first constant produces a wrong address. Mesa's own store and load shaders read the low half from register 0 (`usclib/common.cl:181`, `pvr_iface.h:117–122`). The context store and load programs in `pvr_compute_programs.c` follow the same order. The byte references (`checks/pvr-compute-job.c`, `pvr-compute-package.c`, `pvr-compute-programs.c`) pass each of Mesa's values with its halves exchanged, and `check.py --fw-job` checks the constants in the linked image.

## The image sequence

1. **Before any power change**, in the image's memory: the six firmware objects (compute context, register state, client CCB control and 1,024-byte ring, VM record, timeline) in six Normal-NC pages after the firmware's shared resources, mapped for the firmware at `0xc0800000 + n·0x10000`. The context programs and the job's programs, control stream, input, output and border table sit in 88 write-back pages, each mapped once in the workload VM, with the VM's tables in write-back image RAM. Every page's translation is checked first. The job's GPU addresses lie in the PDS, USC and general heaps that system init names.
2. **After the health check**: the workload pages are cleaned and the tables published; `MMUCACHE` is sent and consumed; the client CCB control is formatted; the job publishes its output guards, checks that the timeline is 0, writes the CDM command and the UPDATE of the timeline to 1, and kicks.
3. **The wait** ends at 150 ms, leaving 50 ms for the cleanup. The image checks the 32 words and both guards itself when the timeline reaches 1. A timeline that stays 0, or a wrong word, ends the job without failing its transport, because a failed transport would fail the shared kernel CCB and with it the stop.
4. **Cleanup**: `CLEANUP` (106) of the context, with its return word, once the firmware has finished with the context (job complete, timeline reached, or context reset).
5. **The stop and OFF** of the firmware-stop image, whatever the job's result. When the control failed, or the firmware acknowledges neither power request, the stop runs without the acknowledgements, as upstream's hard reset does ([GPU stop](../gpu-stop.md)): the register sequence, and when every idle poll matches, upstream's final soft reset and the OFF. Only a GPU that never reports idle stays powered.

The image records every firmware event of types 101–108 and none fails the control, as upstream processes the firmware CCB without failing on a type it does not handle (`pvr_ccb.c:139–160`). A context-reset notice (105) ends the job as failed and its words are printed. A restart request (109) or a firmware page fault (112) fails the kernel CCB, and the stop then runs without the acknowledgements. Log lines report the control error, the events, a context reset's words, the client CCB read offset, the KICK return word, the MMU command status and the first recovery record.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Job output on port 3-1 | 32 words as computed (`output[0]` `0x2e5ca990`, `output[31]` `0x4775f521`), guards `0x6d35a7c1`, timeline 0 to 1, context cleaned up, firmware stopped, GPU off | [gpu-first-job.jsonl](../evidence/gpu-first-job.jsonl) `gpu-first-job-set29` |
| Job image entry to return | 62.42 ms, of which firmware start 0.67 ms, health check 1.92 ms, stop 22.99 ms | same record |
| Host checks | 13 job cases, 10 stop cases, 122 retained stop faults | `gpu-first-job-host-checks` |

The attempt with swapped PDS constants (`gpu-first-job-set27`) records the failure mechanism.

## Limitations

- The KICK-to-timeline time of the job is not logged and is bounded only by the 25.75 ms of the image that is not start, health check or stop.
- The image does not read the core-count registers (`0xf308`, `0xf300`) that the service's `pvr_compute_mode` admission reads; it runs on the single-core profile (MC1) the A733 user manual documents.
- The meaning of the MMU-status field in a fault record's decode (catalogue base 1) is open.
- The job session ([job session](gpu-job-session.md)) is the next step: larger Mesa-compiled dispatches, the clock and DVFS state, the CORE6 power-on time as a startup cost (7 to 11 ms) and the license rule for the firmware binary.
