# Remote bus masters must be excluded before a cold boot grants memory

A remote writer is any bus master other than the Arm cores that can write DRAM:
the E902 RISC-V core, DMA controllers, and peripherals with their own DMA engine.
Before the [stage-2 loader](cold-entry.md#the-stage-2-build-continues-into-the-slot-loader)
gives an application exclusive memory, it must show that none of these masters
can still write. Otherwise a master started earlier in the boot chain could
overwrite a verified image or the application's memory pools.

[`a733_remote_writers_excluded()`](../platform/a733/remote_writers.c) reads the
reset, clock and DMA-enable registers through a caller-supplied read function
and returns a bitmask of the masters whose exclusion it cannot prove. Zero means
every listed master is excluded. The function never writes a register. The
interface is in [`a733_remote_writers.h`](../include/a733_remote_writers.h).

Status: implemented and checked against modeled register states. The stage-2
loader checks the mask before card access; the memory qualification image
records it under its separate residual-risk contract. The generic provider leaves
CE unproven when the ROM has released its resets. The cold monitor additionally
accepts the fixed nonsecure SD predecessor contract described below. OR100 is not
part of the A733 device processor
inventory, so its historical mask bit 1 is reserved and never emitted.

## The cold boot chain releases CE reset and may leave DMA0 and storage controllers out of reset

The cold boot chain is the board ROM, then boot0, then the SunstoneOS cold-entry
monitor and its stage-2 loader. The TOC1 package contains only the monitor; it
omits U-Boot, the shipped ARM Trusted Firmware monitor and the SCP firmware.

- **E902.** The SCP firmware runs on the E902. In the published firmware the
  shipped monitor starts it; [the cold-entry record](cold-entry.md#remote-processor-exclusion-needs-the-effective-a733-register-map)
  lists its four start writes. The boot0 package parser copies an item named
  `scp` to DRAM at `0x48100000` but does not start a core. boot0's code contains
  no literal address of the E902 configuration block, the CPUIDLE block or the
  RISC-V clock and reset registers. Its messages about the "arisc" reset have
  no pointer anywhere in the image, so the code that printed them is absent.
- **Board ROM.** The captured board ROM does not reference E902 registers. Its
  clock setup at `0x2444` (called from `0xba8`) always continues into `0x21cc`,
  and every exit from that routine reaches `0x218c`. That sequence de-asserts
  both CE resets, opens the CE MBUS gate (bit 2 of `MBUS_GATE_EN_REG`) and opens
  both CE bus gates. The ROM's SPI and raw-NAND boot routines de-assert the DMA0
  reset at `0x10d0`, and a routine at `0xc91c` asserts it again. The ROM also
  writes the NAND0, SMHC0, SMHC2 and UFS reset registers. The literal scan does
  not show whether those writes leave the resets de-asserted.
- **boot0.** Among the masters below, boot0 references only the SMHC0 reset
  register. It reads the card through the SMHC0 internal DMA controller (IDMAC).
  Whether boot0 leaves the IDMAC disabled at hand-off is not established from
  its instructions.

These results come from literal-pool scans. An address that code computes
from an unrelated base is invisible to such a scan, so the survey narrows the
candidates without proving hardware state. For that reason the runtime check
reads every master's state instead of trusting the survey.

## Each master is excluded by documented reset, clock or DMA state

Register sections refer to the pinned A733 User Manual V1.00. Every CCU bus
gating reset (BGR) register is at `0x02002000` plus the listed offset; a reset
bit reads 0 when asserted and bit 0 is the bus clock gate.

| Master | Excluded when | Manual section |
| --- | --- | --- |
| E902 | `RISCV_BGR_REG` (`0x0701021c`) bits 0, 1 and 16 are 0; `RISCV_24M_CLK_REG` (`0x07010210`) bit 31 is 0; CPUIDLE word `0x07050008` bit 0 is 0 | 4.2.5.23, 4.2.5.24 |
| Reserved bit 1 (formerly OR100) | Not an implemented A733 processor; no register access | Manual page 3; datasheet figure 1-1, table 2-1, 3.1 |
| DMA0, DMA1 | Reset asserted (`0x704`, `0x70c` bit 16), or bus gate open, every `DMA_EN_REG` bit 0 clear and `DMA_STA_REG` bits 31 and 15:0 clear | 4.1.6.85, 4.1.6.86, 13.1.7.3, 13.1.7.6 |
| SMHC0 to SMHC3 | Reset asserted (`0xd0c`, `0xd1c`, `0xd2c`, `0xd3c` bit 16), or bus gate open and `SMHC_DMAC` bit 7 (IDMAC enable) clear | 4.1.6.129 to 4.1.6.135, 11.3.8.25, 11.3.9.26 |
| CE | Both resets asserted (`0xac4` bits 16 and 17) | 4.1.6.118 |
| NAND0 | Reset asserted (`0xc8c` bit 16) | 4.1.6.127 |
| UFS | Four resets asserted (`0xd8c` bits 16 to 19) | 4.1.6.138 |
| SPIF (SPI flash controller, which has its own descriptor DMA engine) | Reset asserted (`0xf1c` bit 16) | 4.1.6.167, 11.5.5.7 |
| USB0, USB1, USB2 | All resets asserted (`0x1304` bits 16, 20, 24; `0x130c` bits 16, 20; `0x135c` bit 16) | 4.1.6.195, 4.1.6.197, 4.1.6.202 |
| PCIE0, GMAC0 | Both resets asserted (`0x138c`, `0x141c` bits 16 and 17) | 4.1.6.207, 4.1.6.212 |
| DE0, DI, G2D, EINK | Reset asserted (`0xa04`, `0xa24`, `0xa44`, `0xa6c` bit 16) | 4.1.6.105 to 4.1.6.112 |
| VE | Encoder and decoder resets asserted (`0xa8c` bits 16 and 18) | 4.1.6.116 |
| NPU | Core, AXI and AHB resets asserted (`0xb04` bits 16 to 18) | 4.1.6.120 |
| GPU0 | Reset asserted (`0xb24` bit 16) | 4.1.6.122 |
| CSI | Reset asserted (`0x1844` bit 16) | 4.1.6.245 |
| ISP | Module clock off (`0x1860` bit 31) and ISP MBUS clock masked (`0x5e4` bit 9) | 4.1.6.246, 4.1.6.84 |

Every entry above is the manual's reset default, except that the DMA and SD
controller rules also accept an engine that is out of reset but disabled.

The E902 rule relies on the bus clock gate. The manual documents no E902 core
reset; `RISCV_BGR_REG` bit 0 gates the bus clock of the RISC-V subsystem, which
reaches DRAM through the standby AHB matrix. The E902 maps DRAM `0x40034000` to
`0x7fffffff` in its own address space, so an unexcluded E902 can write the
application area at `0x41000000`. The CPUIDLE bit is undocumented; the shipped
monitor sets it as part of its start sequence, so a set bit counts as a sign
that the core was started. The function never reads the E902 configuration
block at `0x07032000`, because a read of a gated block may not complete.

Device registers are read only when the CCU shows the master out of reset with
its bus gate open. A master that is out of reset with its gate closed stays
unproven, because its state cannot be read.

The CE has no documented idle indication. `CE_TSR` reports which channel runs
a task type, and the value 0 also names channel 0. The generic provider therefore
leaves a CE out of reset unproven; the cold-monitor exception below relies on
its predecessor contract. The ISP shares the `VIDEO_IN` reset, which defaults to
de-asserted, so its rule uses its module clock and MBUS clock instead.

One sample is valid only while nothing can restart a stopped master. Apart
from the Arm cores, the E902 is the programmable control processor in the
A733 device inventory that could do so. A caller therefore refuses on any set bit.

## Device applicability excludes OR100; CE requires predecessor accounting

The manual's broad block diagram on printed page 2 shows both E902 and OR100.
Page 3 explicitly says that some depicted modules are not offered on every
device and directs the reader to the A733 datasheet. That datasheet's device
diagram (figure 1-1), ordering table covering all three A733 variants (table 2-1)
and processor list (section 3.1) specify six A55 cores, two A76 cores and one
E902, with no OR100. The device inventory therefore takes precedence over the
manual's broad diagram.

The manual's interrupt diagram also names OR100, and the generated sun60iw2
U-Boot map at revision `b791be842935b27268ae3d00e943a9075495f30a` names
`SUNXI_CPUS_OR100_CFG_BASE` at `0x07031000`. These family-level definitions do
not establish a second implemented A733 processor. The manual's actual memory
map omits that address and documents E902 configuration at `0x07032000`.
No probe or guessed control write to `0x07031000` is justified. This conclusion
uses device applicability; it does not identify OR100 as an alias for E902.
The native enum keeps bit 1 reserved so historical evidence masks retain their
meaning. The applicable-writer mask and all new samples exclude that bit.

The actual package contains only the native monitor and omits the vendor
monitor, U-Boot and SCP. The vendor ARISC startup service reaches the documented
E902 start sequence; this package does not execute that service. The E902
runtime checks remain necessary and unchanged.

CE remains a real ownership requirement. The ROM releases both CE resets.
At captured-ROM address `0xb88`, the helper at `0x2a0c` reads SID word
`0x030060a0`; a nonzero result reaches uncaptured secure ROM at `0x11000`
through `0xb98`. Absence of CE literals in the captured normal ROM and boot0
cannot establish what that predecessor submitted. A runtime SID check could
exclude the secure branch, but would not alone establish no prior submissions
on the remaining path or reset coverage.

The effective A733 Linux CE v5 implementation at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` has no supported stop operation.
Its removal helper checks only the low byte of task status; the manual does
not define that as a complete both-world idle indication. CE task-load zero
has no effect. Completion bits must be tied to known submissions, including
chained tasks, before they can transfer memory ownership.

A valid initializer must exclude competing control writers, account for every
predecessor task or establish no submissions since a reset with known coverage,
and finish that proof before reusing protected memory. Unknown or active work
must refuse admission without an unproven reset, gate change or pool reuse.
The monitor is already in DRAM when stage2 runs, so a late check cannot protect
its earlier load from arbitrary inherited DMA. That interval needs a boot0 or
predecessor contract as well as the check before slot copying.

The cached follow-up at
`~/.cache/sunstoneos/evidence/cold-remote-followup-1026/` preserves source hashes,
numbered excerpts and exact ROM instructions, including the contrary generated
map. No new hardware observation or CE quiesce sequence is claimed.

## Ordinary nonsecure SD boot has a caller-owned entry contract

A fixed cold firmware entry does not need a hardware-attested reset-cause latch.
Its caller must establish a fresh CE and fabric lifetime before ROM or boot0
reuses memory. Power-on and a qualified whole-system reset can supply that
condition. Arbitrary CPU-only resets, warm jumps and preserved FEL sessions do
not satisfy it. The documented watchdog whole-system reset remains a candidate
route awaiting physical qualification; this contract adds no reset automation.

The captured ROM first attempts SD0 through `0x2f4` and `0xafe4` with arguments
`r0=0, r1=5`. The supported route requires normal CPU0 entry without redirect,
SID security word zero, storage BOOT selection and a successful first SD0
attempt. It loads the pinned eGON image into SRAM at `0x47000`; checksum routine
`0x3244` uses CPU word sums. Secure-ROM dispatch and alternate media/package
paths remain outside this contract.

boot0's local AArch64 handoff at `0x48ee0` preserves that established lifetime;
it does not reset the peripherals into a new one. The loader's failure return
publishes RVBAR `0x20` for forced FEL. That recovery exit is not a cold SD entry.
Media history in boot0 byte `+0x28` and RTC `0x07090120` can detect mismatches,
but cannot attest history. Helper `0x49a5c` polls UART commands, not reset cause.
The required CE boundary precedes `init_DRAM`, translation-table writes and the
monitor copy. A late stage2 reset or poll cannot protect those earlier accesses.

The source audit closes selected control targets, including Thumb IT conditions,
fixed libc/cache/SD callbacks and bounded jump tables. Its store inventory and
pointer-family analysis are separate from that control-flow result. The DRAM
pattern-test address arithmetic excludes CE, and the training-sequencer
constructor resets its cursor before emitting at most 83 entries. The reviewed
source-origin argument confines the remaining stack, SRAM, heap, descriptor,
monitor-copy and peripheral destinations. It establishes no CE submission for
this trusted fixed-firmware route, conditional on the caller-owned initial
quiescence. This is source assurance, not formal verification or physical evidence.

Only the cold monitor applies this predecessor exception. Before slot copying,
it may clear the residual CE bit when SID `0x030060a0` is zero, BOOT input
`0x03000024` bit 8 is high, the SRAM header at `0x47004` contains `eGON.BT0`,
and the low nibble of boot0 media byte `0x47028` is zero. These observations
reject profile mismatches; a warm caller that reproduces them still violates
the entry contract. All other writer bits remain required. The generic provider
continues to refuse a CE with released resets for unknown predecessors. Neither
path writes CE registers or treats task-load zero as completion.

`tools/prepare-microsd-boot.py` pins the complete stock boot0 identity and
validates the sole-monitor TOC1 package. Its optional restore path accepts only
the existing validated derivative, including allowed patches and helper identity.
The package and trusted medium supply the image identity; the SRAM magic is
not a replacement for those host checks or a secure-boot claim.

The preserved vendor DRAM record is another input to the boot path. Before
`init_DRAM`, `0x49ab4` reads 2 KiB at LBA `0x5fb0`, byte offset `0xbf6000`.
The current installer preserves this range. An accepted record can replace the
selected header's 32 parameter words after magic, checksum, boot0 checksum and
SID checks. A boot0 hash alone therefore does not identify the live parameters.

The optional restore hook already checks those live parameters at `0x831f8`.
Board setup selects the header timing set before the vendor record is processed;
restore admission runs afterward. Four focused instruction cases cover blank,
stock-checksum and derivative-checksum records. A valid differing derivative
record returns `STRAP=6` before edits, allowing full-training fallback. No sector
correction is needed. The cases model storage transport and the selector decision;
they do not qualify physical DRAM operation.

The audit and focused evidence are preserved at
`~/.cache/sunstoneos/evidence/cold-ce-lineage-1026/`. Source applicability and
a supported entry ABI do not establish that
a current board has followed that route. Physical cold boot, reset coverage and
memory operation remain unqualified.

## No source defines a safe stop procedure

The function stops nothing. The manual describes resets for the SD controller
DMA (`SMHC_CTRL` bit 2 and `SMHC_DMAC` bit 0) and a pause bit for each DMA
channel (`DMA_PAU_REG`), but it does not state that these drain outstanding
memory writes. The ROM's DMA0 stop routine at `0x1090` and its reset assertion
at `0xc91c` show what the vendor code does, not that the transactions complete.
Reversing the shipped monitor's E902 start writes is equally unproven. A stop
procedure therefore needs a source that defines completion of outstanding
writes before it can replace a refusal.

## The stage-2 loader checks before reading the card

The stage-2 loader calls the function at EL3 before card access, using one
aligned 32-bit load per callback. It records a nonzero mask and refuses before
slot copies. After admission, the existing SD provider establishes its own
controller ownership and completion contract before enabling IDMAC.

```c
static int mmio_read(void *context, uint64_t address, uint32_t *value)
{
    (void)context;
    *value = *(volatile const uint32_t *)(uintptr_t)address;
    return 1;
}
const struct a733_remote_writers_ops ops = {mmio_read, 0};
uint32_t unproven = a733_remote_writers_excluded(&ops);
```

## Memory qualification retains its separate residual-risk contract

The memory qualification monitor samples the mask once at EL3, before EL1
entry, and stores it in the result record. Its historical exception mask accepts
bits 1, 4 and 12: the former OR100 bit, CE and USB0. Bit 1 remains numerically
reserved, and new samples leave it clear. Historical records are unchanged.

CE remains unproven when its resets are released. USB0 is out of reset because
the ROM's FEL stack uploaded the image; the captured stack uses programmed I/O
and does not execute concurrently with the image. These facts do not give a
cold loader general predecessor ownership. A CE or USB0 write during memory
qualification could appear as a DRAM fault, so preparation still requires the
existing explicit acceptance of that residual
([memory-qualification.md](memory-qualification.md#host-side)).

## Checks

[checks/remote-writers-model.py](../checks/remote-writers-model.py) compiles
the function with the standalone-boot compiler flags and runs it in Unicorn.
All register values are modeled, and the E902 configuration block is unmapped.
Its 113 cases cover reset defaults, the shipped monitor's start writes together
and one at a time, the SD controller state that boot0 may leave, each DMA
channel and status bit, every listed reset bit, read failures and missing
callbacks. Each case also checks that the function writes no register and reads
no unexpected address.

[checks/remote-writers-sources.py](../checks/remote-writers-sources.py) pins
boot0, the captured board ROM, the manual text and the datasheet text by
SHA-256. It repeats the literal-pool survey, decodes the ROM's CE and DMA0
sequences, and confirms the manual and datasheet statements cited above.

```sh
~/.cache/sunstoneos/venv/bin/python checks/remote-writers-model.py --output /new/directory/model
~/.cache/sunstoneos/venv/bin/python checks/remote-writers-sources.py --output /new/directory/sources
```
