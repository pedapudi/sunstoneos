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
`aarch64_device_cache()` takes. The NPU memory callbacks in
[a733_npu_memory.h](../include/a733_npu_memory.h) take it directly. The DMA
controller's callback table names the same three operations with
`enum a733_dma_cache`, and `platform/a733/dma_native.c` maps them onto the
AArch64 enum by name.

## Variants and their helpers

The *Files* columns count the C sources under `drivers/`, `platform/`,
`apps/`, `boot/`, `runtime/` and `include/` that call the helper, apart from
the four headers themselves. Local copies that remain are counted under
*Remaining callers*. Local range helpers were classified by behavior: each was
compiled on the host and compared with every header predicate over all
combinations of 15 edge values (0 through 4, 0x1000, 0x1001, the middle of the
address space and the top 4097 addresses). A helper that matched one predicate
on every combination is a copy of it.

### Task context

Every copy masks IRQs while it reads the port's per-core interrupt and
critical-section nesting counters. The SMP port indexes those counters by the
core number in TPIDR_EL1; the single-core port keeps one pair.

| Variant | Meaning | Helper | Callers |
| --- | --- | --- | --- |
| EL1 and owner task | Refuses unless CurrentEL is EL1 and IRQs were unmasked on entry, before masking. Then requires zero nesting, a running scheduler and the given task as the current task. | `a733_task_context_owner_el1(owner)` | 13 files: the ADC, camera, TWI, radio, PCIe, PMIC, PWM, SPI, UART and USB2 native adapters, `platform/a733/hdmi_native.c`, `typec_native.c` and `maintenance_watchdog.c` |
| Any task, no EL check | Always masks and restores DAIF, then requires IRQs unmasked on entry, zero nesting, a running scheduler and any current task. CurrentEL is not read. | `a733_task_context_any_no_el_check()` | `platform/a733/shared_locks.c` |

Instance checks (`initialized`, `self == n`, sealed configuration) stay at the
call site, before the helper. The TWI and camera adapters replace the check
with `a733_camera_model_current()` when `SUNSTONE_CAMERA_MODEL` is defined; that
conditional stays at the call site too.

### Single ranges

The *exclusive end* is p + n; the *last byte* is p + n - 1. A range that ends at
the top byte of the address space has a representable last byte but no
representable exclusive end.

| Variant | Meaning | Helper | Files | Example callers |
| --- | --- | --- | --- | --- |
| Exclusive end, zero allowed | p non-NULL; p + n fits | `a733_span_excl` | 10 | `drivers/net/aic8800_*.c`, `drivers/storage/sunxi_sdio.c` |
| Exclusive end, nonempty | p non-NULL; n nonzero; p + n fits | `a733_span_excl_nonempty` | 26 | `drivers/gpu/pvr_*.c`, `drivers/npu/nbg_*.c`, `platform/a733/gpu_memory.c` |
| Last byte, nonempty | p non-NULL; n nonzero; p + n - 1 fits | `a733_span_incl_nonempty` | 6 | `drivers/bluetooth/bt_acl.c`, `drivers/npu/npu_session.c` |
| Last byte, empty accepted | n zero accepted with any p, including NULL | `a733_span_incl_or_empty` | 2 | `drivers/bluetooth/bt_h4.c`, `drivers/uart/a733_uart.c` |
| Last byte, empty only when absent | n zero accepted only with NULL | `a733_span_incl_or_null_empty` | 1 | `drivers/spi/a733_spi.c` (32-bit length) |
| Containment | [start, start + n) inside [base, base + bytes), no sums formed | `a733_extent_within` | 3 | `contains` in `drivers/gpu/pvr_compute_programs.c` and `inside` in `pvr_compute_job.c`, which take structures; `drivers/npu/nbg_model.c` |

### Pairs of ranges

Pointer forms (`a733_ranges_*`) apply a NULL rule; extent forms
(`a733_extents_*`) take 64-bit integer addresses and treat address zero as an
ordinary address. A *wrapping* range is one whose exclusive end does not fit.

