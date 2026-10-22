# Shared range, deadline, MMIO, mapping and task-context helpers

Four headers replace helpers that many native adapters and drivers define
privately. Each header holds static inline functions, and each function name
states one semantic rule. Copies whose rules differ map to different functions;
the headers do not merge them.

| Header | Mechanism |
| --- | --- |
| [a733_task_context.h](../include/a733_task_context.h) | Admission of the calling FreeRTOS task: EL, IRQ mask, interrupt and critical nesting, scheduler state, owner task |
| [a733_range.h](../include/a733_range.h) | Address-range validity, disjointness and overlap; monotonic deadline samples; overflow-checked and saturating deadline sums |
| [a733_mmio.h](../include/a733_mmio.h) | 8-, 16-, 32- and 64-bit register accessors with the DSB SY placement in the name |
| [a733_mapping_check.h](../include/a733_mapping_check.h) | Stage-1 translation checks (AT S1E1R/W through a callback) with the attribute and shareability rule as a parameter |

The cache-maintenance operation enum is `enum aarch64_cache_op` in
[aarch64_device_memory.h](../include/aarch64_device_memory.h), which
`aarch64_device_cache()` already takes. The DMA controller enum
`enum a733_dma_cache` and the NPU memory enum `enum a733_npu_cache_op` name the
same three operations for their callback tables. `platform/a733/dma_native.c`
and `platform/a733/npu_native.c` map them onto the AArch64 enum by name.

## Variants and their helpers

The counts below cover `drivers/`, `platform/`, `apps/`, `boot/` and
`runtime/`, and include the callers listed under *Migrated callers*. Range helpers were classified by behavior: each local helper was
compiled on the host and compared with every header predicate over all
combinations of 15 edge values (0 through 4, 0x1000, 0x1001, the middle of the
address space and the top 4097 addresses). A helper that matched one predicate
on every combination is listed under it.

### Task context

Every copy masks IRQs while it reads the port's per-core interrupt and
critical-section nesting counters. The SMP port indexes those counters by the
core number in TPIDR_EL1; the single-core port keeps one pair.

| Variant | Meaning | Helper | Callers |
| --- | --- | --- | --- |
| EL1 and owner task | Refuses unless CurrentEL is EL1 and IRQs were unmasked on entry, before masking. Then requires zero nesting, a running scheduler and the given task as the current task. | `a733_task_context_owner_el1(owner)` | 14 files: the ADC, camera, TWI, radio, PCIe, PMIC, PWM, SPI, MMC, UART and USB2 native adapters, `platform/a733/hdmi_native.c`, `typec_native.c` and `maintenance_watchdog.c` |
| Any task, no EL check | Always masks and restores DAIF, then requires IRQs unmasked on entry, zero nesting, a running scheduler and any current task. CurrentEL is not read. | `a733_task_context_any_no_el_check()` | `platform/a733/shared_locks.c` |

Instance checks (`initialized`, `self == n`, sealed configuration) stay at the
call site, before the helper. The TWI and camera adapters replace the check
with `a733_camera_model_current()` when `SUNSTONE_CAMERA_MODEL` is defined; that
conditional stays at the call site too.

### Single ranges

The *exclusive end* is p + n; the *last byte* is p + n - 1. A range that ends at
the top byte of the address space has a representable last byte but no
representable exclusive end.

| Variant | Meaning | Helper | Copies | Example callers |
| --- | --- | --- | --- | --- |
| Exclusive end, zero allowed | p non-NULL; p + n fits | `a733_span_excl` | 9 | `drivers/net/aic8800_*.c` `span`, `drivers/storage/sunxi_sdio.c` |
| Exclusive end, nonempty | p non-NULL; n nonzero; p + n fits | `a733_span_excl_nonempty` | 25 | `drivers/gpu/pvr_*.c` `range` and `span`, `drivers/npu/nbg_model.c` `cpu_range` |
| Last byte, nonempty | p non-NULL; n nonzero; p + n - 1 fits | `a733_span_incl_nonempty` | 3 | `drivers/bluetooth/bt_acl.c`, `drivers/npu/npu_session.c` |
| Last byte, empty accepted | n zero accepted with any p, including NULL | `a733_span_incl_or_empty` | 2 | `drivers/bluetooth/bt_h4.c`, `drivers/uart/a733_uart.c` |
| Last byte, empty only when absent | n zero accepted only with NULL | `a733_span_incl_or_null_empty` | 1 | `drivers/spi/a733_spi.c` (32-bit length) |
| Containment | [start, start + n) inside [base, base + bytes), no sums formed | `a733_extent_within` | 2 | `drivers/gpu/pvr_compute_programs.c` `contains`, `pvr_compute_job.c` `inside` (both take structures) |

