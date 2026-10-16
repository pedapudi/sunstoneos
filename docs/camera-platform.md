# Cold CSI analog initialization retains shared ownership

The [native analog controller](../drivers/camera/a733_camera_platform.c)
initializes the shared CSI analog PHY from a proven reset state. It programs
the reference-voltage trim, releases analog resets, enables the reference and
LDO, and verifies software termination-resistor calibration. The implementation
passes source/model checks and native compilation. It has not run on the board.

The [API](../include/a733_camera_platform.h) is a bounded controller around
admission and ordered MMIO callbacks. Native clock, power, reset and pin-manager
bindings remain outstanding. An `ACTIVE` result means that this analog sequence
and its readbacks completed. It does not establish camera capture readiness or
physical analog qualification.

## Admission establishes reset provenance and excludes all other users

Common PHY registers at `0x05810000` affect PHY A, B and C. Admission reserves
both complete common register words and excludes every other writer for the
controller's lifetime. It also excludes all three PHY/packet paths, holds
sensors quiet and owns the required clocks, resets, rails, pins and interrupt
routing. Shared-register read/modify/write operations span separate polls.

The admission record identifies either an independently established cold reset
or a resolved boot handoff that establishes the same reset state. Both origins
require explicit evidence of reset, resolved inherited ownership, drained prior
transactions and independent recovery. Neither origin permits adoption of an
already active analog PHY. No native provider currently supplies this evidence.

Before any write, the controller records the common control and calibration
registers, each PHY control register and each packet-port control register.
It rejects enabled common power/reset controls, external reference selection,
PRBS test selection or a nonzero link mode. Enabled lane/current controls or an
enabled packet port also cause refusal. These observations corroborate the
admission record; zero register values cannot establish reset provenance.

The controller retains its token after success and after faults. It provides
no shutdown or release operation. A higher-level owner must keep the shared
analog resources alive for every receiver that uses them. Fault recovery must
retain exclusion until an independent recovery procedure resolves the hardware
state and all outstanding users.

## Software calibration writes SET and reads RESULT

The A733 manual defines calibration register `0x04` as follows:

| Bits | Access | Meaning |
| --- | --- | --- |
| `20:16` | Read-only | RESULT; reflects SET in software mode |
| `8:4` | Read/write | SET; requested software calibration code |
| `3` | Read-only | FLAG; fixed at one in software mode |
| `2` | Read/write | RESETN; applies only to automatic calibration |
| `1` | Read/write | SOFT; enables software calibration |
| `0` | Read/write | AUTO; enables automatic calibration |

The controller selects SET `0x10`, enables SOFT and disables AUTO. It preserves
the initial RESETN value without toggling that automatic-only reset. The written
word contains only documented read/write fields; RESULT, FLAG and reserved
fields are zero in the write value. Readback must confirm the control fields.

After analog power setup, polling requires FLAG equal to one and RESULT equal
to `0x10`, with a fixed caller deadline. A missing reflection remains pending
until that deadline; changed control fields fail immediately. Software-mode
reflection confirms the requested code, not a measured termination resistance.

Both inspected Linux BSP versions erroneously use the RESULT bit position as
a write mask. The native sequence follows the manual's read-only semantics.
It also selects software mode alone; the source enables both SOFT and AUTO.
The manual explicitly defines the software reflection but does not specify
precedence when both enables are set.

## Analog enable order follows the A733 source

The fixed profile uses link mode zero, internal reference selection, disabled
PRBS testing and raw trim bits `0xb00`. The controller programs these settings
with analog power and reset controls still clear, then writes software
calibration. It releases PWDNZ, releases RSTN, enables VREF and enables LVLDO
in that order. Each write completes with an ordering barrier and masked readback.
Final verification requires the complete configured control profile.

The A733 source uses trim codes two at bits `11:10` and three at bits `9:8`.
Its names for the 0.2 V and 0.9 V fields are reversed relative to the manual.
Admission therefore requires independent approval of the raw board profile;
source agreement alone cannot supply that approval. The controller preserves
these source values without inventing a correction to either field name.

Neither the inspected enable sequence nor the cited register descriptions
specifies a delay between these analog writes. The controller adds no arbitrary
delay. Platform admission must establish stable upstream power and clocks,
including any board-required settling interval, before common-register access.
Hardware qualification must resolve the trim discrepancy and any additional
analog settling requirement before this profile can be admitted on the board.

Every poll performs at most one admission or MMIO operation plus monotonic
time checks. Pending admission retains the same token. Failed accesses, changed
tokens, dropped writes, deadline expiry and time regression cause terminal
faults. Subsequent polling performs no further operations after a fault.

## Capture also requires mappings, receiver control and buffer completion

Analog readiness does not install the system IOMMU or authorize DMA traffic.
The IOMMU controller requires its own global ownership and resolved handoff
across both banks and all ten masters. Quiet camera hardware alone cannot
satisfy that requirement.

The [capture route](camera-route.md) separately needs a readable VIPP parameter
mapping and writable Y/UV image mappings. For the fixed 640×480 NV16 profile,
the parameter image occupies 1,024 bytes in an independently reserved page;
each image plane occupies 307,200 bytes. Page ownership, actual prefetch guards,
cache maintenance and mapping lifetime remain platform responsibilities.

The global owner must bind admitted analog resources to the receiver, install
the mappings, publish parameters and prepare DMA before starting the sensor.
Receiver and route readiness do not return image-buffer ownership to the CPU.
That transfer follows DMA capture completion, producer quiescence, hardware
disable, transaction drain, final error checks and cache synchronization.

## Focused checks cover only the new analog behavior

The source check pins the Orange Pi kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` and executes its unchanged trim,
reset-release and power-enable helpers. The native final control register and
each power/reset write match those helpers. Calibration uses an independent
model of the manual's writable fields and read-only software reflection.

The model checks admission evidence, cold-state refusal, delayed reflection,
retained tokens, dropped writes, late operations, clock regression and access
failures at every callback boundary. AddressSanitizer and
UndefinedBehaviorSanitizer report no errors. The AArch64 object contains 1,892
bytes of code/read-only data, no mutable globals and no undefined symbols.
Its largest direct stack frame is 64 bytes. These are build facts, not
performance measurements.

Exact inputs, commands, logs, binaries and source snapshots are retained in
`~/.cache/sunstoneos/evidence/camera-platform-20261016/`. Existing sensor,
receiver, route and DMA checks were not rerun. The analog controller adds no
physical qualification evidence.

Primary references:

- [A733 camera source](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/vin/vin-mipi/combo_csi):
  `combo_csi_reg.c`, SHA256
  `e6368908f8393707c82aafaf7704af52a1c2b26c829b54553c123ed8bcfe09b4`,
  and `combo_csi_reg_i.h`, SHA256
  `33cd56ebac15fcb1b9457b221d983f004576a0ebf0766224f4b256dbafdf282d`.
- [A733 manual documentation project](https://gitlab.com/tina5.0_aiot/product/docs/-/tree/d0dc4d615be00b76e3a0818d0818eff8f69ffa15):
  `A733_User Manual_V1.00.pdf`, SHA256
  `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
  Sections 16.1.10.1–16.1.10.2, printed pages 885–887, define common analog
  controls and calibration. Section 16.1.11.1 defines local PHY enable controls.