| Variant | Meaning | Helper | Files | Example callers |
| --- | --- | --- | --- | --- |
| Disjoint, NULL and wrap refused | Both non-NULL, both ends fit, no shared byte; zero length allowed | `a733_ranges_disjoint` | 53 | the native adapters, the crypto engine and PCIe drivers, `drivers/usb/a733_typec.c`; `bt_range_disjoint()` in `include/bt_memory.h` |
| Disjoint and nonempty | As above, and both lengths nonzero | `a733_ranges_disjoint_nonempty` | 10 | `drivers/gpu/pvr_cccb.c`, `platform/a733/npu_reserved.c`, `platform/a733/dma_startup.c` |
| Empty ranges are disjoint | Zero length is disjoint from everything, even NULL | `a733_ranges_disjoint_or_empty` | 1 | `bt_range_disjoint_empty()` in `include/bt_memory.h` |
| Optional second range | First range required; NULL second range is absent | `a733_ranges_disjoint_optional_second` | 2 | `drivers/net/aic8800_dispatch.c`, `aic8800_network.c` |
| Fail-closed overlap | True when either end wraps or a byte is shared; no NULL rule | `a733_ranges_overlap_or_wrap` (pointers), `a733_extents_overlap_or_wrap` (integers) | 16 | `drivers/display/*.c`, `drivers/camera/*.c`, `drivers/pinctrl/a733_pins.c`, `drivers/iommu/a733_iommu.c` |
| Fail-closed, empty never overlaps | Zero length never overlaps; otherwise fail-closed | `a733_extents_overlap_nonempty_or_wrap` | 1 | `drivers/display/a733_dsi.c` |
| Disjoint, wrap refused, no NULL rule | Both ends fit and no shared address | `a733_extents_disjoint_checked` | 3 | `drivers/display/a733_dp.c`, `a733_display_pll.c`, `drivers/gpu/a733_gpu_platform.c` |
| Intersection by distance | Integer intervals intersect; zero length never does; no wrap concept | `a733_extents_intersect` | 7 | `drivers/npu/a733_npu_memory.c`, `npu_session.c`, `drivers/spi/a733_spi.c`, `drivers/uart/a733_uart.c` |
| Disjoint, unchecked | `a + an <= b \|\| b + bn <= a`, modular | `a733_extents_disjoint_prechecked` | 22 | `drivers/gpu/pvr_*.c`, `drivers/npu/nbg_assembly.c`, `platform/a733/gpu_memory.c` |
| Overlap, unchecked | `a < b + bn && b < a + an`, modular | `a733_extents_overlap_prechecked` | 24 | `drivers/net/aic8800_*.c`, `drivers/dma/*.c`, `drivers/npu/nbg_*.c`, `runtime/device_memory_pool.c` |

The unchecked forms are correct only when the caller has already established
that both ends fit, usually through a span check on the same ranges. With a
wrapping input they can report overlapping ranges as disjoint.

Thirty-five local helpers combine these predicates with object-specific
ranges, for example `outside()` in the native adapters, which tests a caller
buffer against the adapter instance and its locks. They stay local and call the
shared predicate inside. Three one-line wrappers, `overlap` in
`drivers/spi/a733_spi.c` and `drivers/uart/a733_uart.c` and `separate` in
`drivers/npu/nbg_assembly.c`, convert pointers to integers and call the header
function. Four helpers match no header function and stay local:
`drivers/npu/nbg_sh_nt.c` `overlap` and `separate` (unchecked forms that treat
zero length as disjoint), `drivers/net/a733_radio_entropy.c` `contains`, and the
32-bit window check `range` in `platform/a733/cpu_entry_native.c`.

### Deadlines

| Variant | Meaning | Helper | Files | Example callers |
| --- | --- | --- | --- | --- |
| Admit a sample | Refuse a sample earlier than the last one or at or after the deadline; store it only on admission | `a733_deadline_admit(now, &last, deadline)` | 51 | `timely()` in `drivers/thermal/a733_thermal.c`, `drivers/adc/a733_adc.c`, `drivers/crypto/a733_ce.c` |
| Store, then compare | Refuse only a backward sample, store it, then compare with one or more deadlines | `a733_monotonic_advance(now, &last)` followed by the caller's comparisons | 16 | `timely()` in `drivers/pci/a733_pcie.c`, `drivers/pwm/a733_pwm.c`, `drivers/storage/a733_mmc_platform.c`, `drivers/usb/a733_usb2_platform.c` |
| Plain comparison | `now < deadline` with no stored sample, or with extra state | none | 7 local | `drivers/gpu/pvr_compute_service.c`, `platform/a733/gpu_disposal.c`, `apps/rtc-returning/main.c` |
| Overflow-checked sum | `*out = now + delta` unless the sum overflows | `a733_deadline_after(now, delta, &out)` | 53 | `drivers/usb/a733_typec.c`, `drivers/usb/a733_typec_pd.c`, `drivers/net/aic8800_*.c` |
| Saturating sum | `now + delta`, or UINT64_MAX when the sum overflows; a saturated bound never expires | `a733_deadline_after_saturating(now, delta)` | 7 | `drivers/bluetooth/bt_avdtp.c`, `bt_hfp_ag.c`, `bt_rfcomm.c` and `bt_sco.c`; `apps/bluetooth/headset.c` |

