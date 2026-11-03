# Design: high and low performance modes

Status: **implemented; host-tested**. No board has run the service; physical mode changes and recovery are open. A performance mode is a named set of operating points, one per block: the six Cortex-A55 cores (CPU-L), the two Cortex-A76 cores (CPU-B), the DynamIQ Shared Unit that holds the L3 cache (DSU), the GPU and the NPU. An operating point is a clock rate with the supply voltage the vendor runs it at. **High** is the default and uses, for every block, the highest operating point that the vendor uses for the chip's speed bin, with the vendor's voltage. **Low** is an optional power-saving mode built only from vendor operating points ([AGENTS.md](../../AGENTS.md), "Implementation"). The user approved CPU, GPU and NPU clock and voltage changes ([handoff](../handoff.md)).

The mode service switches an image between the two modes at run time, at the user's request from the host, and reports each block's rate and voltage read back from the hardware. [The user guide](../performance-modes.md) describes the host command. Host-model results are in [the records](../evidence/performance-modes.jsonl); the board case is in [the board runbook](../board-runbooks/performance-modes.md).

## The mode table for speed bin vf0400

Both boards read speed-bin code 4, column vf0400 (SID word `0x0300624c` = `0x496` on port 3-3, `0x495` on port 3-1). The table is `drivers/power/a733_perf_mode.c`, from linux-orangepi `2ac08e8c`: `sun60iw2p1-cpu-vf.dtsi` V0.9 ("cpu-vf") and `sun60iw2p1.dtsi` ("soc"). The boards' oscillator runs at 26 MHz, so the CPU and NPU rows use the `opp-microvolt-26m-vf0400` column.

| Block, supply | High | Low |
| --- | --- | --- |
| CPU-L, DCDC5 | 1794 MHz at 1000 mV (cpu-vf `:599`, `:615`) | 1196 MHz at 800 mV (cpu-vf `:438`, `:454`) |
| CPU-B, DCDC3 | 2002 MHz at 1050 mV (cpu-vf `:1340`, `:1356`) | 1404 MHz at 800 mV (cpu-vf `:1110`, `:1126`) |
| DSU, DCDC5 | 1274 MHz at 1000 mV (cpu-vf `:1989`, `:2005`) | 910 MHz at 800 mV (cpu-vf `:1851`, `:1867`) |
| GPU, DCDC4 | 1008 MHz at 960 mV (soc `:4736`, `:4739`) | 600 MHz at 800 mV (soc `:4682`, `:4685`) |
| NPU, DCDC2 | 852 MHz at 800 mV (cpu-vf `:2069`, `:2087`) | 492 MHz at 800 mV (cpu-vf `:2039`, `:2063`) |

**Choice of each low point.** Low uses the minimum voltage of the block's table, 800 mV, and at that voltage the highest rate of the bin's column, so the mode saves the voltage step and loses as little rate as the table allows.

- CPU-L and CPU-B: 1196 MHz and 1404 MHz are the fastest 26 MHz rows at 800 mV in vf0400 ([CPU top points](cpu-top-opp.md)).
- DSU: the vendor rule below gives 910 MHz for the low clusters.
- GPU: the vendor kernel reads `opp-microvolt-vfdefault`, because its device tree has no speed-bin map for the GPU ([GPU vendor default](gpu-vendor-default.md)). In that column 400 MHz and 600 MHz run at 800 mV and 800 MHz needs 840 mV, so low is 600 MHz. High stays 1008 MHz at the 960 mV that the vendor kernel applies and that the board ran (the vf0400 column itself lists 900 mV).
- NPU: 492 MHz is the only row below the vendor's 852 MHz point; both run at 800 mV, so the NPU's low mode saves clock power only.

**DSU rule.** The DSU is never selected on its own. Its point follows the two clusters by the vendor dsufreq rule (`sunxi-dsufreq.c`; SHA-256 in `reference-backsolve-20261025/cpu/sources.json`). The rule takes the smaller of two bounds (`:295`):

- three quarters of the faster cluster, rounded down to 26 MHz (`:284`–`291`);
- the fastest DSU row whose voltage equals that of CPU-L's row (`:197`–`228`, `:293`), or, when no DSU row has that voltage, the fastest DSU row at the lowest voltage (`:489`–`495`).

