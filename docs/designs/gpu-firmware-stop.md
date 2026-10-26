# Stopping the GPU firmware and its first command

Status: **step (a)+(b) implemented, host-checked and frozen for port 3-1;
not yet run on a board.**

The firmware start passed on port 3-1: the open firmware reported started
672 µs after the release of its processor
([the firmware-start design](gpu-firmware-start.md)). That image leaves the
GPU on with the firmware running, so its lifetime ends with a power removal.
This note derives from the vendor's host driver how the firmware is stopped
before the GPU powers off, and which command the firmware acknowledges
first, and gives the board steps that start the firmware, send it a
command, stop it and power the GPU off, so that a lifetime can continue.

**Sources.** The vendor's sequence comes from the Imagination DDK host
driver and the `rgx_sunxi/` platform glue as shipped in orangepi
`linux-orangepi` `2ac08e8c`, cached with their URLs and SHA-256 in
`~/.cache/sunstoneos/sources/gpu-fw-start-ddk-2ac08e8c/manifest.json`
(paths below relative to `rogue_km/`). The command numbers, structure
layouts and power states come from the open firmware's interface, the
upstream Linux headers in
`~/.cache/sunstoneos/sources/gpu/board-2ac08e8c…/imagination/`, because the
board runs the open firmware. The firmware binary is never disassembled
(`LICENSE.powervr` line 14).

## The vendor's power-off

`PVRSRVSetDevicePowerStateKM` (`services/server/common/power.c:894`) powers
the GPU off in this order (`power.c:727–790`):

1. **Device pre-power: `RGXPrePowerState`**
   (`services/server/devices/rgxpower.c:358`):
   1. `RGXSendPowerOffKick` (lines 205–248) clears the power-sync word,
      writes one `RGXFWIF_KCCB_CMD_POW` command with `RGXFWIF_POW_OFF_REQ`
      into the kernel CCB, kicks the firmware, and polls the power-sync word
      for 1.
   2. It then requires the firmware's power state `ePowState` in system data
      to be `RGXFWIF_POW_OFF`; any other state means the firmware denied the
      transition, and the GPU is not powered off (lines 410–428).
   3. `RGXFinalisePowerOff` (lines 257–344) marks the OS connection offline,
      waits for the firmware's interrupts to drain, closes the timer
      correlation, and calls `RGXStop` through `RGXDoStop`. A failure of
      `RGXStop` is logged and ignored, "since the power was removed"
      (lines 333–341).
2. **System pre-power: `sunxiPrePowerState`**
   (`rgx_sunxi/sunxi_platform.c:316`), which does nothing for OFF.
3. **System post-power: `sunxiPostPowerState`**
   (`rgx_sunxi/sunxi_platform.c:361`): runtime power put and clocks off, the
   platform OFF that SunstoneOS's default path reproduces.

The forced-idle request comes first where the vendor powers the GPU off
right after its initialisation: `PVRSRVDeviceIdleRequestKM`
(`services/server/common/pvrsrv.c:3085`) calls `RGXForcedIdleRequest`
(`rgxpower.c:1519–1610`), which sends `RGXFWIF_POW_FORCED_IDLE_REQ` with
`RGXFWIF_POWER_FORCE_IDLE`, polls the same power-sync word, and only then
`PVRSRVSetDevicePowerStateKM` sends the power-off request
(`pvrsrv.c:3099`). Upstream Linux does the same in `pvr_power_fw_disable`
(`pvr_power.c:95–112`): forced idle, then a forced power-off, each
acknowledged through the power-sync word.

Every command is kicked by `__MTSScheduleWrite`
(`services/server/devices/rogue/rgxfwutils.c:215–234`): a write of the GP
data master, 0, to `RGX_CR_MTS_SCHEDULE` (`0xb00`), then a read of the same
register so that the write reaches the register bank.

### `RGXStop` for a MIPS core

`RGXStop` (`services/server/devices/rogue/rgxstartstop.c:992–1241`) runs
after the power-off acknowledgement:

1. `RGXDeviceAckIrq`: for a native MIPS driver `RGXAckIrqMIPS` reads the
   MIPS wrapper interrupt status (`0x8a8`) and, when its event bit is set,
   writes it to the clear register (`0x8b0`)
   (`services/server/devices/rogue/rgxinit.c:420–462, 477–500`).
