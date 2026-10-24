# USB0 takeover from the boot ROM's device session

Status: **implemented and qualified on one board**. The admission rule and
the takeover sequence below run in the USB platform provider
(`drivers/usb/a733_usb_platform.c`); its one native write path for USB0
registers is `a733_usb_platform_native_write`
(`drivers/usb/a733_usb_platform_native.c`). On 2026-10-24 the bundle
`board-bundles-20261023/rank-9-usb-v3/` passed on port 3-3
(`board-session-20261023/port-3-3/r9-v3-usb/`): its PCTL byte store cleared
SoftConn, the OUT/IN exchange completed with received 1 and sent 40, the
USB task quiesced and the monitor returned to a responsive ROM, with
`native_usb.functional_success` true. That image issued the same `strb` to
the same register as the remaining path. The later changes below (the
detach interval timed from the first clear SoftConn read, and the admission
mask without LSDEV and FSDEV) are host-modeled; `rank-9-usb-v4/` carries
them on the current platform and has not run on a board
([rank 9 runbook](../board-runbooks/rank-9-usb.md)).

When the native USB returning image starts, the boot ROM's recovery-mode
(FEL) driver still owns USB0: the controller is enumerated by the host, in
device role, with its D+ pull-up enabled. The provider's ROM device mode
(`A733_USB_ROM_DEVICE`) admits that state, disconnects from the host,
resets the device controller and then runs the same PHY preparation as a
cold start. This document gives the admission rule for the inherited
state, the sequence and its sources, and what remains unproven.

## The refusal on 2026-10-23

The rank 9 bundle `board-bundles-20261023/rank-9-usb-v2/` ran on port 3-3
and returned to the ROM with monitor status 1. Its log
(`board-session-20261023/port-3-3/r9-v2-usb/kernel.log`) records:

| Field | Value | Meaning |
| --- | --- | --- |
| `USB_STEP`, `PMIC_STAGE` | 5, 4 | All four supply snapshots matched; the PMIC provider is ready |
| `USB_FAULT_KIND` | 4 | A register value failed its mask and expected value |
| `USB_FAULT_STAGE` | `0x16` | `ROM_DISCONNECT`, value 22 of the provider's stage enumeration |
| `USB_FAULT_ADDRESS`, `MASK`, `EXPECTED` | `0x04100040`, `0x40`, 0 | SoftConn must read clear |
| `USB_FAULT_OBSERVED` | `0x00029940` | The word read back after the write |
| `USB_FAULT_WRITES_ATTEMPTED`, `COMPLETED` | 3, 3 | Every write callback returned success |
| `USB_IO_DENIAL`, `USB_PROVIDER_DENIAL` | 0, 0 | No native guard or provider-health refusal |

In that image, `ROM_DISCONNECT` called `update(p, USB+0x40, BIT(6), 0, 0)`:
read the word, write it back with bit 6 clear, and read it again. The
readback still had bit 6 set, so the provider recorded the value failure
and retained ownership. The three writes are the disconnect write and,
by inference from the count, the two `ROM_MASK` writes that cleared the
endpoint interrupt enables at `0x04100048` and the bus interrupt enables
at `0x04100050`, the two of its four registers that the matched ROM sets
(see below).

The 2026-10-18 refusal on the second board was a different check. It
failed in `ROM_CHECK` (stage `0x14`) under mask `0x700` with no write,
because the guard then required DEVCTL SESSION (word bit 8) clear. The
correction that permits SESSION changed the `ROM_CHECK` mask to `0x8600`
with expected value `0x8000`. The rank 9 image contained that correction:
its provider source in `compiled-inputs/28/a733_usb_platform.c` is
byte-identical to the tree, and `rom_pio` in its `build/probe.elf` tests
the word with `mov w9,#0x8600` and `cmp w9,#0x8,lsl #12` at
`0x410057ac`. The 2026-10-23 run passed that check and `ROM_MASK`, and
then failed at the next stage. The correction was exercised; it does not
cover the SoftConn write, because that is a separate defect.

## The packed word at USB0+0x40

