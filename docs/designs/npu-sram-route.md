# Route the shared SRAM to the NPU in every NPU startup

Status: **implemented** in the NPU startup sequencer, the NPU backends and the finite image's EL3 return path; **qualified on the board** for the routing effect and the finite startup; **host-tested** for the second start in one power lifetime and the failure classes. The [three-start runbook](../board-runbooks/npu-starts.md) tests the production sequence, the power-first order and adoption of a running PLL_NPU on a board. Records: [npu-sram-route.jsonl](../evidence/npu-sram-route.jsonl).

## Problem and reference

The A733's 512 KiB shared SRAM appears to the CPU at `0x00074000`-`0x000f3fff` and is the NPU's internal SRAM. Bit 1 of SYSCTRL `0x03000008`, which the manual does not document, selects its owner: set, the CPU; clear, the NPU.

- The boot ROM sets bit 1 at ROM offsets `0x20c`-`0x220` before it first uses its stack at `0x000f3400` in that SRAM. The FEL scratchpad at `0x000ee800` is also in it. A cold entry at `0x0` reaches that store before touching the SRAM. The forced-FEL return that SunstoneOS images use enters at `0x20` and zeroes `0x00044000`-`0x000f3ffc` before it reaches `0x20c`.
- Board U-Boot clears bit 1 and reads it back at board init, before any NPU clock or reset change (comment "set sram to npu").

SunstoneOS images load through FEL, so no U-Boot runs and both boards read `0x03000008` = `0x2`. With the SRAM routed to the CPU the finite image's jobs complete with wrong outputs; with the routing below the outputs match the reference (`npu-sram-route-routing-effect`).

The vendor kernel powers domain 4 before any NPU clock or reset change ([NPU reference backsolve](../reference-backsolve-npu.md)): its NPU driver compiles out runtime power management, and the platform bus attaches the device to its power domain with power-on before probe. The sequencer follows that order.

## Startup sequence

`a733_npu_startup_step` runs these stages on the bound bootstrap task ([npu-startup.md](../npu-startup.md)):

1. **Tokens and supply.** The clock, rail and PCK tokens, then the wait for the owner's DCDC2 verification.
2. **Register pass and power request.** Under the register lock, the owner's domain-4 check and the read-only register pass, which also requires bit 1 of `0x03000008` set, the boot ROM's routing. A refusal has written nothing and releases every token. The step then requests domain 4 ON through its row and returns PENDING. The owner adopts an ON domain without a PCK600 write and powers an OFF domain.
3. **Row.** The step waits for the owner's answer, bounded by `A733_POWER_PCK_BOUND_US` and two ticks. From here on every failure keeps the tokens.
4. **PLL_NPU.** The method-2 enable table ([npu-pll.md](../npu-pll.md)), or, for an adopted PLL, one read that must still show it locked at the profile. LOCK is then sampled three times in a row in both cases.
5. **Open.** Under one lock acquisition: the PLL output gate opens; bit 1 of `0x03000008` is read, required set, cleared and read back; then the local open table releases the AHB, AXI and core resets and opens the bus and module gates. The 2 ms settle starts at the end of this stage, after the vendor platform's power and clock hook.
6. **Final readback.** Domain 4 ON, bit 1 clear, the local words and the PLL word. The step publishes the register window open and returns READY.

The routing write comes after PLL_NPU has locked and its output gate opened, before the first write of CCU `+0xb04`. The register pass established that the NPU resets are asserted and its bus and module gates closed, and the exclusive NPU-local clock token keeps every other writer out until the write.

`a733_npu_startup_stop`, after the controller's CLOSE, closes the register window, applies the local stop table, then sets bit 1 again with readback.

The allowlist adapter ([`a733_npu_platform_native.c`](../../drivers/npu/a733_npu_platform_native.c)) writes only bit 1 of the routing word, and only while CCU `+0xb04` holds the NPU's AHB, AXI and core resets asserted and its bus gate closed and the module gate at `+0xb00` is closed.