### Pairs of ranges

Pointer forms (`a733_ranges_*`) apply a NULL rule; extent forms
(`a733_extents_*`) take 64-bit integer addresses and treat address zero as an
ordinary address. A *wrapping* range is one whose exclusive end does not fit.

| Variant | Meaning | Helper | Copies | Example callers |
| --- | --- | --- | --- | --- |
| Disjoint, NULL and wrap refused | Both non-NULL, both ends fit, no shared byte; zero length allowed | `a733_ranges_disjoint` | 46 | `separate` in the native adapters, `drivers/usb/a733_typec.c` `disjoint`; exported equivalents `a733_ce_separate()` and `bt_range_disjoint()` |
| Disjoint and nonempty | As above, and both lengths nonzero | `a733_ranges_disjoint_nonempty` | 7 | `drivers/gpu/pvr_cccb.c`, `platform/a733/npu_reserved.c`; exported equivalent `a733_dma_separate()` |
| Empty ranges are disjoint | Zero length is disjoint from everything, even NULL | `a733_ranges_disjoint_or_empty` | 0 local | exported equivalent `bt_range_disjoint_empty()` |
| Optional second range | First range required; NULL second range is absent | `a733_ranges_disjoint_optional_second` | 2 | `drivers/net/aic8800_dispatch.c`, `aic8800_network.c` |
| Fail-closed overlap | True when either end wraps or a byte is shared; no NULL rule | `a733_ranges_overlap_or_wrap` (pointers), `a733_extents_overlap_or_wrap` (integers) | 9 | `overlap` in `drivers/display/*.c`, `drivers/pinctrl/a733_pins.c` and `platform/a733/cpu_coherence.c`; `drivers/iommu/a733_iommu.c` `overlaps` |
| Fail-closed, empty never overlaps | Zero length never overlaps; otherwise fail-closed | `a733_extents_overlap_nonempty_or_wrap` | 1 | `drivers/display/a733_dsi.c` |
| Disjoint, wrap refused, no NULL rule | Both ends fit and no shared address | `a733_extents_disjoint_checked` | 3 | `drivers/display/a733_dp.c`, `a733_display_pll.c`, `drivers/gpu/a733_gpu_platform.c` |
| Intersection by distance | Integer intervals intersect; zero length never does; no wrap concept | `a733_extents_intersect` | 6 | `drivers/npu/a733_npu_memory.c`, `npu_session.c`, `drivers/spi/a733_spi.c`, `drivers/uart/a733_uart.c` |
| Disjoint, unchecked | `a + an <= b \|\| b + bn <= a`, modular | `a733_extents_disjoint_prechecked` | 20 | `drivers/gpu/pvr_*.c` `separate` and `apart`, `boot/a733/stage2/loader.c` |
| Overlap, unchecked | `a < b + bn && b < a + an`, modular | `a733_extents_overlap_prechecked` | 19 | `drivers/net/aic8800_*.c`, `drivers/npu/nbg_*.c`, `runtime/device_memory_pool.c` |

The unchecked forms are correct only when the caller has already established
that both ends fit, usually through a span check on the same ranges. With a
wrapping input they can report overlapping ranges as disjoint.

Thirty-five local helpers combine these predicates with object-specific
ranges, for example `outside()` in the native adapters, which tests a caller
buffer against the adapter instance and its locks. They stay local and call the
shared predicate inside. Four helpers match no header function and stay local:
`drivers/npu/nbg_sh_nt.c` `overlap` and `separate` (unchecked forms that treat
zero length as disjoint), `drivers/net/a733_radio_entropy.c` `contains`, and the
32-bit window check `range` in `platform/a733/cpu_entry_native.c`.

### Deadlines