2. Polls `SIDEKICK_IDLE` (`0x3c8`) for every unit but GARTEN, SOCIF and
   HOSTIF (mask `0x78`), then `SLC_IDLE` (`0x3898`) for `0xff`.
3. Clears the thread-0 interrupt and background DM associations (`0xb40`,
   `0xb30`); thread 1 only for a META core.
4. Polls `BIF_STATUS_MMU`, `BIFPM_STATUS_MMU`, `BIF_READS_EXT_STATUS`
   (unless `XT_TOP_INFRASTRUCTURE`), `BIFPM_READS_EXT_STATUS` and
   `SLC_STATUS1` for 0, `SLC_IDLE` again, `SIDEKICK_IDLE` again, and for a
   MIPS core GARTEN idle (bit 2 of `0x3c8`).
5. Asserts no soft reset. The platform OFF removes the power.

## The first command and its completion

The smallest command is the health check: a 88-byte kernel CCB command with
only its type set (`RGXFWHealthCheckCmdInt`,
`services/server/devices/rogue/rgxfwutils.c:4958–4978`; upstream
`pvr_power.c:175–180`). It carries no address and no payload, so the
firmware touches nothing but its own queues and data to execute it. The
vendor driver sends it as the first command when it connects to running
firmware (`rgxpower.c:715–723, 739–747`).

The completion comes back through the kernel CCB, not the firmware CCB: the
host clears the command's 32-bit return slot before the kick, and the
firmware sets `RGXFWIF_KCCB_RTN_SLOT_CMD_EXECUTED` (bit 0) in it after
executing the command (`include/rogue/rgx_fwif_km.h:1466–1470`; upstream
`pvr_kccb_wait_for_completion`, `pvr_ccb.c:404–413`). It also increments
`kccb_cmds_executed` in OS data. The firmware CCB carries requests that the
firmware starts itself, such as a free-list grow or a context reset; no
command issued at initialisation is answered there.

The command types are the open firmware's: the health check is 115
(`ROGUE_FWIF_KCCB_CMD_HEALTH_CHECK`, upstream `pvr_rogue_fwif.h:1188`),
where the vendor's own firmware numbers it 111
(`include/rogue/rgx_fwif_km.h:908`). The power command is 107 in both, with
power-off request 1 and forced-idle request 2. The open firmware's power
states are OFF 0, ON 1, FORCED_IDLE 2 and IDLE 3 (upstream
`pvr_rogue_fwif.h:105–110`).

## What SunstoneOS uses

SunstoneOS already carries the upstream-derived control and stop
components: `pvr_gpu_control.c` reads the firmware's compatibility record,
starts the queues, submits one health check and waits for its return slot;
`pvr_gpu_stop.c` sends the forced-idle and power-off requests, waits for
each acknowledgement, and runs the register sequence. Three changes bring
them to the vendor's flow:

