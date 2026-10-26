# Performance-mode measurements

The current integration is based on the thermal-protected OS at `6d77a16d`.
Evidence is in `~/.cache/sunstoneos/evidence/resume-perf-modes-1025/`.
The owner takes 30 modeled ticks for its boot switch, as before. A thousand
idle ticks add no register reads, PMIC snapshots or device requests. Cleanup
contention retries the same snapshot within its original deadline; it adds
no snapshot submission. CPU and combined NPU profiles link natively.

The owner checks include thermal interruption before and during a switch,
NORMAL returning before an active change finishes, snapshot interruption,
transient cleanup contention and retained timeout failures. The combined
native fixture exercises the actual 1176-byte session admission and both USB
diagnostics: thermal operation 7 and performance operation 8. These are
software results; physical mode changes and recovery remain unqualified.
The source-backed engine retains three CPU requests and two PMIC selector
writes per full switch. The current shared PLL lock helper takes 233 modeled
microseconds and 43 lock samples per stopped-NPU retune, with five writes
and 19 or 20 register reads.

Current linked sizes, in bytes:

| Profile | `.text` | `.rodata` | `.bss` |
| --- | ---: | ---: | ---: |
| CPU performance, high boot | 903,584 | 43,689 | 1,567,264 |
| CPU and NPU performance, low boot | 983,840 | 47,801 | 1,707,808 |

These totals include the current thermal evidence window. The older sizes
below use an earlier base and cannot isolate the current service's cost.

## Earlier implementation measurements

The figures below describe the inherited implementation at `17b63bd2`.
They are retained evidence, not a size or byte-identity claim for the current
integration. Current source uses the shared PLL lock helper; the earlier
forced-inline comparison does not describe that helper.

These figures come from host models and builds; no board has run the
service. The models give each CPU domain change a fixed modeled cost, so
their wall times show the service's own overhead and ordering, and the PMIC
and PLL latencies of a board add to them. Evidence is in `~/.cache/sunstoneos/evidence/perf-modes-20261025/`.

## Switch costs

`checks/perf-mode.py` runs the engine model (`checks/perf-mode-model.c`),
stepped once per 1 ms owner tick with a modeled 1 ms voltage settle and a
0.1 ms relock per CPU change.

| Metric | Value | Source |
| --- | --- | --- |
| CPU driver requests per full switch | 3 (CPU-L, CPU-B, DSU) | Engine model |
| PMIC selector writes per full switch | 2 (DCDC5, DCDC3) | Engine model |
| Boot switch from boot0's state | 5.0 ms modeled | Engine model |
| High to low, low to high | 5.4 ms modeled each | Engine model |
| NPU held after its session stops | 0.4 ms, the modeled retune | Engine model |
| Refusal of a switch whose NPU session stays open | 1.5 s, the hold bound | Engine model |
| PLL_NPU writes per retune of a stopped NPU | 5, each read back | `checks/npu-startup-host.c --retune` |
| Register reads and lock samples per retune | 19 or 20 reads, 70 lock samples, 2 register-lock acquisitions | Same |
| Modeled retune time | 228 µs | Same |
| Boot switch in the OS owner model, with the readback | 30 owner ticks | `checks/perf-os-model.c` |
| PMIC snapshots per readback | 4 (DCDC5, DCDC3, DCDC4, DCDC2) | Same |

## Image size

`llvm-size-21 -A` of the linked OS images:

| Build | `.text` | `.rodata` | `.bss` |
| --- | --- | --- | --- |
| CPU clock build (`A733_A76_ADMITTED`, `SUNSTONE_A733_OS_CPU_CLOCK`) | 890,400 | 43,129 | 1,532,576 |
| The same with `SUNSTONE_A733_OS_PERF` | 899,808 (+9,408) | 43,321 (+192) | 1,533,408 (+832) |
| NPU profile | 952,352 | 45,721 | 1,672,352 |
| NPU profile with the CPU clock owner and the service, low boot mode | 979,936 (+27,584) | 47,513 (+1,792) | 1,674,016 (+1,664) |

The second NPU row's growth includes the CPU clock driver and its owner,
which the NPU profile did not link before.

## Default images

The ten default OS variants were built from clean archives at one path, at
the branch base (`fc889bba`, the CPU top-point merge) and at `17b63bd2`, with
the same mimus checkout (`9c57286b`), and their loadable segments compared
(`images/comparison.json` in the evidence directory). The OS build identity
hashes every `apps/os` header and the changed board sources, so the 32-byte
`a733_smp_build_id` differs in every image.

| Variants | Result |
| --- | --- |
| `sup`, `res`, `resaud`, `resradio`, `resradioaud`, `headset`, `resheadset`, `radio-headset`, `resradio-headset` | Loadable bytes equal apart from the build identity |
| `npu` | Equal segment layout and symbol sizes; besides the build identity, 165 bytes differ, all inside `a733_npu_startup_step` |

The NPU difference is code generation, not behaviour: `lock_wait` now has a
second caller, the stopped-NPU retune, which the linker then discards from
this image. Forced inline at both callers, the step keeps its 5,596 bytes,
with a few loads and adds placed differently (for example 287 `ldr` instead
of 282 plus 5 `ldur`). Without the forced inline, the first revision
(`ec0fe8da`) called `lock_wait` as a 316-byte function. The startup host
model (`checks/npu-startup-host.c`, every mode) passes on the source.

Gate `perf-modes-20261025-2` built all twelve OS variants, the ten defaults
and both performance-mode variants, at `17b63bd2` and verified the check
index.