| Variant | Meaning | Helper | Copies | Example callers |
| --- | --- | --- | --- | --- |
| Admit a sample | Refuse a sample earlier than the last one or at or after the deadline; store it only on admission | `a733_deadline_admit(now, &last, deadline)` | 16 | `timely()` in `drivers/thermal/a733_thermal.c`, `drivers/adc/a733_adc.c`, `drivers/crypto/a733_ce.c` |
| Store, then compare | Refuse only a backward sample, store it, then compare with one or more deadlines | `a733_monotonic_advance(now, &last)` followed by the caller's comparisons | 4 | `drivers/pci/a733_pcie.c`, `drivers/pwm/a733_pwm.c`, `drivers/storage/a733_mmc_platform.c`, `drivers/usb/a733_usb2_platform.c` |
| Plain comparison | `now < deadline` with no stored sample, or with extra state | none | 13 | `drivers/gpu/pvr_compute_service.c`, `apps/rtc-returning/main.c` |
| Overflow-checked sum | `*out = now + delta` unless the sum overflows | `a733_deadline_after(now, delta, &out)` | 2 helpers, about 90 inline guards | `drivers/usb/a733_typec.c` `add`, `drivers/usb/a733_typec_pd.c` `limit` |
| Saturating sum | `now + delta`, or UINT64_MAX when the sum overflows; a saturated bound never expires | `a733_deadline_after_saturating(now, delta)` | 8 | `after` in `drivers/bluetooth/bt_avdtp.c`, `bt_hfp_ag.c`, `bt_rfcomm.c` and `bt_sco.c`; `apps/board/radio.c` `later` |

The caller samples its clock and refuses a failed sample before calling
`a733_deadline_admit()`. Several samplers also record a failure reason or test a
retained fault; those parts stay in the caller.

### MMIO barrier placement

Native adapters order register accesses with DSB SY and a compiler memory
clobber. The accessor name states where the barriers are.

| Variant | Meaning | Helper | Files | Example callers |
| --- | --- | --- | --- | --- |
| Fenced on both sides | DSB SY; access; DSB SY | `a733_mmio_readN_dsb_sy_both`, `a733_mmio_writeN_dsb_sy_both` | 52 | `read32`/`write32` in the native adapters, `apps/*/native.c` register loads |
| Completion after | Access; DSB SY | `a733_mmio_readN_dsb_sy_after`, `a733_mmio_writeN_dsb_sy_after` | 17 | `platform/a733-smp*/launch.c` `rd`/`wr` (AArch32), `platform/a733/dma_native.c` reads |
| Fenced sequence | DSB SY, then several accesses, such as a read-modify-write check | `a733_mmio_dsb_sy()` with the plain accessors | 21 | read-modify-write paths in the native adapters, `apps/rtc/*.c` status reads |
| Plain | One volatile access, no barrier, no memory clobber | `a733_mmio_readN`, `a733_mmio_writeN` | 1 header | `mmio_read`/`mmio_write` in `platform/a733/boot.h` |

A file can contain more than one placement. The barrier forms compile for
AArch64 and AArch32; the plain accessors also compile on the host. Register
allowlists, mapping checks and lock checks stay with each caller.

### Mapping checks

Seventy direct calls of `aarch64_device_translate()` check that addresses
translate with an expected physical address, MAIR attribute value and
shareability (PAR_EL1.SH: 0 non-shareable, 2 outer, 3 inner). A
`struct a733_mapping_rule` states the rule:

- `access`: `A733_MAPPING_READ`, `A733_MAPPING_WRITE` or
  `A733_MAPPING_READ_WRITE`. Read-write translates read first and requires
  the same attribute value from both.
- `accept`: one or two attribute values, each with a mask of accepted SH
  values (`A733_SH_NON`, `A733_SH_OUTER`, `A733_SH_INNER`, `A733_SH_ANY`).
- `same_shareability`: read and write SH must be equal.
- `compare_physical`: the physical address must equal the expected one.

