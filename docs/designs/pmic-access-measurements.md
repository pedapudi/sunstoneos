# PMIC register access and service spin timing: measurements

These measurements compare the provider, native adapter and service of
[pmic-access.md](pmic-access.md) with the base tree, bringup commit
`7816755f`. Every figure comes from a host model, a compiled object or a
linked image; no board run has used the changed provider.

## Conditions

- **Base**: `git archive 7816755f` into
  `~/.cache/sunstoneos/build/pmic-access/base-src`.
- **Head**: this branch with the changes of the commit that adds this
  document.
- **Host models**: clang 21.1.8 with `-std=c11 -O1 -Wall -Wextra -Werror
  -fsanitize=address,undefined`. Each bus flag sets at a sub-microsecond
  time: the completion time of the write that starts the phase, in
  nanoseconds, plus the phase's wire time.
- **Base service figures** come from the base drivers compiled with this
  tree's `checks/a733-pmic.c` and `checks/power-domains-service.c`, with the
  assertions on step times removed, because the base provider fails them.
  That copy is `~/.cache/sunstoneos/build/pmic-access/measure-base/`.
- **Images**: the project's AArch64 toolchain through the session's
  configure script, with the application options listed under
  "Images rebuilt".

Percentages are of the bit-time wire time at 400 kHz: 252 bit times of
2,500 ns, 630 µs, for a snapshot and 325 bit times, 812.5 µs, for a
selector write. The model's bytes take 23 µs, so its own snapshot wire is
642 µs and its selector-write wire 828 µs.

## Budget rows

| Metric | Base | Head | Measured by |
| --- | --- | --- | --- |
| Time reads per DCDC2 snapshot; per DCDC3 selector write | 469; 599 | 180; 229 | `checks/a733-pmic-cost.c`, both trees; `--limits` passes on head |
| Register accesses per snapshot; per selector write | 211; 270 | 200; 255 | Same |
| Serial accesses from a completed bus phase to the next phase-start write, per snapshot | 161 over 42 phase boundaries | 150 over 42 phase boundaries | Measurement harness over the provider model, `~/.cache/sunstoneos/build/pmic-access/serial/serial.c` |
| Task checks per register access | 1 (`read32()` and `write32()` each call `xTaskGetSchedulerState` and `xTaskGetCurrentTaskHandle` and write DAIF twice) | 0 (no call, no DAIF access); one per poll and one per bus entry | Disassembly of `a733_pmic_native.c.obj` of the board image; `checks/pmic-tree.py` passes on head and fails on base |
| Static instructions of `read32()`; of `write32()` | 156; 84 | 107; 34 | Same disassembly; each keeps two `DSB SY` and one `LDAR` |
| Snapshot step, 400 kHz, 200 ns per access, 40 ns per time read | 712 µs (113%) | 685 µs (109%) | `checks/power-domains-service.c` |
| Selector write, same costs | 920 µs (113%), one step | 883 µs (109%), one step | Same |
| Snapshot step, 400 kHz, 850 ns per access, 100 ns per time read | 854 µs (136%) | 758 µs (120%) | Same, board-cost table |
| Snapshot step, 900 ns | 856 µs (136%) | 782 µs (124%) | Same |
| Snapshot step, 1,300 ns | 947 µs (150%); 915 µs (145%) with 40 ns per time read, the fit to the board's median | 845 µs (134%) | Same |
| Selector write at 850 ns; 900 ns; 1,300 ns | Two steps, one tick wait, at each | One step of 975 µs (120%); two steps, one tick wait; two steps, one tick wait | Same |
| Flags 10% later than the 400 kHz divisor implies (25 µs bytes, 6 µs conditions), 200 ns per access | Snapshot and selector write each two steps with one tick wait | Snapshot one step of 751 µs, selector write one step of 968 µs (708 µs and 913 µs of wire) | Same, slow-bus case |
| Tick waits, 100 kHz, snapshot and selector write | 2 and 3 | 2 and 3 | Same |
| BUSY results per other-core caller; from `a733_pmic_live` | At most 1; 0 | At most 1; 0 | Same |
| Supply verification after the service's first step, 400 kHz; 100 kHz | 8,438 µs; 16,218 µs | 8,383 µs; 16,032 µs | Same |

The existing 110% assertion of the 400 kHz case at 200 ns per access stays.
It compares the step with the model's own wire time, 642 µs for a snapshot
and 828 µs for a selector write. The base provider fails it once flags set
at sub-microsecond times (712 µs and 920 µs); the head provider passes
(685 µs and 883 µs). With whole-microsecond flags, as before this change,
the base gave 687 µs and 885 µs.

The service check asserts every head figure in the table that it prints: one
snapshot step within 155% of the bit-time wire at each board cost, the
selector-write step counts of the table, and one step within 110% of the
slow bus's own wire time.

## Images rebuilt

