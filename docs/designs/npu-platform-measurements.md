# Measurements: NPU cold boot, recovery and power management

These measurements compare this branch with its base revision,
the commit where it leaves the integration branch, `6cd12f0`, against the budgets in
[the design](npu-platform.md). The branch implements the parts of the design
whose code lies in the NPU startup, platform and PLL controllers, the
convolution application's clock adapter and its session admission. The
remaining parts change interfaces or checks that other components own; the
last section lists them. All runs used the host only; no command contacted
USB devices or the board.

## Conditions

- **Firmware.** Both revisions were configured with the project's standard
  cross-build options plus `-DA733_NPU_SUPERVISED=ON` and the selected
  convolution fixtures, and built with `nice -n 10` and six jobs. The images
  compared are the multicore supervised convolution image, the single-core
  convolution image and the combined accelerator image. The accelerator image
  was configured from `apps/accelerators` with the complete-graph fixtures,
  `A733_NPU_INTERRUPT`, `A733_NPU_LARGE_GRAPH`, `A733_NPU_NN_POOL_GRAPH` and
  `A733_NPU_RUNTIME_INPUT`.
- **Register-model counts.** Two harnesses count callbacks in the existing
  register models: [checks/npu-platform-budget.c](../../checks/npu-platform-budget.c)
  for local startup and local stop, and
  [checks/npu-platform-pll-budget.c](../../checks/npu-platform-pll-budget.c)
  for PLL preparation. One poll stands for one bootstrap tick, as in the
  native bootstrap loop. The same harness sources were compiled against both
  revisions with `clang-21 -fsanitize=address,undefined`.
- **Two-core startup counts.** The startup fixture `checks/npu-startup.c` and
  the PLL adoption fixture `checks/npu-clock-handoff.c` run on two QEMU
  Cortex-A55 cores with real FreeRTOS tasks and mutexes. Both revisions print
  the number of calls and FreeRTOS ticks from the first acquisition step to
  READY; the base revision received the same counting lines in a copy outside
  the repository. Each fixture ran three times per revision; the table gives the range.
  The base revision's counting lines match those the branch adds to the two
  fixtures, and change no assertion or startup call.
- **Sizes.** Code sizes are the summed sizes of function symbols in each image
  and the `.text` size of each controller's object file in the multicore
  image, both read with `llvm-nm` and `llvm-size`. Source sizes are line
  counts of each file. Barriers are `dsb` and `dmb` instructions in those
  object files, counted with `llvm-objdump`. Image data and BSS sizes are
  read with `llvm-size`. Both revisions contain the same integrated work, so
  the image differences come from the branch's NPU changes alone.
- **Counted items** come from the code: session-page hashes, ownership checks
  and bytes hashed before activation.

## Results

