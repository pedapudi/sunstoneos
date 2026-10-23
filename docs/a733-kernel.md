# Physical FreeRTOS startup and bounded recovery

The A733 kernel-check image runs FreeRTOS on the boot Cortex-A55. An EL3 monitor
configures nonsecure EL1, the GICv3 interrupt controller, and architectural
timers. FreeRTOS executes the same queue, preemption, and floating-point context
check used by the emulator foundation. The image then returns to the ROM's
FEL recovery interface, where the host retrieves its DRAM log.

This is a physical kernel milestone. Persistent boot, native USB diagnostics,
other CPU cores, storage services, mimus, and complete hardware support remain
outstanding. The check does not leave an interactive OS running.

The runner accepts genuine DDR evidence followed by an explicit sequence of
successful kernel ROM returns. Repeated `--warm-return` arguments bind each
record's initial instance to the previous returned instance, then require the
final instance to match the current survey and live board. The runner preserves
and hashes those records. A changed USB node alone cannot establish continuity.
This path passes physically for the second board's returning GPU supply probe
under `gpu-supply-probe-20261018/runtime/`; no DDR retraining was needed.

## Execution and recovery

The image starts at `0x41000000`. The monitor invalidates data and instruction
caches before entering C, using the cache geometry reported by the CPU. It keeps
two GIC security states, disables unrelated interrupt delivery during the check,
and assigns the physical nonsecure timer to Group 1, interrupt 30.

The secure physical timer uses Group 0, interrupt 29, routed to EL3 as an FIQ.
It expires after 48 million counter ticks. With the board's declared 24 MHz
counter frequency, this is two seconds. EL1 cannot mask this EL3 interrupt by
masking its own exceptions. This recovery mechanism depends on the CPU, counter,
and interrupt controller; it does not replace an independent hardware watchdog.

The monitor saves the distributor control, enabled SPI masks, and modified
redistributor settings. On completion, timeout, or a captured exception, it
disables the timers and restores those settings before the forced-FEL reset.
Register-write waits and cleanup waits have counter-based deadlines.

