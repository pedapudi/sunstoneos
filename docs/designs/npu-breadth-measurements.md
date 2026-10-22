# NPU runtime measurements

These measurements check the NPU runtime, which loads and switches
catalogue models in fixed slots, against the budgets in
[npu-breadth.md](npu-breadth.md). They compare the integration base (commit
`c45a76e`) with the branch (commit `aeb9cd8`). All values come from host
builds, QEMU runs with a register model in place of the NPU, and the host
bound check; no board or NPU was used.

## Method

- **Sources.** Each commit was exported with `git archive` into its own
  directory under `~/.cache/sunstoneos/build/npu-breadth-measure/`. The
  session code `drivers/npu/npu_session.c` is identical at both commits.
- **Code and data size.** Both commits were configured with the shared
  configuration script and built with `-j 6`. The NPU board applications
  `apps/accelerators` (ShuffleNetV2 graph) and
  `apps/npu-convolution-single-core` (the small convolution fixture) were
  configured as separate projects at both commits. `llvm-size-21 -A` gave
  section sizes, `llvm-objcopy-21 -O binary` gave loadable images, and
  `llvm-nm-21 -S` gave per-function sizes.
- **Per-inference counts.** `checks/npu-session.py` ran three modes. The
  `reserved` mode runs the session code with five catalogue models; the
  branch's version of `checks/npu-session.c` prints its counters, so it
  measures the base behavior. The base's own check passed in the `session`
  and `reserved` modes but prints no counters. The `runtime` mode runs the
  same five models in four runtime slots. Figures are per ShuffleNetV2
  inference, from the COUNT lines of three jobs. The `service-shared` mode
  passed at both commits.
- **Time bounds.** `checks/npu-runtime-bounds.py` computed G, P, H, W and the
  start, reply and load bounds from the counted bytes at the design's
  estimated worst-case rates at 494 MHz, and matched its byte counts against
  the runtime COUNT line.
- **Call overhead.** `llvm-objdump-21` disassembled the service worker in the
  accelerators image at both commits.

## Per-inference results for ShuffleNetV2

| Metric | Base (session code) | Branch (runtime) | Budget (interim) | Within budget |
| --- | ---: | ---: | --- | --- |
| Requests and replies | 3 and 3 | 1 and 1 | 1 and 1 | Yes |
| Client wake-ups | 3 | 1 | 1 | Yes |
| Address translations of buffers | 3,216 (all five models) | 0 | 0 | Yes |
| Register reads and writes per job | Not counted | 12 and 2 | At most 16 | Yes |
| SHA-256 bytes | 920,616 | 0 | 0 | Yes |
| Integrity-check bytes read | 541,656 | 1,509,856 | About 1,510,000 | Yes |
| CPU bytes zeroed or copied | 1,244,162 | 1,093,632 zeroed, 0 copied | 1,093,632 zeroed | Yes |
| Cache lines maintained | 36,672 | 19,442 | 19,442 | Yes |
| Worker steps: preparation, pool, post-job check | None | 11, 67 and 31 | Not set | — |

The runtime reads more integrity-check bytes than the session code: two NPU
table scans of 20,480 bytes, 27,112 address-bearing bytes, an END tail of
521,168 bytes and 920,616 read-only bytes. The design sets this interim
figure in its section on interim integrity checks, which replace per-job
SHA-256 until fixed mappings make the slot memory read-only; with those
mappings the budget is 0.

In the case whose interrupts arrive late, four jobs overlapped 489 worker
steps and 11 ticks and read 509 registers in that time. The budget allows 16
accesses per job plus one read per overlapping step and four per tick, 597.

## Time bounds

| Term | Budget | Emulator check image (four slots) | First consumer (one slot, USB0 on core 0) | Within budget |
| --- | --- | ---: | ---: | --- |
| G, pre-trigger block | 1.15 ms | 1.15 ms | 1.44 ms | Yes |
| P, job and pool preparation | 4.33 ms | 4.33 ms | 5.41 ms | Yes |
| H, post-job check | 1.86 ms | 2.01 ms | 2.52 ms | No, reason stated |
| W = P + H + L | 6.29 ms; 7.86 ms with USB0 | 6.58 ms | 8.22 ms | No, reason stated |
| Longest non-yielding step other than G | C = 0.1 ms | L = 0.235 ms | 0.29 ms | No, reason stated |
| Start wait / held reply | Below the collect timeout | 3.01 s / 4.02 s, timeout 20 s | 7.15 ms / 1.02 s, timeout 5 s | Yes |
| One ShuffleNetV2 LOAD on an idle worker, portable SHA-256 | About 58 ms | 65.2 ms (1,204 steps bounded, 1,135 counted) | Same | No, reason stated |