Every target whose link includes the provider, the native adapter or the
service was built from the head tree and the base tree with
`-DSUNSTONE_A733_THERMAL=ON -DSUNSTONE_A733_ADC=ON -DSUNSTONE_A733_MONITOR=ON
-DSUNSTONE_A733_PCIE=ON -DSUNSTONE_A733_PCIE_TEST=ON -DSUNSTONE_A733_PWM=ON
-DSUNSTONE_A733_SPI=ON -DSUNSTONE_A733_STORAGE_READ=ON
-DSUNSTONE_A733_DEINTERLACE=ON -DSUNSTONE_A733_CRYPTO=ON
-DSUNSTONE_A733_RTC=ON -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON
-DSUNSTONE_A733_OS=ON -DSUNSTONE_GPU_SUPERVISED=ON
-DSUNSTONE_CAMERA_SUPERVISED=ON -DSUNSTONE_HDMI_FRAMEBUFFER=ON
-DSUNSTONE_BLUETOOTH_APPLICATION=ON`. All 23 targets link:
`sunstone-a733-board`, `-os`, `-mimus-board`, `-mimus-native`,
`-gpu-render`, `-hdmi`, `-camera`, `-storage-maintenance`, `-storage-read`,
`-thermal`, `-adc`, `-monitor`, `-pwm`, `-pwm-single-core`, `-spi`, `-rtc`,
`-pcie`, `-pcie-test`, `-crypto`, `-crypto-single-core`, `-deinterlace`,
`-bluetooth` and `npu-startup-model`. Each image's `.text` is between 64 B
and 2,112 B smaller than the base's, except `npu-startup-model`, which does
not link the native adapter and keeps the same size. The provider object
shrinks from 12,340 B to 12,084 B of code and the adapter from 1,928 B to
1,580 B; the service grows from 7,344 B to 7,376 B.

The NPU board image and the supervised NPU application need reviewed
artifacts that this build does not have, and were not built. The OS image
variants with A76 admission, resident mode or the CPU clock driver were not
built separately; they compile the same provider units as the OS image.

The returning images build from the head tree with
`--platform-build ~/.cache/sunstoneos/build/pmic-access/central`:

- **Native USB channel image** (`apps/usb-returning`, rank 9): the provider,
  adapter and consumer objects, `main.c` and `platform/a733/native_usb.c`
  differ from the base. The last three differ only in field offsets, because
  `struct a733_pmic` grew by `phase_at`. Its linked check passes 27 cases.
- **Returning thermal image** (`apps/thermal-returning`, `--device thermal`,
  rank 6): each of the seven units it compiles from the tree has the same
  instructions in both trees, and its ELF on the board run has no
  `a733_pmic_` symbol. Its linked check passes 13 cases.

## Checks run on head

| Check | Result |
| --- | --- |
| `checks/a733-pmic.c`, `-polls`, `-waits`, `-parent`, `-mmc`, `-cost` (with `--limits`) | Pass |
| `checks/power-domains-service.c`, `checks/power-domains-pck.c` | Pass |
| `checks/npu-startup-host.c` (with `checks/npu-startup-host/` headers) | Pass |
| `checks/a733-storage-native-power.c`, `isp0-provider-profile.c`, `monitor-application-nominal.c`, `a733-radio-platform.c`, `pwm-service-pmic.c`, `pwm-service.c`, `a733-pwm.c` | Pass |
| `checks/a733-camera-native.py`, `checks/a733-mmc-platform.py`, `checks/cpu-clock-check.py` | Pass |
| `checks/pmic-tree.py` | Pass; fails on base, as intended, and fails on a tree with a planted per-access task check or a planted adapter-read call outside the provider |
| `apps/thermal-returning/check.py`; `apps/usb-returning/check.py` | Pass, 13 and 27 cases |
| Pure-host checks that `checks/check-index.py --changed` selects | 13 pass; `audit-build-safety-configure.py`, `power-domains-tree.py` and `provider-clock-reference.py` fail on the base tree too |
| `checks/hdmi-framebuffer.py` | Fails to link (`vTaskEnterCritical` undefined) on the base tree too |
| `checks/check-index.py --verify` | Pass with the regenerated index |

## Reproduction

```sh
P=~/.cache/sunstoneos/build/pmic-access
git archive 7816755f | tar -x -C $P/base-src
# In each tree, for example the cost check:
clang -std=c11 -O1 -Wall -Wextra -Werror -Iinclude -Ichecks checks/a733-pmic-cost.c \
    drivers/power/a733_pmic.c drivers/power/a733_rail_leases.c drivers/clock/a733_clock_leases.c \
    drivers/gpu/a733_gpu_resources.c drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c \
    -o cost && ./cost --limits
# The service check adds drivers/power/a733_power_domains.c, a733_pmic_consumer.c
# and drivers/gpu/a733_gpu_power.c to checks/power-domains-service.c.
```

The image lists and digests are in `$P/images.txt`; build logs, check logs and
the comparison objects are under `$P`.

## Board evidence still required

The figures above do not replace the board measurements of
[pmic-access.md](pmic-access.md): the access-cost windows and step times of
the next bundle 6, a DCDC3 selector write in one step, and the rank 9 native
USB image rerun on the changed provider.