A returning physical monitor requires both GICD_CTLR affinity-routing bits,
ARE_S and ARE_NS, already set. An entry missing either bit returns status 4
before any GIC write. The observed A733 ROM value `0x37` satisfies this condition.
The [GIC specification](https://documentation-service.arm.com/static/601412d54ccc190e5e681269),
section 12.9.4, makes an enabled-to-disabled ARE transition unpredictable. Cleanup
therefore preserves every ARE bit enabled at cleanup entry. For admitted physical
runs, this restores the saved CTLR without clearing affinity routing.

The secure QEMU model permits cold startup with ARE clear. It enables affinity
routing and retains those bits during cleanup, then exits through semihosting.
That exit does not restore the emulator's cold GIC state or establish ROM-reset
safety. An initialization failure before ARE enable leaves the bits clear.

EL1 identity-maps peripheral addresses and the first 128 MiB of DRAM. The result block
at `0x42000000` uses normal non-cacheable memory, so the cache-disabled monitor
and ROM can read the log. Other mapped RAM uses normal cacheable attributes.
The table walker reads the two table pages through the data cache, but
`platform_init()` writes them with the cache off. `platform_init()` therefore
cleans and invalidates both pages with `a733_cpu_coherence_flush()` before the
first table write and again after the last, before it sets `TTBR0_EL1`. The
[memory qualification image](memory-qualification.md) also maps the rest of
DRAM, as non-executable write-back blocks.
The monitor cleans and invalidates data caches before reset. The current mapping
still permits writes and execution in the same RAM regions; task isolation and
separate code/data permissions remain unfinished.

With secure interrupt routing enabled, this GIC exposes 16 nonsecure priorities.
The FreeRTOS configuration uses that range, and startup checks the priority-mask
readback. The observed mask is `0xf0` on both the board and the secure emulator.

## Physical results

On 2026-10-15, two cases passed using the same 24,544-byte raw image:

| Case | Observation | Scope established |
| --- | --- | --- |
| Forced stall | EL1 masked exceptions and stalled; EL3 captured interrupt 29 after 2.000027 seconds and returned to FEL | Recovery from this EL1 stall through the secure timer |
| Kernel | 100 queue exchanges in 100 ticks, with non-yielding background work and floating-point context checks | Physical timer preemption, blocking wakeup, queue ordering, and context preservation on the boot CPU |

The kernel case took 0.100423 seconds from monitor entry to return, measured
with the architectural counter and its declared frequency. These are single
functional observations, not boot-reliability or timing-variance measurements.
The counter frequency has not been independently calibrated.

Both returns recreated the USB node. The session access rule granted permissions
automatically, and the host identified A733 before reading each result. Neither
case issued persistent-storage writes. The board remains in FEL after the check.

Raw evidence:

- Forced stall: `~/.cache/sunstoneos/evidence/kernel-20261015-230456`.
- Kernel: `~/.cache/sunstoneos/evidence/kernel-20261015-230508`.
- Secure emulator: `~/.cache/sunstoneos/evidence/a733-kernel-model.json` and
  `a733-kernel-model-timeout.json` in the same evidence directory.

The physical image SHA-256 is
`fc204fa9819d9da61113de31953fd7c8f3d0a9da4dd34388ed297d017b4a3313`.
Its ELF SHA-256 is
`f79953087cc5a012da3c2a7dccd9628501f356e02f4b58278b470a698ae32128`.
The secure emulator ELF SHA-256 is
`aeaa6c680cb8d71cf5f90f2f52af9f5ec5421379a24aade350701dd720b637e0`.

ELF section totals are 24,368 bytes of text/read-only data, 168 bytes of
initialized data and monitor state, and 79,216 bytes of BSS and reserved stacks.
The host also reserves a separate 16 KiB result block. These figures cover the
check image, including static tasks and translation tables; they do not estimate
the memory needed by mimus or the completed OS.

## Build and run the affected check

Use the toolchain and pinned kernel source from [build.md](build.md). The existing
configured build directory supports the physical image and secure emulator model:

```sh
cache="$HOME/.cache/sunstoneos"
cmake -S . -B "$cache/build/qemu-virt"
cmake --build "$cache/build/qemu-virt" --target a733-kernel-check a733-kernel-model -j 8
/usr/lib/llvm-21/bin/llvm-objcopy -O binary \
  "$cache/build/qemu-virt/a733-kernel-check.elf" \
  "$cache/build/qemu-virt/a733-kernel-check.bin"
```

The physical runner requires initialized DDR, a retained prior probe signature,
and the installed session access rule. [Boot preparation](boot.md) describes
those prerequisites. Select the case affected by the change:

```sh
python3 tools/check-a733-kernel.py \
  --fel "$cache/build/sunxi-a733/sunxi-fel" --mode timeout
python3 tools/check-a733-kernel.py \
  --fel "$cache/build/sunxi-a733/sunxi-fel" --mode kernel
```

The runner holds the shared board lock, checks identity and the reviewed ROM
return entry, verifies the upload, and executes once. After intentional
re-enumeration, it waits for the new node and permissions before sending another
USB request. A transport error or missing return stops the run without retry.

For a changed monitor or scheduler, the secure emulator exercises the same
monitor and EL1 implementation with QEMU's GIC addresses and a semihosting exit:

```sh
"$cache/host/usr/bin/qemu-system-aarch64" \
  -machine virt,secure=on,virtualization=on,gic-version=3 \
  -cpu cortex-a55 -smp 1 -m 128M -nographic -monitor none -serial none \
  -semihosting-config enable=on,target=native \
  -device loader,file="$cache/build/qemu-virt/a733-kernel-model.elf",cpu-num=0
```

The normal case prints the kernel result and exits with status 0. Add
`-device loader,addr=0x42000050,data=1,data-len=8` for the forced-stall case;
its expected exit status is 2. Use a ten-second host timeout for either case.
QEMU's counter rate differs from the board declaration, so emulator elapsed
time does not measure the board's timeout duration.

Successful evidence remains reusable while relevant inputs remain unchanged.
Documentation changes and commits do not require another boot or emulator run.


On 2026-10-18, seven focused instruction cases checked this ARE correction:
physical rejection with neither or only one ARE bit set, physical cleanup with
both bits set, QEMU cold startup, and an early QEMU RWP timeout. They execute
the compiled monitor and reject every write that clears an enabled ARE bit.
The three physical admission refusals make no GIC write. One affected secure-QEMU
kernel build also completed 100 exchanges and exited with status 0.

Evidence is under `~/.cache/sunstoneos/evidence/monitor-are-20261018/`:
`instructions/result.json` contains the seven modeled cases, and `qemu-kernel.json`
contains the secure-emulator run. These checks establish ARE admission/preservation;
they do not qualify the monitor's other timeout or redistributor cleanup behavior.
The corrected physical monitor has not been run on the board. Earlier physical
results above remain evidence for their recorded image.