The caller samples its clock and refuses a failed sample before calling
`a733_deadline_admit()`. Several samplers also record a failure reason or test a
retained fault; those parts stay in the caller.

### MMIO barrier placement

Native adapters order register accesses with DSB SY and a compiler memory
clobber. The accessor name states where the barriers are.

| Variant | Meaning | Helper | Files | Example callers |
| --- | --- | --- | --- | --- |
| Fenced on both sides | DSB SY; access; DSB SY | `a733_mmio_readN_dsb_sy_both`, `a733_mmio_writeN_dsb_sy_both` | 49 | `read32`/`write32` in the native adapters other than the PMIC adapter's S_TWI0 accesses, `apps/*/native.c` register loads |
| Completion after | Access; DSB SY | `a733_mmio_readN_dsb_sy_after`, `a733_mmio_writeN_dsb_sy_after` | 29 | `platform/a733/dma_native.c` reads, `apps/rtc/*.c` writes, `apps/npu-interrupt/irq-route.c`, the PMIC adapter's S_TWI0 control write (unqualified on the board; [pmic-runtime.md](pmic-runtime.md)) |
| Fenced sequence | DSB SY, then several accesses, such as a read-modify-write check | `a733_mmio_dsb_sy()` with the plain accessors | 21 | read-modify-write paths in the native adapters, `apps/rtc/*.c` status reads |
| Plain | One volatile access, no barrier, no memory clobber | `a733_mmio_readN`, `a733_mmio_writeN` | 24 | the accesses that follow `a733_mmio_dsb_sy()`; `drivers/pci/a733_pcie_native.c` 8- and 16-bit accesses |

A file can contain more than one placement. The barrier forms compile for
AArch64 and AArch32; the plain accessors also compile on the host. Register
allowlists, mapping checks and lock checks stay with each caller.

### Mapping checks

The mapping checks pass `aarch64_device_translate()`, or another function with
the `a733_translate_fn` signature, to a walk that checks that addresses
translate with an expected physical address, MAIR attribute value and
shareability (PAR_EL1.SH: 0 non-shareable, 2 outer, 3 inner). Fifty-five files
use them. A `struct a733_mapping_rule` states the rule:

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
| Device registers, write, outer shareable | `WRITE`, `{0x00, OUTER}`, identity | `device` in `drivers/video/a733_video_unit.c` and `apps/g2d/native.c`, `h264_device` in `apps/h264/native.c`; page lists in `apps/adc/native.c` |
| Device registers, one direction | `READ` or `WRITE`, `{0x00, ANY}`, identity | `platform/a733/maintenance_watchdog.c` `mappings`, `apps/gpu-finite/finite.c` |
| NPU device registers | `{0x00, ANY}` or `{0x04, ANY}`, given physical address | `device_mapping` in `platform/a733/npu_native.c` |
| Write-back pools and images | `WRITE` or `READ`, `{0xff, INNER}`, identity | `memory_reservations.c`, `dma_pool_session.c` `image`, `gpu_reserved.c` `private_image` |
| Write-back with offset | `WRITE`, `{0xff, INNER}`, physical offset, 64-byte stride | descriptor memory in `apps/crypto/native.c` |
| Normal-NC pool | `READ_WRITE`, `{0x44, OUTER \| INNER}`, same SH, identity | `memory_uncached.c`, NC pages in `device_memory_layout.c` |
| Display mapping | `READ_WRITE`, given attribute with `OUTER \| INNER` unless the attribute is 0x00, same SH, given physical address | `a733_display_native_mapping()` |
| USB2 buffers | `WRITE`, given attribute with `OUTER \| INNER` for 0x44 and `ANY` otherwise, identity | `mappings` in `drivers/usb/a733_usb2_native.c` |
| GPU output spans | `WRITE`, `{0xff, ANY}` or `{0x44, ANY}`, physical address not compared | `output_span` in `platform/a733/gpu_backend.c` |
| HDMI audio buffers | `READ` or `WRITE`, `{0x44, OUTER \| INNER}`, given physical address, 4 KiB stride; the PCM source pages `READ`, `{0xff, INNER}`, identity | `dma_mapping` and `hdmi_audio_boot` in `apps/hdmi-audio/native.c` |

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
`a733_npu_memory_ops.translate`, which also takes a cookie, so host checks can
model it. Its own page loop checks each translation with
`a733_mapping_translation_ok()`, the per-result test that the walks use.

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