### A second start in one power lifetime

Local stop leaves PLL_NPU enabled, locked at the profile and with its output gate open; domain 4 stays ON. The register pass adopts only that PLL state: control word `0xf8305100` apart from the inherited reserved bit 24, with the gate controls at CCU `+0x1920` and `+0x19a0` set. The vendor clock driver behaves the same way: its PLL enable helper returns without a write when the PLL's enable, LDO and output gate bits are already set. Adoption makes no PLL or gate-control write. Every other enabled state refuses before any write: other factors, the output gate closed, a clear gate-control bit, or no LOCK. A second start takes 4,414 µs in the host model (`npu-sram-route-host-cost`).

## Register window

The startup structure holds one atomic word, `window_open`. The bootstrap task stores 1 with release order when it reaches READY, and 0 at the start of local stop and on any failure. The three NPU backends that bind a startup, [`npu_convolution.c`](../../drivers/npu/npu_convolution.c), [`a733_npu_runtime.c`](../../drivers/npu/a733_npu_runtime.c) and [`npu_session.c`](../../drivers/npu/npu_session.c), load it with acquire order before every register read and write and refuse the access while it is 0. A refusal makes no bus access; the controller then faults with an I/O failure and retains its resources. READY requires the routing, so an open window implies the SRAM belongs to the NPU. The window costs one acquire load and one compare per register access.

The raw adapter ([`npu_native.c`](../../platform/a733/npu_native.c)) has no startup and keeps no window. Only host models call it directly.

## Return to the ROM

The finite image's EL3 receipt path ([`monitor.c`](../../apps/npu-finite/monitor.c)) reads `0x03000008` after the GIC and secondary-core cleanup and before it publishes the report. If bit 1 reads clear, it first requires CCU `+0xb04` to hold the NPU's AHB, AXI and core resets asserted with the bus gate closed and the `+0xb00` module gate closed, then sets bit 1. It parks with `a733_smp_stop(7, 0x03000008)` if that check fails or the readback still shows the NPU routing. Only then does it call `npu_finite_rom_return`, which holds the image's only `RMR_EL3` write. This step does not depend on the EL1 restore at local stop. The default, thermal, interrupt and entry-checkpoint profiles of the finite image share this monitor.

The other images that link the startup do not return to the ROM. The supervised OS with the NPU profile, the NPU board image, the convolution images and the accelerator composition end in a terminal halt that needs a power removal. Their local stop restores the CPU routing at EL1.

## Exit paths while the SRAM is routed to the NPU

The bit is clear from the open stage until local stop. Every way out of that window in the finite image:

| Path | Restore |
| --- | --- |
| Normal: two jobs, CLOSE, local stop, quiescence, SMC, EL3 receipt check, ROM return | EL1 at local stop, then EL3 before the report and `RMR_EL3` |
| Local stop fails or retains | No quiescence, so `platform_exit` issues SMC 4; EL3 parks in `a733_smp_stop` |
| Any EL1 fault, deadline or refused receipt | SMC or exception to EL3; the monitor parks in `a733_smp_stop` (status 6) |
| EL3 cleanup failure | Parks with status 7 before the restore |
| EL3 recovery timer (FIQ) | The monitor parks in `a733_smp_stop` (status 6) |
| Bus stall | The CPU never returns |

The parked paths do not restore the bit, and they never return control to the ROM: `a733_smp_stop` waits in `WFE` at EL3 with interrupts masked and writes no reset flag. Only a power removal recovers them, and that is a cold entry at `0x0`, which sets bit 1 before touching the SRAM.

That bound depends on the watchdog staying disabled. A watchdog reset would re-enter the ROM without the EL3 restore, and the state of SYSCTRL after such a reset is unknown. The finite host refuses to launch when `0x02050014` bit 0 is set (`apps/npu-finite/host.py`), and the monitor refuses entry when it is set (`apps/storage-read-single-core/monitor.c`). The OS NPU profile is supervised only: configuration refuses it with the resident lifetime, whose watchdog would reset the board.