While CPU-B runs at 1488 MHz or more, the DSU stays at least at that fastest row at the lowest voltage (`:314`–`320`). With both clusters high the rule gives min(1482, 1274) = 1274 MHz; with both low, min(1040, 910) = 910 MHz. CPU-L high with CPU-B low gives 1274 MHz, and CPU-L low with CPU-B high gives 910 MHz. The CPU clock driver admits only table rows, so a result that is not a row of the column is refused before any write.

**Per speed bin.** The table has one entry per bin column, holding the high and low point of every block. The service reads the SID word, maps it with the CPU clock driver's map (`a733_cpu_clock_bin`) and refuses a bin with no entry, including code 0 (an unprogrammed fuse) and every column other than vf0400, before any write. `checks/perf-mode.py` compares every entry with the pinned rows and recomputes its DSU points with the rule.

## Where each block's change comes from

**CPU-L, CPU-B and the DSU.** The CPU clock driver of [the CPU top points](cpu-top-opp.md) performs one domain change in the vendor order. For a raise it writes the voltage, waits |ΔV| ÷ 250 µV/µs rounded up to 1 ms, then relocks the PLL; for a lowering it relocks first and then writes the voltage. It parks the cluster on the oscillator during the relock as boot0 does, and keeps DCDC5 covering both CPU-L and the DSU. The service issues one driver request per domain that changes, in this order:

1. the DSU, when it lowers and its target is already at or below three quarters of the faster cluster;
2. the clusters that lower, CPU-L then CPU-B;
3. the clusters that raise, CPU-B then CPU-L;
4. the DSU, when it raises, or when its lowering had to wait for a cluster raise. A THROTTLE lowering that was never restored can leave that state.

This is the vendor order (`cpufreq-dt.c:85`–`109`): the DSU is lowered before a cluster change and raised after it.

**NPU.** The vendor changes the NPU clock only while the NPU is stopped. Its run-time path, debugfs `vipdrv_clkfreq_set` (`vip_drv_os_debug.c:411`–`461`, SHA-256 `f127e195…`), refuses while a core is powered ("NPU is running, unable to change freq", `:430`). `vipdrv_drv_update_clk_freq` (`vip_drv_device_platform.c:822`–`886`, SHA-256 `a47f85f2…`) then brackets the change with `npu_clk_uninit` and `npu_clk_init`, which close the module clock and assert the resets. The devfreq hook (`:974`–`1020`) changes only the core's internal divider, in power-of-two steps, so it is no precedent for a 492 MHz point. [The NPU PLL design](../npu-pll.md#two-profiles-and-the-rate-change) states the same rule: the rate changes only while the module gate is closed and the resets are asserted.

The service therefore changes the NPU only after its session has ended and the local stop has closed its gates and asserted its resets. The change is the board kernel's PLL rate change (`ccu_nm_set_rate`, `ccu_nm.c:250`–`344`), which the NPU startup also makes for a PLL that an earlier image left at the other profile. The steps are output gate off, factors, lock enable cleared and set, three lock reads 3 µs apart, 20 µs, output gate on. The NPU bootstrap task, the startup's only owner, makes it (`os_npu_perf_retune` in `apps/os/npu.c`). While a session is open the switch waits for the stop, at most 1.5 s, and otherwise refuses as busy with nothing written. At boot the NPU starts at the boot mode's rate (`a733_npu_startup_rate`). The OS NPU service runs one session per lifetime and has no restart, so a switch after the session changes the rate that a later start would use. Running NPU work in both modes within one lifetime needs an NPU restart; until it exists, NPU work runs at the boot mode, and an image built with the low boot mode runs it at 492 MHz.

**GPU.** In images with the GPU service (`SUNSTONE_A733_OS_GPU`) the GPU owner changes the GPU's point through its privileged worker (`os_gpu_perf_action`). The live owner switches only after acknowledged firmware idle, retaining DCDC4 and PERI0 ownership through the transition, with voltage before a frequency increase and after a decrease. [The GPU performance design](gpu-performance.md) holds the sequence. An image without the GPU service reports the GPU as **not owned**: a switch leaves it unchanged, and a request that names it is refused. The vendor sequence the owner follows is in the record `performance-modes-gpu-reference`.

## The mode service

**Safety rules.**