| Rule family | Rule | Example callers |
| --- | --- | --- |
| Device registers, read and write | `READ_WRITE`, `{0x00, ANY}`, identity | `mapped` in `platform/a733/hdmi_native.c`, `drivers/display/a733_display_framebuffer.c`, `apps/displayport/native.c`; `at` in `typec_native.c` |
| Device registers, write, outer shareable | `WRITE`, `{0x00, OUTER}`, identity | `device` in `drivers/video/a733_video_unit.c`, `apps/h264/native.c`, `apps/g2d/native.c`; page lists in `apps/adc/native.c` |
| Device registers, one direction | `READ` or `WRITE`, `{0x00, ANY}`, identity | `platform/a733/maintenance_watchdog.c` `mappings`, `apps/gpu-finite/finite.c` |
| NPU device registers | `{0x00, ANY}` or `{0x04, ANY}`, given physical address | `device_mapping` in `platform/a733/npu_native.c` |
| Write-back pools and images | `WRITE` or `READ`, `{0xff, INNER}`, identity | `memory_reservations.c`, `dma_pool_session.c` `image`, `gpu_reserved.c` `private_image`, profile pages in `apps/board/main.c` |
| Write-back with offset | `WRITE`, `{0xff, INNER}`, physical offset, 64-byte stride | descriptor memory in `apps/crypto/native.c` |
| Normal-NC pool | `READ_WRITE`, `{0x44, OUTER \| INNER}`, same SH, identity | `memory_uncached.c`, NC pages in `device_memory_layout.c` |
| Display mapping | `READ_WRITE`, given attribute with `OUTER \| INNER` unless the attribute is 0x00, same SH, given physical address | `a733_display_native_mapping()` |
| USB2 buffers | `WRITE`, given attribute with `OUTER \| INNER` for 0x44 and `ANY` otherwise, identity | `mappings` in `drivers/usb/a733_usb2_native.c` |
| GPU output spans | `WRITE`, `{0xff, ANY}` or `{0x44, ANY}`, physical address not compared | `output_span` in `platform/a733/gpu_backend.c` |
| HDMI audio buffers | `WRITE`, given attribute with one exact SH value, identity | `apps/hdmi-audio/native.c` |

Ranges use one of three walks:

- One address: `a733_mapping_check()`.
- Every `step` bytes from the start address, below the end:
  `a733_mapping_check_stride()`. With an unaligned start and a 4096-byte step,
  the last partial page is visited only when a step lands in it. The pool
  initializers, the USB2 buffer check and the crypto descriptor check walk this
  way.
- Every 4 KiB page that holds a byte of the range, starting at the start address:
  `a733_mapping_check_pages()`. Walks that translate each page base give the
  same result, because every address of one 4 KiB page translates with the
  same attributes.

The NPU memory manager translates through its own callback,
`a733_npu_memory_ops.translate`, so host checks can model it. Its rule check
needs a translate function with the `a733_translate_fn` signature, or keeps its
own loop.

## Migration rule

A migration replaces a local copy only with the helper whose meaning is the
same for every input. Before replacing a copy, determine:

1. For ranges: inclusive or exclusive end, zero-length handling, NULL handling,
   wrap handling and argument order. Most helpers take the object first and the
   caller's buffer second, but the predicates are symmetric; check the
   argument order only for `a733_ranges_disjoint_optional_second()` and
   `a733_extent_within()`. A helper whose arguments are `uint32_t` lengths or
   addresses matches only for representable values.
2. For deadlines: whether the stored sample advances on an expired sample, and
   which failure handling the caller performs.
3. For MMIO: the exact barrier placement and the presence of the memory
   clobber. A migration keeps the barrier strength; a weaker barrier is a
   separate change that needs board qualification.
4. For mapping checks: which translations are made and in which order, the
   accepted attribute values, the SH rule, whether physical addresses are
   compared, and the walk.
5. For task context: whether the copy checks CurrentEL, whether it compares
   the current task with an owner, and which instance checks precede it.

Then:

- When the semantics match a helper, call it and delete the local copy.
  Keep a one-line local wrapper only where it binds a caller-specific argument,
  such as an owner field.
- When they match no helper, keep the local copy. Add a new helper only when
  at least two copies share the new meaning, give it a name that states the
  difference, and add its cases to [shared-helpers.c](../checks/shared-helpers.c).
- Do not move a caller to a stricter or looser helper as part of a migration.
  For example, replacing an unchecked disjointness test with
  `a733_ranges_disjoint()` adds a NULL and wrap refusal. That change needs its
  own justification and the module's focused checks.
- Rebuild every target that compiles the changed file, and compare the
  disassembly of the changed functions with the previous build. Expected
  differences are register allocation, branch layout and `__LINE__` constants
  in assertions. Barrier instructions and the order of DAIF, TPIDR_EL1 and
  scheduler calls must stay the same. Then run the focused checks that compile
  the file; `python3 checks/check-index.py --changed FILE` lists them.

