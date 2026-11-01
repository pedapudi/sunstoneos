# Returning NPU images under WDT0

The finite startup, single-inference and two-network images accept WDT0 mode
`0xb1`: enabled, with a 16-second interval. Their retained 40-byte marker
declares the common watchdog ABI, RTC base and a 20-second application window.
The host arms the timer immediately before executing the SRAM launcher.
The launcher and EL3 monitor require system-reset configuration 1, reset output
2 and disabled primary and alternate software redirects.

An independent supervisor task restarts WDT0 every 20 milliseconds while the
application is live. Application failure, the session deadline or an unexpected
watchdog configuration stops these restarts. EL3 stops shorten an enabled timer
to half a second. Normal return requires successful controller CLOSE, local
stop, SRAM handback and monitor cleanup. It then disarms WDT0, checks the mode
readback and records RETURN_ROM before requesting the CPU-local ROM return.
An ignored disarm write prevents that normal return.

## Reset contract and its evidence

**Observed in the published ROM:** `fixs-brom.bin`, SHA-256 prefix `3a2a7d28`,
branches from reset address `0x0` to `0x44`. With both software redirects
disabled, CPU0 reaches `0x210–0x220`, which sets SYSCTRL `0x03000008` bit 1,
before its first SRAM access. Loading a stack pointer before that store does
not access the stack. The captured board ROM has the same reset instructions
through `0x228`. Source and binary inventories are in
[the ROM reference](../../docs/vendor-firmware/boot-rom-and-boot0.md).

**Observed difference:** forced-FEL entry `0x20` clears SRAM
`0x44000–0xf3fff` before restoring the routing bit. It is suitable for the
normal CPU-local return only after the image has handed SRAM back. The armed
watchdog lifetime therefore installs no software redirect to `0x20` and uses
no recovery trampoline in SRAM or DRAM.

**Inferred from the whole-system reset configuration and ROM instructions:**
WDT0 reset takes the normal ROM path, which restores CPU SRAM routing before
using SRAM even if the NPU owned it at expiry. The route need not survive reset.
The generic watchdog trials returned to FEL on both boards and retained the
seven RTC breadcrumb words, as recorded in `wdt-fel-return-20261031` evidence.
Those trials did not run an NPU job. Recovery with active NPU transactions,
including the interconnect reset, remains physically unqualified.

No image or host recovery decision depends on DRAM or SRAM contents surviving
watchdog reset. The returning transport reads the watchdog mode and RTC records
before requesting DRAM. A watchdog return does not supply a normal receipt or
permit restoration of old DRAM or SRAM over the new ROM instance. The next
attempt starts with `tools/run-ddr.py`, then fresh NPU system-register reads.
Development admission requires CPU SRAM routing, closed local gates and asserted
local resets. It accepts the supported disabled or locked PLL state. A failed
readback refuses the attempt; it does not trigger a retry or infer device drain.

## Configuration and checks

`tools/dev-run.py npu` consumes the NPU's native 456-byte session template,
preserves its PMIC and pin configuration, and binds new session identities to
the selected image and live fabric clocks. The template is configuration data,
not current physical qualification. The NPU startup independently checks its
power, PLL, routing and speed-bin requirements. Bin 4 selects the vendor's
852 MHz point at a verified 800 mV.

The focused watchdog check exercises admission, failure/deadline refresh
suppression, ignored disarm writes and stop behavior. Linked instruction models
exercise the active EL1 vectors, first-failure preservation and monitor return.
The breadcrumb check verifies finite stages `0x0601–0x060b`, retained failure
`0x06ff`, runtime-helper stages `0x0620–0x062f`, and GPU/NPU first-failure names.
The generated image README records each image's hash, expected report, stage
order and coordinator commands. These software checks do not replace the
active-NPU watchdog recovery test.