1. **Admission before any write.** A switch starts only when the speed bin has an entry, every named block is owned, and the thermal policy reads NORMAL with every sensor read. No other command may run, the last readback must cover every owned block, the DSU rule must give a table row, and the CPU grant must keep two changes for thermal restores after the switch. Otherwise the service refuses with a reason and writes nothing.
2. **Plan from the hardware, report the hardware.** Every command ends with a readback of every block, and a switch plans from the last readback. The CPU clock driver inspects the live PLL and rail again before each write.
3. **NPU only while stopped.** The NPU changes after its session has ended. A session still open after 1.5 s makes the service refuse the switch as busy, with nothing written.
4. **CPU order** as above; the driver orders voltage and frequency inside each domain.
5. **Stop at the first failure.** A step that the driver refuses, that expires or that fails ends the switch. Blocks already changed keep their new point, the failed block keeps the state its driver leaves, and the remaining blocks keep their old point. The service reads every block back and reports the mode as mixed, the failed block and the reason. It makes no automatic rollback, which would add writes after a failure whose cause is not known. The user may request a mode again.

**Failure classes** (performance architecture, section 1.8):

| Failure | Class | Retained |
| --- | --- | --- |
| Any admission refusal; the NPU session still open after 1.5 s | 1, before any device access | Nothing |
| CPU driver REFUSED or EXPIRED | 1 before the first write; after a voltage raise the rate is unchanged and the raised voltage covers it, which the readback shows | Nothing; the driver releases its leases |
| CPU driver RETAINED | 3, state unknown | The driver's leases and PMIC request; the thermal task fails the board. The report is written first, but USB0 stops answering once the board fails, so it stays in RAM |
| NPU PLL write or lock failure | 3 | The NPU startup's tokens and register lock; the board fails, as for every NPU platform fault in the OS image |
| A decoded read refusal with completed provider cleanup | 1 | Nothing; the block is reported as not read |
| PMIC completion still pending at its deadline, or uncertain reap/release | 3 | The outstanding request and rail token; the CPU owner publishes failure and terminates the board session |

**Thermal.** A switch needs NORMAL. Under THROTTLE the CPU clock driver keeps lowering the clusters to 1014 MHz and restoring them at NORMAL; each restore spends a grant change, which is why admission keeps two. A thermal transition or an independently started CPU transition invalidates the cached readback. During a switch it also cancels the remaining plan. An already-started owner action finishes under its original bound; no later planned write starts, even if NORMAL returns first. A fresh stable readback is required before another switch. Readback snapshots drain any outstanding request before releasing its rail; unproven completion terminates the session.

**Boot.** The image's boot mode is the image manifest's boot field (`perf_boot_mode`, default `high`). The NPU starts at that mode's rate. Once the CPU sensors have a reading, the CPU owner reads every block back and then switches the CPU domains to the boot mode through the user path. This replaces the one-request-per-cluster start of an image without the service. The recognizer waits for it as it waits for those requests (`os_cpu_clock_ready`).

**Grant.** Each CPU domain change spends one change of the session's CPU grant (at most 64); a domain already at its target is not requested. A mode-service image needs a grant that names both clusters at the bin's high points. From the boot state the boot switch spends three changes, and each later full switch spends three, so a session allows 19 full switches with two changes kept for restores. The report carries the changes left.

## OS integration