## Migrated callers

These callers use the headers now:

- `platform/a733/shared_locks.c`: `a733_task_context_any_no_el_check()`.
- `platform/a733/maintenance_watchdog.c`: `a733_task_context_owner_el1()` after
  its NULL-task refusal; `a733_mmio_read32_dsb_sy_both()` and
  `a733_mmio_write32_dsb_sy_both()` for the watchdog registers.
- `platform/a733/typec_native.c`: `a733_task_context_owner_el1()` and
  `a733_ranges_disjoint()`.
- `drivers/usb/a733_typec.c`: `a733_ranges_disjoint()` and
  `a733_deadline_after()`.
- `drivers/usb/a733_typec_pd.c`: `a733_deadline_after()`; the former helper
  returned -1 on overflow, and the call sites now test for zero.
- `drivers/pinctrl/a733_pins.c`: `a733_ranges_overlap_or_wrap()`.
- `drivers/npu/a733_npu_pll.c` and `drivers/clock/a733_media_clock.c`:
  `a733_deadline_admit()`.
- `platform/a733/dma_native.c`: `a733_mmio_read32_dsb_sy_after()`,
  `a733_mmio_write32_dsb_sy_both()` and `a733_mmio_dsb_sy()`.
- `platform/a733/memory_reservations.c` and `platform/a733/memory_uncached.c`:
  `a733_mapping_check_stride()` with the write-back and Normal-NC pool rules.
- `drivers/net/a733_radio_platform_native.c`: `a733_task_context_owner_el1()`
  after its instance check; `a733_mmio_read32_dsb_sy_both()`,
  `a733_mmio_write32_dsb_sy_both()` and `a733_mmio_dsb_sy()` for the SDC1,
  CCU and GIC registers.
- The Wi-Fi stack (`drivers/net/aic8800_*.c`, `a733_radio_platform.c`,
  `a733_radio_session.c`, `a733_radio_entropy.c` and `sunstone_net.c`):
  `a733_span_excl()`, `a733_extents_overlap_prechecked()`,
  `a733_ranges_disjoint()`, `a733_ranges_disjoint_nonempty()`,
  `a733_ranges_disjoint_optional_second()` and
  `a733_ranges_overlap_or_wrap()` for its buffer checks;
  `a733_deadline_admit()` in the firmware, radio platform, radio session and
  entropy samplers, and `a733_monotonic_advance()` for the first entropy
  sample; `a733_deadline_after()` where a deadline sum is stored at once.
- The Bluetooth stack (`drivers/bluetooth/*.c` and `include/bt_memory.h`):
  `bt_range_disjoint()` and `bt_range_disjoint_empty()` call
  `a733_ranges_disjoint()` and `a733_ranges_disjoint_or_empty()`; the ACL and
  L2CAP span checks use `a733_span_incl_nonempty()` and the H4 span check
  `a733_span_incl_or_empty()`; the security sampler uses
  `a733_deadline_admit()`; stored deadline sums in the ACL, HCI, L2CAP and SDP
  server units use `a733_deadline_after()`; the AVDTP, hands-free, RFCOMM,
  SCO and security bounds use `a733_deadline_after_saturating()`.
- `apps/bluetooth/headset.c` `cap()` and `bluetooth_cadence()` in
  `apps/bluetooth/bluetooth.h`: `a733_deadline_after_saturating()`.

## Remaining callers

Counts per directory group after the migrated callers. Columns:

- *task*: task-context copies.
- *range*: local range predicates that match a header function or that need a
  local composition of one; *comp.*: object-specific helpers built on them.
- *sampler*: deadline samplers not yet using `a733_deadline_admit()`.
- *sums*: inline `x > UINT64_MAX - y` guards on time values.
- *mmio*: files with DSB-SY-ordered volatile accesses, and the number of such
  access statements.
- *AT*: direct `aarch64_device_translate()` calls.
- *cache*: uses of the DMA or NPU cache enums.