USB0 is at `0x04100000` (A733 user manual V1.00, memory map, "USB0
0x04100000---0x041FFFFF"). Manual section 18.5 lists "Soft
connect/disconnect", an internal DMA controller and an interface to the
external DMA controller, but it has no register table. The register
layout therefore comes from two pinned sources: TinyUSB's
`src/portable/sunxi/musb_def.h` (revision `d668b0ab`) and the Orange Pi
Allwinner BSP header `bsp/drivers/usb/sunxi_usb/include/sunxi_usb_bsp.h`
(revision `2ac08e8c`, SHA-256 `829b22d7…`). Both place four byte
registers in the word:

| Byte | Offset | Register | TinyUSB | BSP |
| --- | --- | --- | --- | --- |
| 0 | `0x40` | PCTL (MUSB POWER) | `USBC_REG_o_PCTL`, line 96 | line 57 |
| 1 | `0x41` | DEVCTL | `USBC_REG_o_DEVCTL`, line 131 | line 92 |
| 2 | `0x42` | EPIND (endpoint index) | `USBC_REG_o_EPIND`, line 104 | line 65 |
| 3 | `0x43` | VEND0 | `USBC_REG_o_VEND0`, line 138 | line 99 |

The bit names are TinyUSB lines 264 to 271, 450 to 457 and 468 to 469, and
BSP lines 405 to 412, 606 to 613 and 624 to 625. Bit 6 of the word is PCTL
bit 6, `USBC_BP_POWER_D_SOFT_CONNECT`: while it is set, the controller
enables the D+ pull-up and the host sees a connected device.

The observed word `0x00029940` decodes as:

- PCTL `0x40`: SoftConn set. High-speed enable, high-speed mode, reset,
  resume and suspend are clear.
- DEVCTL `0x99`: SESSION (bit 0), VBUS level 3 (bits 3 and 4, VBUS valid)
  and BDEVICE (bit 7). HOSTREQ, HOSTMODE, LSDEV and FSDEV are clear.
- EPIND `0x02`: the ROM last selected endpoint 2.
- VEND0 `0x00`: PIO bus selection, no DMA request selected.

This is the state the matched nonsecure ROM (SHA-256 `95be69c9…`,
disassembly `sources/brom/ns-brom.dis`) leaves. Its connection routine sets
SoftConn with `ldrb`/`orr #64`/`strb` at offsets `0x1db8` to `0x1dc4`,
selects endpoint 2 with `strb` at `0x1d50`, writes INTUSBE `7` with `strb`
at `0x1d68`, sets INTTxE bit 0 with `ldrb`/`strb` at `0x1d70` to `0x1d78`,
and clears VEND0 bit 0 with `strb` at `0x2458`. Its device test at
`0x23bc` to `0x23e8` requires BDEVICE and VBUS level 3.

## Why the disconnect write did not take effect

Established by the record and the image:

- The write was one 32-bit `str` of `0x00029900` to `0x04100040`
  (`a733_usb_platform_native_write`, `str w2,[x1]` at `0x4100686c`),
  followed by `dsb sy`, and it returned without a bus fault.
- The 32-bit readback, also after `dsb sy`, returned `0x00029940`: no byte
  of the word changed.
- The two other completed writes were the `ROM_MASK` stores to
  `0x04100048` and `0x04100050`, and their readbacks passed. This is an
  inference from the write count: of the four `ROM_MASK` registers, only
  these two are left nonzero by the matched ROM. So a 32-bit store did
  take effect at those two addresses.

Every pinned source writes PCTL with an 8-bit store:

- TinyUSB `dcd_disconnect` (`dcd_sunxi_musb.c`, lines 879 to 883) uses
  `USBC_REG_clear_bit_b`, an 8-bit read-modify-write (`musb_def.h`, lines
  18 and 63). `dcd_init` calls it first (line 841); with the A733 patch
  (`patches/tinyusb-a733-dcd.patch`) it then calls `a733_usb_prepare`,
  whose 150 ms detach and reset sequence ran on the board in earlier TinyUSB
  images.
- BSP `USBC_Dev_ConectSwitch` (`usbc/usbc_dev.c`, lines 681 to 694) uses
  `USBC_REG_set_bit_b` and `USBC_REG_clear_bit_b`, which are `readb` and
  `writeb` (`sunxi_usb_bsp.h`, lines 23 to 47). The BSP's one 32-bit write
  to PCTL is the test-packet path in `udc/sunxi_udc.c`, lines 144 to 146,
  which sets bit 0.
- The matched ROM writes PCTL, EPIND, VEND0 and INTUSBE only with `strb`.

No source states how the A733 handles a 32-bit store that spans PCTL,
DEVCTL, EPIND and VEND0. One candidate is that such a store does not update
PCTL, although a 32-bit store did take effect at INTUSBE (`0x50`), also a
byte register, in the same run. A second candidate is that the bit reads
back clear only some time after the write; no source states such a delay.
A third, an agent that sets SoftConn again, has no candidate: the ROM was
not running, the USB interrupts were disabled at the GIC and TinyUSB had
not started. The 2026-10-24 run shows that the byte store clears the bit;
it does not separate the first two candidates.

## Admission rule for the inherited state

`ROM_CHECK` reads the word once before any write and applies this rule.
Each later takeover stage that writes (`ROM_MASK`, `ROM_DISCONNECT`,
`ROM_RESET_ASSERT`) applies it again first.

| Bits of the word | Field | Rule | Reason |
| --- | --- | --- | --- |
| 0 to 5, 7 | PCTL except SoftConn | inherited | The device reset in `ROM_RESET_ASSERT` returns them to reset values; none selects host role or DMA |
| 6 | SoftConn | inherited, then removed | The ROM sets it to stay enumerated; the takeover clears it and requires it clear before the reset |
| 8 | SESSION | inherited | A B-device session; it selects neither host role nor DMA (2026-10-18 correction) |
| 9 | HOSTREQ | refused | Host-role request |
| 10 | HOSTMODE | refused | Host role |
| 11, 12 | VBUS level | inherited | Status from the PHY |
| 13, 14 | LSDEV, FSDEV | inherited | Named for host-side speed detection; no source shows a hazard in device role, and HOSTMODE is refused |
| 15 | BDEVICE | required | Device role; the matched ROM tests the same bit |
| 16 to 23 | EPIND | inherited | Selects which endpoint's registers appear at `0x80`; reset clears it |
| 24 | VEND0 bus selection | inherited | `USBC_SelectBus` (`usbc/usbc.c`, lines 543 to 577) sets it for PIO as well |
| 25 to 31 | VEND0 DMA request selection | refused | `USBC_SelectBus` sets these bits only for DMA transfers |

The provider tests the refused and required bits with mask `0xfe008600`
and expected value `0x8000`. The VEND0 test does not detect every DMA
selection: `USBC_SelectBus` encodes TX endpoint 1 as DRQ_SEL 0, which
leaves only bus selection set, the same value as PIO. The DMA start bits
and the PIO-only history below remain the DMA guard:

- all sixteen internal DMA channel start bits (bit 31 of
  `0x04100540 + 16n`, BSP `USBC_REG_o_DMA_CHAN_CFN`, line 160) must be
  clear at every takeover stage and at final verification;
- the DMA interrupt enables at `0x04100500` (BSP `USBC_REG_o_DMA_INTE`,
  line 158) are cleared and verified;
- the coordinator's admission still requires a PIO-only USB history in the
  powered lifetime and external DMA0 held in reset with its gates off,
  because clear start bits do not prove that earlier DMA completed.

## Takeover sequence

Each stage holds the common register mutex, performs at most the writes
listed, and checks the deadline before and after each callback. A failed
check records the first failure and retains ownership with no further
MMIO.

| Stage | Registers | Source |
| --- | --- | --- |
| `BASELINE` | Clock, fabric and PERI0 predicates; CCU `0x1300` PHY reset deasserted, `0x1304` device gate and reset on with host gates and resets off, `0x1340` 24 MHz reference | [Cold USB0 device preparation](../usb-platform.md); matched ROM initialization at `0xa20` |
| `ROM_CHECK` | Admission rule above; sixteen DMA start bits | This document |
| `ROM_MASK` | Clear INTTxE/INTRxE `0x48`, INTUSBE `0x50`, DMA interrupt enables `0x500`, ISCR enables `0x400` bits 0 to 3, writing 0 to its write-one-to-clear bits 4 to 6 | TinyUSB `musb_def.h` lines 99 to 102 and 166; BSP lines 60, 63, 125 and 158 |
| `ROM_DISCONNECT` | One 8-bit store of PCTL with SoftConn clear, if it is set; then one read of the word | TinyUSB `dcd_disconnect`; BSP `USBC_Dev_ConectSwitch` |
| `ROM_WAIT_SOFTCONN` | Only while SoftConn still reads set: one read of the word per poll, no write, bounded by the provider deadline | This document; no source states a delay |
| `ROM_WAIT_DISCONNECT` | No MMIO for 150 ms from the first read with SoftConn clear | The interval of `drivers/usb/a733_usb.c` `a733_usb_prepare`; **no source proves that 150 ms suffices for host disconnect detection** |
| `ROM_RESET_ASSERT` | Require SoftConn, HOSTREQ, HOSTMODE and every interrupt and DMA enable clear; then clear CCU `0x1304` bit 24, keeping the bus gate | Device reset from `a733_usb_prepare` |
| `ROM_WAIT_RESET` | No MMIO for 1 ms | `a733_usb_prepare` |
| `ROM_RESET_RELEASE` | Set CCU `0x1304` bit 24 | `a733_usb_prepare` |
| `ROM_WAIT_RELEASE` | No MMIO for 40 µs | Pinned platform initialization ([usb-platform.md](../usb-platform.md)) |
| `ROM_CORE_CHECK`, then `PHY_CONTROL` to `VERIFY` | The cold sequence | [Cold USB0 device preparation](../usb-platform.md) |

The provider issues the disconnect as one write callback for the word
`old & ~0x40`. `a733_usb_platform_native_write` accepts only that word: it
reads the current word, refuses any other change, and stores the low byte,
with `dsb sy` before and after. Reads of the word stay 32-bit; the
2026-10-23 readbacks decode consistently with the ROM's writes. Every
consumer reaches the store through that function: gpu-display, hdmi-audio
and npu-convolution call it directly, and usb-returning and the other
`a733_native_usb` images call it through `platform/a733/native_usb.c`.

`ROM_DISCONNECT` reads the word right after the store. When SoftConn reads
clear, the 150 ms detach interval starts. When it still reads set, the
provider enters `ROM_WAIT_SOFTCONN` and re-reads on later polls without
writing; the interval starts at the first clear read. A bit that never
clears exhausts the provider deadline and retains ownership with a time
failure (kind 1) at stage `0x1d`, whose observed value is the last word
read; no reset is written. `ROM_RESET_ASSERT` checks SoftConn clear again
before the reset write.

The returning application's shutdown in `apps/usb-returning/main.c`
removes SoftConn through the same function, then checks it after its
150 ms detach interval.

The takeover issues at most seven writes before the cold sequence: four in
`ROM_MASK`, one PCTL byte and two CCU writes for the reset. No stage loops
on hardware; waits are absolute counter deadlines that the polling caller
observes.

## Performance and simplicity

The provider runs one stage per poll. The USB task in
`apps/usb-returning/main.c` calls `a733_native_usb_step` and waits
`vTaskDelay(1)`, one tick of 1 ms (`configTICK_RATE_HZ` 1000), after each
poll that does not finish. The takeover baseline is about 166 polls, about
166 ms, from `BASELINE` to `PREPARED`: 16 polls run non-wait stages, and
the rest are tick polls during the 150 ms detach, 1 ms reset and 40 µs
release intervals. The linked check counts the polls of its modeled
takeover at the same tick and records them in `check-result.json`.

These tick waits meet the condition of
[section 1.3](../performance-architecture.md#13-one-step-per-1-ms-tick):
the USB0 interrupt is not qualified, and the 150 ms and 1 ms waits are at
least one tick each. The waits during those intervals stay. The 16
non-wait stages cost about 16 ms of tick latency; running consecutive
non-wait stages back to back in one poll while they report progress would
remove it. That change is not made here.

Timing the detach interval from the first clear read adds at most one read
per poll while SoftConn still reads set, and removes the time between the
store and that read from the takeover when the bit clears late.

## Unproven

- How the A733 handles a 32-bit store across the four byte registers; the
  2026-10-24 run shows only that the byte store clears SoftConn.
- That 150 ms is long enough for every host to observe the disconnect. One
  Linux host on port 3-3 did.
- That LSDEV and FSDEV never matter in device role.
- Completion of DMA issued earlier in the powered lifetime; the PIO-only
  history remains a coordinator prerequisite.
- The detach timing from the first clear read and the 12 GiB EL1 map
  platform of `rank-9-usb-v4/` on a board.

## Checks

All checks are offline and model register behavior:

- `checks/a733-usb-platform.py` builds and runs the host-compiled provider
  checks. `checks/a733-usb-rom-handoff.c` takes over from the observed
  word with the ROM's interrupt enables. It checks SoftConn reading clear
  30 ms after its write, with the 150 ms interval starting at that read,
  and SoftConn never clearing, which retains at the deadline before any
  reset write. It also checks six inherited variants, including LSDEV and
  FSDEV, and ten refused variants, each refused before any write.
- `apps/usb-returning/check.py` runs the linked provider and native
  callbacks of the returning image from the 2026-10-23 input, clock capture
  and observed word. Its register model leaves PCTL unchanged on a word
  store and updates it on a byte store. On the image refused on 2026-10-23
  the model reproduces the board's fault record field for field; on the
  built image the takeover reaches `PREPARED` with one byte store. It also
  checks the never-clearing SoftConn and five admission refusals.
- `apps/usb-returning/host-check.py` requires that the 2026-10-23 session
  record never classifies as functional success.
- `checks/usb-first-fault.py` runs the native callback, BUSY and
  first-fault cases on the built image.