- **Build option.** `SUNSTONE_A733_OS_PERF=ON` adds the service. It needs `SUNSTONE_A733_OS_CPU_CLOCK` (A76 admitted) and may be combined with `SUNSTONE_A733_OS_NPU`, whose profile then accepts the CPU grant. `SUNSTONE_A733_OS_PERF_BOOT=high|low` (default `high`) sets the boot mode, which the image stores and the link check records as `perf_boot_mode`. The linked `os_perf_usb_operation` publishes `perf_operation: 8`; the host refuses manifests without that exact capability. Images without the option link nothing new.
- **Owner.** The thermal task, which owns the CPU clock driver, runs the service: one bounded step after each thermal step, once per 1 ms tick, with the driver's 200 µs bursts. A thermal transition cancels the active plan after its outstanding owner action finishes. A terminal driver result belongs to the service only when the service's own request started the transition.
- **Readback.** Register words: the CPU clock report's eight reads (oscillator, SID, each CPU PLL and mux word); PLL_REF (CCU `+0x000`), PERI0 (`+0x0a0`), PLL_GPU0 (`+0x0e0`), the GPU clock word (`+0xb20`), PLL_NPU (`+0x2a0`) and the NPU clock word (`+0xb00`). Rates are decoded with N and P0 as field + 1 from the 24 MHz PLL_REF, which the decode verifies, with the GPU divider table and the NPU module divider. A gated module clock is reported as stopped, with its PLL's rate. Voltages: one PMIC snapshot each of DCDC5, DCDC3, DCDC4 and DCDC2, each under a separate shared rail lease, taken only while the CPU driver is idle. Each request has a 150 ms deadline; expiry with completion unproven retains ownership and ends the session. Rates and voltages are programmed values, and the readback changes no driver state.
- **NPU stop and retune.** The NPU bootstrap task waits for retune requests after the local stop. The service posts the target rate in an atomic word and notifies the task; the task answers in a second atomic word. The service admits an NPU change only when the startup reached READY at boot, and waits for the stop as above.
- **USB0 operation 8.** One 16-byte request carries one of three commands in its flags byte: report (0), read back (1) and switch (2), with the mode and block selection in the offset word. A switch must name the session's epoch in the generation field; a report and a read back need none. USB0 numbers each accepted command and answers every request with the 192-byte report, without a frame header, as operations 5 and 6 do. USB0 writes the answer field: accepted with its number, busy (a command waiting or running, or a report that could not be copied consistently), or refused (a stale epoch). A request whose framing is malformed stops USB0, as for every other operation.
- **Report.** Written by the thermal task under a sequence counter (odd while writing) and copied by the USB0 task with at most four attempts. It names the session, the last accepted and completed command numbers, the result, the reason and the failed block, the boot mode, the present mode (high, low or mixed), the bin, the CPU changes left. Per block it names the target point, the read-back rate and voltage, whether the image owns it, and whether its clock is stopped.

## The host command

`tools/sunstone-perf.py high|low|status [--block cpu-l,cpu-b,cpu,npu]` takes the board lock of the port, opens the running OS on USB0, checks that the image manifest records the service, and sends operation 8. `status` requests a readback, waits for its completion and prints each block's target, rate and voltage. `high` and `low` take the epoch from a report, send the switch and wait for its completion, at most 5 s. The supervised launcher holds the board until its run ends, so the same functions also serve it: `--perf-sequence high,low,high` makes one switch before each recognition.

## Qualified figures

All rows are host-model results; see `performance-modes-host-checks`.

| Metric | Value |
| --- | --- |
| CPU requests and PMIC selector writes per full switch | 3 and 2 |
| Full switch in the engine model, 1 ms owner step | 5.4 ms (boot switch from boot0's state 5.0 ms); budget under 30 ms |
| Boot switch with readback in the OS owner model | 30 owner ticks |
| PLL_NPU writes per retune of a stopped NPU | 5, each read back; 233 µs and 43 lock samples modeled |
| Idle cost | No register reads, PMIC snapshots or device requests in a thousand idle ticks |
| Default OS images | Loadable bytes equal to the base apart from the build identity (the NPU variant differs inside `a733_npu_startup_step` by code generation) |
| `.text` of the CPU performance image with high boot | 903,584 bytes |

No path in the service runs per job or per frame; it uses no heavier form of section 1 of the performance architecture.

## Checks

`checks/perf-mode.py` compares every table entry with the pinned device-tree rows, compiles `checks/perf-mode-model.c` with the engine and the CPU clock driver's tables, and runs it. It also runs the OS owner (`apps/os/perf.c`, in `checks/perf-os-model.c`) over modeled providers with and without the NPU, and drives `tools/sunstone-perf.py` against a modeled USB device. The engine model covers the order of every switch, every refusal before a write, an NPU session that ends inside the bound and one that does not, each step failing, the report after a failure, and a bin without an entry. The NPU startup host model (`checks/npu-startup-host.c`) covers the retune of a stopped NPU. The native check reuses the existing OS NPU instruction model; RTOS and device effects are modeled.

## Limitations and open work

1. Run [the board case](../board-runbooks/performance-modes.md) (`performance-modes-board-plan`): one lifetime boots the OS in high, runs NPU jobs, switches to low and back while reading each block, and runs a recognition in each mode. It follows the CPU top-point steps on port 3-3.
2. The NPU cannot run in both modes within one lifetime until the OS NPU service can restart.
3. Only speed bin vf0400 has a table entry; other bins refuse.
4. Readback reports programmed values, not analog voltage.