| Group | task | range | comp. | sampler | sums | mmio files | mmio sites | AT | cache |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| drivers/adc | 1 | 2 | 0 | 2 | 0 | 1 | 7 | 0 | 0 |
| drivers/audio | 0 | 1 | 0 | 0 | 3 | 1 | 2 | 0 | 0 |
| drivers/bluetooth | 0 | 0 | 4 | 0 | 4 | 0 | 0 | 0 | 0 |
| drivers/camera | 1 | 1 | 0 | 2 | 1 | 1 | 2 | 0 | 0 |
| drivers/clock | 0 | 3 | 0 | 0 | 0 | 1 | 3 | 0 | 0 |
| drivers/crypto | 0 | 0 | 4 | 1 | 0 | 1 | 6 | 0 | 0 |
| drivers/display | 0 | 11 | 0 | 2 | 11 | 1 | 2 | 2 | 0 |
| drivers/dma | 0 | 1 | 0 | 1 | 2 | 1 | 3 | 0 | 5 |
| drivers/gpu | 0 | 41 | 9 | 1 | 5 | 1 | 2 | 0 | 0 |
| drivers/i2c | 1 | 2 | 0 | 0 | 0 | 1 | 2 | 0 | 0 |
| drivers/iommu | 0 | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| drivers/net | 0 | 0 | 5 | 0 | 3 | 0 | 0 | 0 | 0 |
| drivers/npu | 0 | 14 | 0 | 2 | 1 | 2 | 4 | 1 | 5 |
| drivers/pci | 1 | 5 | 1 | 1 | 1 | 1 | 4 | 0 | 0 |
| drivers/pinctrl | 0 | 0 | 0 | 0 | 0 | 1 | 2 | 0 | 0 |
| drivers/power | 1 | 4 | 2 | 3 | 1 | 1 | 2 | 0 | 0 |
| drivers/pwm | 1 | 2 | 1 | 1 | 0 | 1 | 2 | 0 | 0 |
| drivers/rtc | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| drivers/spi | 1 | 3 | 0 | 1 | 0 | 1 | 5 | 0 | 0 |
| drivers/storage | 1 | 10 | 2 | 2 | 3 | 1 | 3 | 0 | 0 |
| drivers/thermal | 0 | 1 | 0 | 1 | 0 | 1 | 2 | 0 | 0 |
| drivers/uart | 1 | 3 | 0 | 0 | 0 | 1 | 3 | 0 | 0 |
| drivers/usb | 1 | 4 | 1 | 2 | 2 | 2 | 12 | 1 | 0 |
| drivers/video | 0 | 1 | 1 | 0 | 1 | 1 | 3 | 1 | 0 |
| platform/ | 1 | 19 | 2 | 2 | 7 | 15 | 32 | 17 | 9 |
| apps/* (39 applications) | 0 | 3 | 3 | 5 | 23 | 29 | 53 | 48 | 3 |
| boot/ | 0 | 1 | 0 | 0 | 0 | 2 | 9 | 0 | 0 |
| runtime/ | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| Total | 11 | 134 | 35 | 30 | 69 | 68 | 165 | 70 | 22 |

The applications with the most remaining work are `apps/rtc` (five files with
14 ordered accesses and three AT calls), `apps/os` and `apps/hdmi-audio` (five
AT calls each). The counts come from source patterns and the behavioral range
classification; read each copy before replacing it.

## Checks

[checks/shared-helpers.py](../checks/shared-helpers.py) runs the cases in
[checks/shared-helpers.c](../checks/shared-helpers.c):

```sh
~/.cache/sunstoneos/venv/bin/python checks/shared-helpers.py --output /absolute/new/directory
```

- The host build runs at -O0 and -O2 with
  `-fsanitize=address,undefined -fno-sanitize-recover=all`. It compares every
  range predicate with a 128-bit reference formula over the edge values above,
  and has named cases for adjacent ranges, NULL, zero length, ranges that end at
  the top byte, and sums that overflow at UINT64_MAX. Deadline, plain MMIO and
  mapping-check cases use a translation model that records each translated
  address and direction.
- `clang --target=aarch64-none-elf` compiles the task-context and MMIO
  probes twice: with the pinned FreeRTOS single-core SRE port, and with the
  project's patched SMP port and the eight-core configuration. The runner
  disassembles both and requires each barrier placement, the entry checks before
  `msr DAIFSet, #2`, the TPIDR_EL1 read between the mask and the DAIF restore,
  and the scheduler calls after the restore.
- `clang --target=arm-none-eabi -march=armv8-a` compiles the MMIO probes for
  AArch32 with the same barrier checks.

The check uses no board, emulator or FreeRTOS scheduler.