| Metric | Design target | Base revision | Branch | Within target |
| --- | --- | --- | --- | --- |
| Session-page hashes at admission | 1 (copy) | 2: live page at EL3, copy at seal or EL1 | 1: the EL3 copy | Yes, for the convolution, single-core, runtime and finite images; the accelerator image has its own admission |
| Local startup polls, acquisition to READY | Combined with PLL: ≤ 3 | 17 | 3 | Local startup alone, yes; combined, no |
| Local startup reads and writes | Combined: ≤ 60 reads, ≤ 12 writes | 51 and 12 | 51 and 12 | Local startup alone, yes; combined, no |
| Local startup clock samples | Combined: ≤ 16 plus one per LOCK sample | 189 | 12 | Local startup alone, yes |
| Local startup mutex acquisitions | Combined: ≤ 3 | 15 | 2 | Local startup alone, yes |
| PLL polls, enabled retuning | Combined: ≤ 3 | 13 | 6 | No |
| PLL reads and writes, enabled retuning | Combined: ≤ 60 reads | 44 and 7 | 43 and 7 | No |
| PLL clock samples | Combined: ≤ 16 plus one per LOCK sample | 152 | 19, with 3 LOCK samples | PLL alone, yes |
| PLL mutex acquisitions | Combined: ≤ 3 | 12 | 5 | No |
| Local gate (local stop) | 1 call, ≤ 12 reads, 5 writes, ≤ 6 clock samples, 1 lock | 6 polls, 102 reads, 5 writes, 238 clock samples, 6 locks | 1 call, 10 reads, 5 writes, 4 clock samples, 1 lock | Yes |
| Ownership checks per register access | 0; one per startup call | Startup: owner-task check on every read, write and clock sample. Adapter: EL read, health query, scheduler query and task compare on every read, write and clock sample | 0 in both; checked at each startup step, acquisition and mutex entry and release | Yes |
| Startup calls in the two-core fixture | ≤ 3 | 28, over 30 ticks | 9 or 10, over 13 or 14 ticks | No |
| PLL and startup calls with PLL adoption | ≤ 3 startup calls | 13 PLL and 28 startup | 6 PLL and 11 startup | No |
| Bootstrap ticks to READY with PLL adoption | ≤ 4 without PENDING | 41 | 19 | No |
| Bytes hashed before activation, accelerator image | 0 | 28 calls × 1,881,936 bytes, 52.7 MB | 11 calls × 1,881,936 bytes, 20.7 MB | No |
| Startup PMIC traffic | None | 12 register reads, 6 bus-state reads, 5,160 µs wait | Unchanged | No |
| Source lines, code and header | Startup ≤ 260 + 60; platform ≤ 90 + 30; PLL ≤ 60 + 25; adapter deleted | Startup 436 + 131; platform 225 + 96; PLL 204 + 109; adapter 130 + 19 | Startup 445 + 131; platform 223 + 96; PLL 204 + 109; adapter 128 + 19 | No; startup 9 lines above the base, the others at or below it |
| Function code bytes | No budget | Multicore 190,004; single-core 180,648; accelerators 343,244 | 185,508; 176,124; 338,824 | Smaller in each image, by 4,496, 4,524 and 4,420 bytes |
| Controller object `.text`, multicore image | No budget | Startup 10,152; platform 7,260; PLL 5,436; adapter 3,208; admission 992 | 9,948; 5,280; 3,884; 2,524; 916 | Each smaller |
| Barriers in the controller objects, multicore image | No startup budget; per-job `DSB SY` has its own row in the design | Admission 0; startup, platform, PLL and adapter 0 | 0 in each | Same as base |
| Data and BSS | No budget | Multicore 2,700,721; single-core 2,426,144; accelerators 8,571,760; admission object 706 | Multicore and accelerators unchanged; single-core 2,426,112; admission object 674 | Yes: the 32-byte retained digest is gone, and nothing grew |

The local startup and PLL rows count each controller alone. The design's
startup targets count both together, because it replaces the two controllers
and the adapter's parent check with one sequencer and one register pass of 29
or 30 reads. That sequencer is not part of this branch, so the combined counts
are 9 polls, 94 reads, 31 clock samples and 7 mutex acquisitions.

In the two-core fixtures the 5,160-microsecond DCDC2 settle accounts for about
six startup calls. The design removes that wait with the power authority's
domain row, which has not landed.

## Results outside the design's targets

The design's budget table states, beside each of these rows, the measured
result and the change the target still needs:

- **Startup calls, PLL polls, reads, mutex acquisitions and bootstrap ticks
  remain above the combined targets.** Each improved on the base revision.
  The targets need the single sequencer described in the last section.
- **Startup PMIC traffic is unchanged**, and the design places its removal
  with the power authority.
- **Bytes hashed before activation** follow the startup call count; the hash
  belongs to the session runtime.
- **Source lines stay above the targets.** Platform, PLL and adapter are at or
  below the base revision. Startup is 9 lines above it: 5 lines of the loop
  that runs acquisition steps until one waits, which cut two-core startup
  calls from 28 to 9 or 10, and 4 comment lines that state where the ownership
  check now runs.

Object code and barriers are no worse than the base revision. The
acquisition loop has no iteration counter, because the startup stage only
advances; the compiler therefore does not unroll it, and the startup object
is 204 bytes smaller than the base revision. EL3 admission issues no barrier
of its own: the single-core seal's `DSB SY`, or the multicore boot's `DSB SY`
before its next register access, completes the copy before EL1 enables
caching.

## Changes

