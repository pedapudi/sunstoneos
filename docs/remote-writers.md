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

Status: implemented and checked against modeled register states. No boot image
calls it yet, and no board has reported the register values it reads. With the
current sources it cannot return zero on a board: the OR100 processor and the
crypto engine (CE) remain unproven, as explained below.

## The cold boot chain starts only the SD controller, CE and possibly DMA0

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
  writes the NAND0, SMHC0, SMHC2 and UFS reset registers.
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
| OR100 | Never; no register controls it in either pinned document | 1, 12.1.2 |
| DMA0, DMA1 | Reset asserted (`0x704`, `0x70c` bit 16), or bus gate open, every `DMA_EN_REG` bit 0 clear and `DMA_STA_REG` bits 31 and 15:0 clear | 4.1.6.85, 4.1.6.86, 13.1.7.3, 13.1.7.6 |
| SMHC0 to SMHC3 | Reset asserted (`0xd0c`, `0xd1c`, `0xd2c`, `0xd3c` bit 16), or bus gate open and `SMHC_DMAC` bit 7 (IDMAC enable) clear | 4.1.6.129 to 4.1.6.135, 11.3.8.25, 11.3.9.26 |
| CE | Both resets asserted (`0xac4` bits 16 and 17) | 4.1.6.118 |
| NAND0 | Reset asserted (`0xc8c` bit 16) | 4.1.6.127 |
| UFS | Four resets asserted (`0xd8c` bits 16 to 19) | 4.1.6.138 |
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
a task type, and the value 0 also names channel 0. A CE out of reset therefore
stays unproven. The ISP shares the `VIDEO_IN` reset, which defaults to
de-asserted, so its rule uses its module clock and MBUS clock instead.

One sample is valid only while nothing can restart a stopped master. Apart
from the Arm cores, the E902 and OR100 are the only programmable agents in the
pinned sources that could do so. A caller therefore refuses on any set bit.

## OR100 and CE need further evidence

- **OR100.** The manual's block diagram (section 1) and interrupt controller
  diagram (section 12.1.2) show an OR100 processor in the CPUS domain. The
  datasheet lists only the E902 as a RISC-V or CPUS-domain processor, and
  neither document gives an OR100 clock, reset or memory map. Clearing this
  bit needs a vendor source that either removes OR100 from the A733 or
  documents its reset and bus gate.
- **CE.** The board ROM leaves both CE resets de-asserted on every boot that
  passes its clock setup. Neither the ROM nor boot0 contains a literal CE
  register address, which suggests that no CE task was loaded, but a literal
  scan cannot prove it. Clearing this bit needs either a documented CE idle
  status or a complete control-flow proof that no ROM or boot0 path writes the
  CE task registers.
- **Board values.** No board has reported the registers listed above after a
  cold boot. One read-only snapshot of them on a cold-booted board would show
  which masters the ROM and boot0 actually leave out of reset, including
  SMHC0's IDMAC state, DMA0 after the ROM's media probe and the NAND0, SMHC2
  and UFS resets.

## No source defines a safe stop procedure

The function stops nothing. The manual describes resets for the SD controller
DMA (`SMHC_CTRL` bit 2 and `SMHC_DMAC` bit 0) and a pause bit for each DMA
channel (`DMA_PAU_REG`), but it does not state that these drain outstanding
memory writes. The ROM's DMA0 stop routine at `0x1090` and its reset assertion
at `0xc91c` show what the vendor code does, not that the transactions complete.
Reversing the shipped monitor's E902 start writes is equally unproven. A stop
procedure therefore needs a source that defines completion of outstanding
writes before it can replace a refusal.

## The stage-2 loader should call the check before reading the card

The stage-2 loader in `boot/a733/stage2/` should call the function at EL3,
before it reads the first slot header, with a read function that performs one
aligned 32-bit load. It should store the returned mask in its report and
refuse entry in `a733_stage2_entry_admission()` while the mask is nonzero.
The loader takes over SMHC0 for its own polled reads after this call; the SD
core's own admission already refuses an enabled IDMAC.

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

## Checks

[checks/remote-writers-model.py](../checks/remote-writers-model.py) compiles
the function with the standalone-boot compiler flags and runs it in Unicorn.
All register values are modeled, and the E902 configuration block is unmapped.
Its 111 cases cover reset defaults, the shipped monitor's start writes together
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