## Shared SRAM use while it belongs to the NPU

- **Image.** Every loadable segment and symbol of the finite image lies in DRAM (`0x41000000`-`0x42200000`); the check confirms that none lies in the shared SRAM.
- **EL1 mapping.** The first gigabyte is one Device block (`platform/a733/cpu_coherence.c`), so the CPU makes no speculative or prefetch access to the shared SRAM; no image source names an address in it.
- **Launcher and host.** The launcher, request, report and stack regions (`0x52000`-`0x54000`) and the FEL scratch bytes the readers use (`0x71000`-`0x71080`) lie in SRAM A2, below `0x74000`. The host reads the shared SRAM only after the ROM has returned.
- **ROM.** The ROM's own stack and scratchpad are in the shared SRAM. It does not run between the launch and the forced-FEL reset, which follows the EL3 restore.

## Failure classes

- Register pass refusal, including every unadoptable PLL state: nothing written, every token released.
- Domain 4 read ON by the pass but its row refused or failed by the owner: PLL_NPU already started, so the startup retains with PLL_NPU enabled, possibly locked, and its output gate closed; the NPU resets stay asserted and the SRAM stays with the CPU. Recovery is the power removal, as for every retention.
- Domain 4 not ON within the bound: tokens and row retained; no NPU clock, reset or routing write has happened.
- Bit 1 already clear on entry: refused in the register pass, nothing written, every token released.
- Routing write not read back: the register lock and every resource retained; no reset released.
- Register access outside READY: an I/O fault of the controller; all resources retained.
- Restore readback failure at local stop: RETAINED, so no quiescence and no normal return. At EL3: park with status 7. Both need a power removal.

## Checks

- `checks/npu-startup-host.c` runs the sequencer with the actual PMIC provider, rail ledger, clock registry, GPU/PCK manager and power-domain owner. It checks domain 4's PCK600 command before the first CCU write, the routing write after the PLL output gate and immediately before the first reset release, the window open only at READY, the refusal of an NPU-routed entry word in the pass, the retention when the word ignores the write, the failed restore at stop, and (`--second-start`) a second startup over the state that local stop leaves, with the reserved PLL bit 24 clear and set, and ten adoption refusals that write nothing.
- `checks/npu-startup.c` runs it on two QEMU cores with real FreeRTOS tasks.
- `apps/npu-finite/sram-route-check.py` runs the linked sequencer and allowlist adapter of the finite image from the register pass through local stop, a second start, the two routing refusals, the convolution backend's window, a rate change from the other profile and the reachability of the only `RMR_EL3` write: 11 cases, and 12 for the 852 MHz image, which adds the speed-bin refusal.
- `apps/npu-finite/check.py` runs the EL3 return path with SYSCTRL modeled: the restore precedes the report on both normal returns, an already-set bit is not written, a word that ignores the write parks with status 7, and every refused receipt parks before the restore.
- `checks/npu-stop-native.py` exercises the adapter's routing-word rule, including its refusal while a reset is released or the module gate is open.
- `checks/npu-convolution.c`, `checks/npu-session-runtime.c`, `checks/accelerator-shared-owner.py` and `checks/os-npu.py` pass with the window.

## Figures

| Figure | Value | Record |
| --- | --- | --- |
| Boot to READY, domain 4 OFF at entry, host model | 4,628 µs, 4 calls | `npu-sram-route-host-cost` |
| Second start, PLL_NPU and domain 4 adopted, host model | 4,414 µs, 6 writes, 3 LOCK samples | same |
| Routing effect on the finite image | outputs match the reference only with the routing | `npu-sram-route-routing-effect` |
| Images changed by the startup change | 22 of 68 ELF files | `npu-sram-route-image-comparison` |