- **Local startup.** The poll that completes acquisition continues into one
  locked pass: entry checks, the twelve-step open table with readback, and the
  first domain status read. The settle then needs one poll without the mutex
  and one final locked check.
- **Local stop.** One call and one locked pass check the state startup left,
  then gate the module and bus and assert the three resets from known values,
  reading back each write. It no longer rereads the parent clocks.
- **PLL.** Acquisition stays a poll of its own, because the provider's
  admission must be observable before any PLL access. The baseline checks and
  all six programming steps then run in one locked pass, and the settle check
  and output gate in another. LOCK observations keep their 3-microsecond
  spacing and their limit of 100.
- **Deadline checks.** Both controllers check the deadline at poll entry,
  after releasing the mutex, and inside each locked pass before the first
  write, after each LOCK observation and before release. Individual register
  accesses take no clock sample.
- **Startup binding.** One call runs acquisition steps until one waits. A
  further step runs only after the previous one advanced the acquisition
  stage, so at most seven steps run. The register and clock callbacks drop their per-access owner check;
  register access still needs the mutex, which `enter` takes after its owner
  check.
- **Clock adapter.** Ownership is checked in acquisition, mutex entry and mutex
  release only.
- **Admission.** The EL3 contract check copies the session page into private
  memory, hashes the copy once and validates it. The single-core seal and the
  multicore EL1 copy step check the copy's identity against the contract
  without hashing.

## Checks

All of these passed on the branch:

- `checks/npu-platform.c` in its three modes, `checks/npu-stop.c`,
  `checks/a733-npu-platform-clock-reference.c`, `checks/npu-pll.c`,
  `checks/a733-npu-pll-clock-reference.c` and both budget harnesses with
  `budget`, all with address and undefined-behavior sanitizers.
- The two-core startup fixture in all five modes, the PLL adoption fixture in
  all four modes, and the convolution fixture in its success, mutation, close,
  close-fault and job-command modes.
- `checks/npu-application-clock-reference.py` (six reference cases),
  `checks/npu-stop-native.py` (ten adapter cases),
  `checks/npu-single-core.py --retention-only` (three retained-session cases)
  and `checks/accelerator-providers.py` on the branch images.

These fail identically on both revisions, at the same statement:

- `checks/npu-supervised.py` stops at its first PMIC poll on an unmodeled read
  of `0x0701000c`. Its admission cases, which run earlier, pass on the branch:
  an invalid page is refused at EL3, the valid contract is admitted and the
  EL1 copy step succeeds.
- `checks/npu-single-core.py` without `--retention-only` stops at its USB poll
  after passing its four construction cases.
- The convolution fixture's startup-guard mode stops at its first startup
  poll.
- The native PMIC consumer composition `checks/a733-pmic-consumers.c` stops at
  its first startup poll.

## Not implemented, with the owner each part needs

- **One sequencer and one register pass.** Replacing the platform and PLL
  stage machines and deleting the clock adapter changes interfaces that
  `drivers/npu/npu_convolution.c`, `drivers/npu/npu_session.c`, the CMake
  fragments under `cmake/` and the checks `checks/npu-pll.c`,
  `checks/npu-stop.c`, `checks/npu-convolution.c`,
  `checks/a733-pmic-consumers.c` and the two clock-reference checks use.
- **Removing the PMIC path and domain-ON adoption.** These need the power
  authority's domain row and supply read. Removing adoption also changes the
  supervised live-DRAM profile that `checks/npu-supervised.py` covers.
- **Reduced session record and handoff flags.** These change
  `apps/accelerators/prepare.py`, `apps/accelerators/admission.c`, the runtime
  images and the session checks.
- **Client waits.** A client that holds a WAIT after OPEN and SUBMIT, and
  blocks on a bootstrap notification, changes the operation sequence that
  `checks/npu-finite-client.py` and `checks/npu-interrupt-client.py` model.
- **Service, worker and memory work.** Idle gating, the job, bootstrap and
  cancel deadlines, the 10 ms fallback, REAP folding, removal of the output copy
  and the per-job digest, maintenance and mapping changes belong to
  `npu_service.c`, `npu_session.c`, `npu_convolution.c` and
  `a733_npu_memory.c`.

Physical behavior is unqualified. The board evidence listed in the design
would confirm it.