The design states each excess:

- H is bounded by the model file size, 964,224 bytes, because any catalogue
  model can be loaded into a slot. The 1.86 ms budget is the counted
  920,616 read-only bytes.
- L replaces C because the LOAD step that decodes the staged copy runs about
  58,000 instructions, recorded under "One LOAD step exceeds C".
- A LOAD reads the model through a staging copy and checksums that copy
  again, recorded under "LOAD reads the model through a staging copy". One
  ShuffleNetV2 LOAD hashed 964,224 bytes, checksummed the staging area
  (964,224 bytes), copied 1,856,832 bytes and read 920,616 bytes for the
  check references. The three other slots completed 1,191 jobs while it ran.

## Code and data size

The 13 default targets have byte-identical loadable images at both commits.
No board image links the runtime, but every image that links the NPU
service grows:

| Image | `.text` base → branch | `.bss` base → branch |
| --- | --- | --- |
| `sunstone-a733-accelerators` | 348,652 → 350,700 | 1,379,536 → 1,379,600 |
| `a733-npu-convolution-single-core` (ShuffleNetV2) | 194,360 → 196,408 | 1,262,368 → 1,262,432 |
| `a733-npu-convolution-single-core` (small fixture) | 193,324 → 195,372 | 222,880 → 222,944 |
| `a733-npu-convolution-supervised` | 208,740 → 210,788 | 519,168 → 519,232 |
| `npu-service-model` (host check) | 56,576 → 58,624 | 1,769,200 → 1,769,264 |

Function sizes in the accelerators image account for 2,008 of the 2,048
`.text` bytes. The service worker calls the runtime worker loop whenever
`runtime_mode` is set, so the linker keeps that loop in every image. The
loop and its helpers add 2,600 bytes: `execute_shared` (956), `irq_progress`
(652), `placed` (388), `runtime_settle` (344), `view` (116) and
`irq_mask` (+144). Splitting the assembler and inlining changes remove a net 592
bytes from `nbg_assemble_layout`, `nbg_assembly_layout` and `worker`. The 64 `.bss` bytes are the
runtime fields added to `struct npu_service`.

The runtime itself adds 13,968 bytes of code and 796 bytes of constants in
`a733_npu_runtime.c` and 1,472 bytes in `a733_npu_runtime_interim.c`. Once
the session code is deleted, its 9,516 bytes of code and 716 bytes of
constants leave the images that use the runtime.

## Instructions in images that do not use the runtime

At the base commit the worker inlines `irq_progress` and `execute_shared`,
each of which it calls once. On the branch the runtime worker also calls
them, so the compiler emits both out of line. In the exclusive and shared
service modes, each worker pass that runs interrupt progress now executes a
call, a return and seven register save and restore instructions; each
shared-mode request adds a call, a return and nine save and restore
instructions. Request validation adds one comparison for the LOAD operation.
These are about 10 extra instructions per interrupt progress pass and per
shared request. They were not measured in an instruction count.

## Conclusion

Every per-inference budget of the interim runtime is met, and the time-bound
excesses (H, W, the longest step and the LOAD time) have reasons stated in
the design. The design gives no reason for the cost to images that link the
NPU service but not the runtime: 2,008 bytes of code, 64 bytes of `.bss` and
about 10 instructions per interrupt progress pass and per shared request. One
remedy is to call the runtime worker loop through a pointer that
`npu_service_runtime` installs, so `--gc-sections` drops the loop where no
image calls that function. The runtime fields would then move behind that
pointer, and the existing worker would keep its inlined helpers.

## Reproduce

With the session artifacts map described in
[npu-session.md](../npu-session.md#emulator-checks):

```sh
checks/npu-session.py BUILD OUTPUT_RESERVED --mode reserved --artifacts ARTIFACTS.json
checks/npu-session.py BUILD OUTPUT_RUNTIME --mode runtime --artifacts ARTIFACTS.json
checks/npu-runtime-bounds.py --output OUTPUT_BOUNDS --evidence OUTPUT_RUNTIME/result.json
```
