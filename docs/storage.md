# Storage identification remains incomplete

The FreeRTOS storage probe reaches the SD/MMC controller at `0x04022000`,
resets it, updates its clock, and completes CMD0. CMD1, which requests eMMC
operating conditions, receives no response. No storage device has been identified
or read. This result does not establish that eMMC is absent: population, power,
signal routing, and timing still need confirmation.

The board device tree enables SD, eMMC, and UFS controllers. An enabled node
does not establish physical population. UFS initialization has not run. SD card
detection has not been configured, so the recorded PF input value does not
establish whether a card is inserted.

## Bounded RAM probe

[checks/storage-probe.c](../checks/storage-probe.c) runs in one FreeRTOS task
under the existing two-second EL3 recovery deadline. It records the inherited
clock gates, storage clocks, pin configuration, voltage selection, and controller
version before attempting identification. Shared bus gates and regulator settings
remain inherited; the probe stops if the storage AHB gate is disabled.

The probe temporarily selects the controller's PC pins, enables its private
clock and reset, and uses the low-frequency oscillator with a divisor of 64.
It applies the reference timing-mode setting for controller revisions starting
at 5.3.0. Controller waits have deadlines; CMD1 readiness polling has a
750 ms limit. The command set consists of CMD0, CMD1, and CMD2. It contains no
media programming or persistent configuration commands.

Normal completion restores and verifies the saved pin functions, pulls, private
clock, and bus gate. The controller itself is left reset. A forced monitor
timeout returns to ROM without those task-level restoration guarantees.
The completion marker means the bounded probe finished; it does not mean
storage support passed.

Build and run from the bring-up worktree using the configured cache:

```sh
cmake -S . -B ~/.cache/sunstoneos/build/qemu-virt
cmake --build ~/.cache/sunstoneos/build/qemu-virt --target a733-storage-probe
/usr/lib/llvm-21/bin/llvm-objcopy -O binary \
  ~/.cache/sunstoneos/build/qemu-virt/a733-storage-probe.elf \
  ~/.cache/sunstoneos/build/qemu-virt/a733-storage-probe.bin
python3 tools/check-a733-kernel.py \
  --fel ~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel \
  --image ~/.cache/sunstoneos/build/qemu-virt/a733-storage-probe.bin \
  --expect 'PASS storage probe completed'
```

The loader requires initialized DDR, verified ROM identity, exclusive board
access, a healthy USB host, and the installed reconnect rule. It verifies the
uploaded RAM image before execution. [Build](build.md), [boot](boot.md), and
[USB access](../tools/README.md) describe those prerequisites.

## Physical evidence

On 2026-10-16, the final probe completed in approximately 6.86 ms according to
the declared 24 MHz architectural counter. The controller version was
`0x03050530`. Its initial timing register was `0x81710000`; the probe cleared
bit 31 as required by the inspected reference initialization. CMD1 then reported
response timeout `0x100`, with controller status `0x106`. Pin and clock
restoration checks passed. ROM returned with automatic USB access on port `3-1`.

| Artifact | SHA-256 |
| --- | --- |
| Probe ELF | `3b741e586d86f679ffdc10203cc849aa2a77180a55834068f3ca15e1f61b84bb` |
| Probe binary | `3ed51dfa3aaab85f4477c79ccd3c0846a6cd27412af3b509d6b5defc7c0e76bd` |

Raw evidence is under `~/.cache/sunstoneos/evidence/storage-timing-20261016/`.
The session record includes the loader hash, image readback, ROM identity,
reconnect identity, and retained result. Existing kernel and mimus evidence was
reused because their behavior and dependencies did not change.

An earlier probe, recorded under `storage-identify-20261016/`, also reached
CMD1 timeout and returned to ROM. Its host runner encountered `ENODEV` while
reading sysfs during the expected disconnect. All FEL requests had succeeded.
The retained result was retrieved after metadata confirmed ROM return, without
reloading or resetting the board. The host helper now treats `ENOENT` and
`ENODEV` during session discovery as an absent device; existing caller deadlines
still apply. Other metadata errors remain fatal. The focused host check is
`python3 checks/usb-disconnect.py`; it passed. The final physical session also
completed through the updated discovery function.

## Reference implementation and next work

The [pinned sources](sources.md) supply the register and wiring references:

- Board kernel: `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c`,
  `bsp/drivers/pinctrl/pinctrl-sun60iw2.c`, and the cached board device trees.
- Bootloader: `drivers/mmc/sunxi_mmc.c`, `drivers/mmc/sunxi_mmc.h`,
  `drivers/mmc/host/sunxi_mmc_host_tm4.c`, the type-1 clock implementation,
  and the A733 GPIO definitions.

The next useful work is to establish storage power and population, then initialize
UFS or the applicable SD/MMC path for read-only identification and block access.
UFS references are available in the pinned bootloader's `drivers/ufs/` directory.
Repeated CMD1 attempts need new evidence about wiring, power, or initialization.
Storage backup, native USB block export, persistent boot, writes, power-loss
behavior, and recovery qualification remain outstanding.