| Item | Vendor DDK | Upstream Linux | A733 |
| --- | --- | --- | --- |
| Stop registers | `RGXStop`: interrupt acknowledgement, early `SLC_IDLE` poll, thread-0 associations, no reset | No interrupt acknowledgement, thread-0 and thread-1 associations, `SOFT_RESET` asserted | The vendor's (`pvr_gpu_stop.c`, 14 steps) |
| Kick | `MTS_SCHEDULE` ← 0, read back | Write only | Write, read back (the firmware-stop image's callback) |
| Power state after each acknowledgement | `POW_OFF` required before `RGXStop` | Not checked | FORCED_IDLE (2) after the idle request, OFF (0) after the power-off request, both required |
| A failed register stop after the power-off acknowledgement | Logged; the GPU is powered off | The error is returned | The image powers the GPU off and reports the failure |

The power-off request stays upstream's forced request: the vendor sends a
non-forced request after its forced idle, and with the firmware already in
forced idle both requests end in OFF. The vendor's other steps of
`RGXFinalisePowerOff` (the OS connection state, interrupt counters and the
timer-correlation end) belong to its virtualisation and statistics support,
which the open firmware's upstream driver does not use.

## Board steps

The control component submits the health check before it reports ready,
and the stop requires a ready control. A start that is followed by a stop
therefore always executes one health check first: steps (a) and (b) of the
plan are one image. Combining them adds no uncontrolled state. Every failure
before the firmware acknowledges the power-off request leaves the state that
the firmware-start run left, firmware running and GPU on, which ends the
lifetime with a power removal; after that acknowledgement the image powers
the GPU off as the vendor does, even when an idle check fails.

### Steps (a) and (b): start, one health check, stop, OFF

The firmware-stop image (`apps/gpu-power-id/fw-start.c` built with
`FWS_STOP`; build and check option `--fw-stop`; bundle row 20) runs on port
3-1 after a power removal, the cold DDR start and the provider-readiness
checkpoint. It is frozen in set `board-bundles-20261025-21` for lifetime
`board-session-20261025-18`:

1. The firmware start of the firmware-start image, with the vendor's
   500 ms bound on the started word.
2. The control, within 200 ms: the compatibility record at OS init +40
   (structure versions 3 and BVNC `36.56.104.183`), the queues, one health
   check (type 115) kicked through `MTS_SCHEDULE` with a read-back, and its
   return slot. Between polls it reads the MIPS wrapper interrupt status and
   clears a pending event.
3. The stop, within 300 ms: the forced-idle request, acknowledged with power
   state FORCED_IDLE; the forced power-off request, acknowledged with power
   state OFF; then the 14 register steps of the vendor `RGXStop`.
4. The platform OFF of the default path (CORE6 OFF, TOP5 OFF, clocks and
   PLL_GPU0 off), the supply end and the provider park, each proved as in
   the power-only image.

Every GPU-window access is a 32-bit access after its progress record. A
read that repeats the previous read of the same register is a poll; the
progress record names it, and the log prints its value only when it
changes, so that a poll until a deadline does not fill the 16 KiB log.

Predictions: `GPU FWS control result=0 phase=4 error=0 health=1
health_status=0x1` within a few milliseconds; `GPU FWS power ack_idle=0x1
ack_off=0x1 state_idle=0x2 state_off=0x0 status_idle=0x1 status_off=0x1`;
`GPU FWS after kccb_done=0x3 pow_state=0x0`; `GPU FWS stop result=0
phase=10 error=0 step=14 count=14`; then `COMPLETE GPU firmware stop:
started, health check executed, forced idle and power off acknowledged,
idle, OFF and provider parked`, status 0 and a responsive ROM.

Controlled failure states:

- an incompatible record (control error 4), no health completion within
  200 ms (error 3) or a firmware event (error 7): the firmware keeps
  running, and the lifetime ends with a power removal;
- a power request not acknowledged within the stop's 300 ms, an
  acknowledgement with another power state, or a platform OFF fault: the
  GPU stays powered, and a power removal follows;
- an idle check not met after the power-off acknowledgement: the image
  powers the GPU off, ends its supply sample and parks the provider, and
  reports the failure.

### Step (c): a second start in the same lifetime

After step (a)+(b) passes, the same image runs again in that lifetime,
without a power removal. Its start then begins from a stopped firmware and
an OFF GPU, so a second COMPLETE line shows that the stop leaves a state
from which the firmware starts again. This step is planned, not frozen.

## Design gate

**Cost.** None of these paths is hot. A power-off adds two commands, two
acknowledgement polls and 14 register steps; the health check adds one
command. The control and stop budgets (200 ms and 300 ms) and the vendor's
500 ms start bound fit, with the 100 ms report, inside the 1.5 s session
deadline, and keep the monitor's 500 ms margin.

**Checks.**

- `checks/pvr-gpu-stop.c` and `checks/pvr-gpu-stop-reference.py`: the
  14-step plan, the interrupt acknowledgement with and without a pending
  event, 118 retained faults, and the upstream comparison with each of the
  four vendor differences checked
  (`~/.cache/sunstoneos/evidence/gpu-stop-20261025/`).
- `apps/gpu-power-id/check.py --fw-stop`: eight cases on the linked image,
  with a model of the firmware's side of the kernel CCB, power-sync word and
  interrupt status. Covered: the complete run; an incompatible record; no
  health completion; a firmware event; a denied power-off; no power-off
  acknowledgement; a held idle register after the acknowledgement; a
  platform OFF fault. The start of every case is the firmware-start
  sequence access for access; after it, every access must be an interrupt
  read or clear, a kick with its read-back, or the next stop step.
- The GPU service host checks rerun at the stop change; three fixtures that
  expected the stop's reset now expect none
  (`~/.cache/sunstoneos/evidence/gpu-fw-stop-regress-20261025/`).

**Image comparison.** The firmware-start image's loadable bytes are
unchanged (`487ee41e…`, the image that passed on the board); its debug
information moves with the source lines.
