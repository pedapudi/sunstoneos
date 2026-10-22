# NPU runtime measurements

These measurements check the NPU runtime, which loads and switches
catalogue models in fixed slots, against the budgets in
[npu-breadth.md](npu-breadth.md). They compare the integration base (commit
`6cd12f0`) with the branch (commit `230acd1`). All values come from host
builds, QEMU runs with a register model in place of the NPU, and the host
bound check; no board or NPU was used.

## Method

- **Sources.** Each commit was exported with `git archive` into its own
  directory under `~/.cache/sunstoneos/build/npu-breadth-measure/v3/`. The
  session code `drivers/npu/npu_session.c` is identical at both commits.
- **Code and data size.** Both commits were configured with the shared
  configuration script and built with `-j 6`, including the
  `npu-service-model` and `npu-service-irq-model` host checks. The NPU board
  applications `apps/accelerators` (ShuffleNetV2 graph) and
  `apps/npu-convolution-single-core` (the small convolution fixture) were
  configured as separate projects at both commits. `llvm-size-21 -A` gave
  section sizes, `llvm-objcopy-21 -O binary` gave the loadable images that
  were hashed, and `llvm-nm-21 -S` gave per-function sizes.
- **Per-inference counts.** `checks/npu-session.py` ran four modes. The
  `reserved` mode runs the session code with five catalogue models and prints
  its counters at both commits, so it gives the base behavior. The `runtime`
  mode, present only on the branch, runs the same five models in four
  runtime slots. Figures are per ShuffleNetV2 inference, from the COUNT lines
  of three jobs. The `session` and `service-shared` modes passed at both
  commits.
- **Time bounds.** `checks/npu-runtime-bounds.py` computed G, P, H, W and the
  start, reply and load bounds from the counted bytes at the design's
  estimated worst-case rates at 494 MHz, and matched its byte counts against
  the runtime COUNT line.
- **Worker instructions.** `llvm-objdump-21` disassembled the service worker
  at both commits. QEMU ran `npu-service-model` three times per commit with
  `-d exec,in_asm,nochain` restricted to the worker's address range; each
  logged execution of a translation block was weighted by the block's
  instruction count.

## Per-inference results for ShuffleNetV2

| Metric | Base (session code) | Branch (runtime) | Budget (interim) | Within budget |
| --- | ---: | ---: | --- | --- |
| Requests and replies | 3 and 3 | 1 and 1 | 1 and 1 | Yes |
| Client wake-ups | 3 | 1 | 1 | Yes |
| Address translations of buffers | 3,216 | 0 | 0 | Yes |
| Register reads and writes per job | Not counted | 12 and 2 | At most 16 | Yes |
| SHA-256 bytes | 920,616 | 0 | 0 | Yes |
| Integrity-check bytes read | 541,656 | 1,509,856 | About 1,510,000 | Yes |
| CPU bytes zeroed or copied | 1,244,162 | 1,093,632 zeroed, 0 copied | 1,093,632 zeroed | Yes |
| Cache lines maintained | 36,672 | 19,442 | 19,442 | Yes |
| Worker steps: preparation, pool, post-job check | None | 11, 67 and 31 | Not set | — |

The base translation counter covers the buffers of all five loaded models,
so it exceeds the design's single-model baseline of 2,126. The runtime reads more
integrity-check bytes than the session code: two NPU table scans of 20,480
bytes, 27,112 address-bearing bytes, an END tail of 521,168 bytes and
920,616 read-only bytes. The design sets this interim figure in its section
on interim integrity checks, which replace per-job SHA-256 until fixed
mappings make the slot memory read-only; with those mappings the budget is 0.

In the case whose interrupts arrive late, four jobs overlapped 508 worker
steps and 11 ticks and read 528 registers in that time. The budget allows 16
accesses per job plus one read per overlapping step and four per tick, 616.

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
  check references. The three other slots completed 1,172 jobs while it ran.

## Code and data size

The runtime state sits in a static object outside `struct npu_service`, and
the worker reaches the runtime loop only through a function pointer that
`npu_service_runtime` installs, so `--gc-sections` drops the runtime from
images that never call that function. `irq_progress`, `execute_shared` and
the assembler's `placed` are forced inline.

Thirteen of the 15 default targets have byte-identical loadable images at
both commits; the other two are the NPU service host checks. The images that
link the NPU service keep their section sizes:

| Image | `.text` base and branch | `.rodata` base and branch | `.bss` base → branch |
| --- | ---: | ---: | --- |
| `sunstone-a733-accelerators` | 350,280 | 1,269,800 | 1,361,168, unchanged |
| `a733-npu-convolution-single-core` (ShuffleNetV2) | 194,360 | 1,134,832 | 1,243,936, unchanged |
| `a733-npu-convolution-single-core` (small fixture) | 193,324 | 22,568 | 204,448, unchanged |
| `npu-service-model` (host check) | 56,576 | 2,008 | 1,769,200, unchanged |
| `npu-service-irq-model` (host check) | 55,144 | 2,088 | 1,768,080 → 1,768,096 |

Function sizes grow by 20 bytes in each board image, which section
alignment absorbs: 12 in the worker for the runtime check at task entry and
4 each in `nbg_assemble_layout` and `nbg_assembly_layout`, which share their
checks with the stepwise assembler that the runtime uses. `struct
npu_service` grows from 36,912 to 36,920 bytes for the runtime pointer. In
`npu-service-irq-model` that growth crosses an alignment boundary and adds
16 bytes of `.bss`. The design's budget allows the runtime-mode check and
its pointer.

The runtime adds 13,984 bytes of code and 796 bytes of constants in
`a733_npu_runtime.c`, and 1,472 bytes of code in
`a733_npu_runtime_interim.c`, to an image that links it. No board image
links it yet. Once the session code is deleted, its 9,516 bytes of code and
716 bytes of constants leave the images that use the runtime.

## Worker instructions

The worker in the board images has 1,059 instructions on the branch against
1,056 at the base, and calls neither inlined helper. Five of its
instructions are the runtime check at task entry, which runs once per worker
start rather than once per pass; register allocation and block order account
for the remaining difference.

Across three QEMU runs of `npu-service-model`, the worker executed 6,306,
6,386 and 6,383 instructions at the base and 6,216, 6,218 and 6,186 on the
branch, in 1,937 to 1,978 block executions. The branch executes about 2.6%
fewer worker instructions in the same check, which meets the budget of no
growth beyond the runtime-mode check.

## Conclusion

Every per-inference budget of the interim runtime is met, and the time-bound
excesses (H, W, the longest step and the LOAD time) have reasons stated in
the design. Images that link the NPU service but not the runtime keep the
section sizes of the integration base, except 16 bytes of `.bss` alignment in
one host check, and their worker executes no more instructions than at the
base. The runtime, session, service-shared, reserved, service and interrupt
checks pass on the branch.

## Reproduce

With the session artifacts map described in
[npu-session.md](../npu-session.md#emulator-checks):

```sh
checks/npu-session.py BUILD OUTPUT_RESERVED --mode reserved --artifacts ARTIFACTS.json
checks/npu-session.py BUILD OUTPUT_RUNTIME --mode runtime --artifacts ARTIFACTS.json
checks/npu-runtime-bounds.py --output OUTPUT_BOUNDS --evidence OUTPUT_RUNTIME/result.json
```