## Callers

The table counts, per directory, the C files that call at least one function
of each header. A file that uses several headers is counted once in *Files*.

| Group | Files | task | range | deadline | mmio | mapping |
| --- | --- | --- | --- | --- | --- | --- |
| drivers/adc | 3 | 1 | 2 | 2 | 1 | 0 |
| drivers/audio | 3 | 0 | 1 | 1 | 1 | 0 |
| drivers/bluetooth | 10 | 0 | 3 | 9 | 0 | 0 |
| drivers/camera | 7 | 1 | 7 | 2 | 1 | 0 |
| drivers/clock | 4 | 0 | 3 | 3 | 1 | 0 |
| drivers/crypto | 7 | 0 | 7 | 5 | 1 | 0 |
| drivers/display | 13 | 0 | 11 | 13 | 1 | 1 |
| drivers/dma | 7 | 0 | 4 | 2 | 1 | 0 |
| drivers/gpu | 26 | 0 | 23 | 4 | 3 | 0 |
| drivers/i2c | 2 | 1 | 2 | 1 | 1 | 0 |
| drivers/iommu | 1 | 0 | 1 | 1 | 0 | 0 |
| drivers/net | 13 | 1 | 12 | 8 | 1 | 0 |
| drivers/npu | 13 | 0 | 10 | 3 | 2 | 2 |
| drivers/pci | 5 | 1 | 5 | 1 | 1 | 0 |
| drivers/pinctrl | 2 | 0 | 1 | 0 | 1 | 0 |
| drivers/power | 5 | 1 | 4 | 4 | 1 | 0 |
| drivers/pwm | 2 | 1 | 2 | 1 | 1 | 0 |
| drivers/rtc | 2 | 0 | 0 | 2 | 0 | 0 |
| drivers/spi | 3 | 1 | 2 | 2 | 1 | 0 |
| drivers/storage | 7 | 0 | 7 | 4 | 0 | 0 |
| drivers/thermal | 2 | 0 | 1 | 1 | 1 | 0 |
| drivers/uart | 3 | 1 | 2 | 1 | 1 | 0 |
| drivers/usb | 8 | 1 | 4 | 6 | 2 | 1 |
| drivers/video | 7 | 0 | 1 | 6 | 1 | 1 |
| drivers/watchdog | 1 | 0 | 0 | 1 | 0 | 0 |
| runtime/ | 1 | 0 | 1 | 0 | 0 | 0 |
| platform/ | 21 | 4 | 14 | 3 | 9 | 12 |
| include/ | 4 | 0 | 3 | 1 | 0 | 1 |
| apps/* (40 applications) | 63 | 0 | 4 | 25 | 28 | 37 |
| Total | 245 | 14 | 137 | 112 | 61 | 55 |

The headers under `include/` that call the helpers are `bt_memory.h`
(`bt_range_disjoint()` and `bt_range_disjoint_empty()`),
`a733_camera_common.h`, `a733_dma_controller.h` and `a733_gpu_mapping.h`.

## Remaining callers

Counts per directory group of the local code that does not use the headers.
Groups with nothing left are omitted. The range columns cover static helpers
named `apart`, `contains`, `cpu_range`, `disjoint`, `distinct`,
`independent`, `inside`, `outside`, `overlap`, `overlaps`, `range`,
`separate`, `span` and `valid_span`; inline tests, such as the overlap test in
`nbg_relocate()` in `drivers/npu/nbg_relocation.c`, are not counted. Columns:

- *task*: task-context copies.
- *range*: local range helpers that are copies of a header function.
- *unmatched*: local range helpers that match no header function.
- *comp.*: object-specific range helpers that take structures or combine
  several ranges; most call a header predicate inside.
- *sums*: inline `x > UINT64_MAX - y` guards on time values.
- *mmio*: files with DSB-SY-ordered volatile accesses, and the number of such
  access statements.
- *AT*: direct `aarch64_device_translate()` calls outside the mapping-check
  walks.
- *cache*: uses of `enum a733_dma_cache` and its constants.

| Group | task | range | unmatched | comp. | sums | mmio files | mmio sites | AT | cache |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| drivers/audio | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 |
| drivers/bluetooth | 0 | 0 | 0 | 4 | 4 | 0 | 0 | 0 | 0 |
| drivers/crypto | 0 | 0 | 0 | 4 | 0 | 0 | 0 | 0 | 0 |
| drivers/display | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| drivers/dma | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 5 |
| drivers/gpu | 0 | 0 | 0 | 9 | 0 | 0 | 0 | 0 | 0 |
| drivers/net | 0 | 0 | 1 | 5 | 4 | 0 | 0 | 0 | 0 |
| drivers/npu | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 |
| drivers/pci | 0 | 0 | 0 | 1 | 1 | 0 | 0 | 0 | 0 |
| drivers/power | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 |
| drivers/pwm | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| drivers/storage | 1 | 0 | 0 | 2 | 0 | 1 | 3 | 0 | 0 |
| drivers/usb | 0 | 1 | 0 | 1 | 0 | 1 | 3 | 0 | 0 |
| drivers/video | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| platform/ | 0 | 5 | 1 | 1 | 3 | 9 | 16 | 1 | 5 |
| boot/ | 0 | 1 | 0 | 0 | 0 | 2 | 9 | 0 | 0 |
| apps/* | 0 | 0 | 0 | 4 | 11 | 1 | 4 | 2 | 3 |
| Total | 1 | 7 | 4 | 35 | 27 | 14 | 35 | 3 | 13 |

The copies that a migration could replace are:

- Task context: `current()` in `drivers/storage/a733_mmc_platform_native.c`
  matches `a733_task_context_owner_el1()` after its instance check.
- Ranges: `separate()` in `drivers/usb/a733_usb_platform.c` and in
  `platform/a733/cpu_entry.c`, `cpu_start.c` and `cpu_continue.c` match
  `a733_ranges_disjoint()`; `overlap()` in `platform/a733/cpu_coherence.c`
  matches `a733_ranges_overlap_or_wrap()`; `disjoint()` in
  `platform/a733/cpu_entry_native.c` and `boot/a733/stage2/loader.c` matches
  `a733_extents_disjoint_prechecked()`.
- Deadlines: `timely()` in `drivers/usb/a733_usb_platform.c` admits a sample
  as `a733_deadline_admit()` does and also records a failure, which would stay
  at the call site. `later()` in `apps/board/radio.c` matches
  `a733_deadline_after_saturating()`. The table does not count samplers,
  because many are inline tests in functions with other names. Examples are
  `clock_ok()` in `drivers/uart/a733_uart.c`, `time_ok()` in
  `drivers/audio/a733_i2s_platform.c`, the operation samplers of
  `drivers/crypto/a733_ce_cipher.c`, `a733_ce_hash.c` and `a733_ce_entropy.c`,
  and tests in the Wi-Fi units `drivers/net/aic8800_*.c`. Several of them
  record a failure or test other state in the same expression.
- MMIO: the AArch32 `rd`/`wr` accessors of the five `platform/*/launch.c`
  files place the barrier after the access; the other ordered accesses are in
  `drivers/usb/a733_usb_platform_native.c`,
  `drivers/storage/a733_mmc_platform_native.c`, the SMP platform,
  interrupt-table and observer USB units under `platform/`,
  `apps/storage-read-single-core/monitor.c` and the stage-2 loader and
  admission under `boot/a733/stage2/`.
- Mapping checks: the direct calls in `platform/a733-smp-cached/devices.c`,
  `apps/board/main.c` (the radio profile pages) and
  `apps/storage-read-single-core/runtime.c`.

`enum a733_dma_cache` is the cache operation type of the DMA controller's
callback table, so its uses stay with that interface. The counts come from
source patterns and the behavioral range classification; read each copy before
replacing it.

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
