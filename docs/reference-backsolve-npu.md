# NPU bring-up derived from the vendor reference

This record derives the vendor's NPU sequence on the Orange Pi Zero 3W
(Allwinner A733) from source and boot firmware. It runs from cold boot to the
first NPU register access, then through one job, its completion and shutdown.
It then compares the SunstoneOS startup sequencer, controller, page-table
builder, power-domain owner and NPU board-bundle rows with that sequence.
Each divergence is classified and ranked by its risk of stalling a bus access
or hanging the board.

Status: reference analysis only. It made no board, queue, USB or microSD
access and changed no runtime code.

One physical NPU run already exists. On port 3-1, the supervised numerical
image (SHA-256 `9e4adabf…`) passed startup, opened the controller, completed
two copied and reaped jobs, closed, stopped locally and returned to the boot
ROM. Every output byte differed from the reference, and the two outputs had
the same digest. The record is
`~/.cache/sunstoneos/evidence/processor-completion-20261024/port-3-1/npu-numerical/runtime/session.json`.
The startup and controller register sequence has therefore run once without
a hang. The vendor's routing of the shared SRAM to the NPU, absent from
SunstoneOS, is the divergence that best fits that numerical failure.

## Sources and their gaps

Every source is pinned and hashed in
`~/.cache/sunstoneos/evidence/reference-backsolve-20261025/npu/sources.json`.
Citations below give a path inside the named repository and a line number,
or a binary's SHA-256 prefix and offset.

| Reference | Repository and revision | Contents used |
| --- | --- | --- |
| Vendor NPU kernel driver: VIPLite 2, the Allwinner `aw_nna_vip` module | `orangepi-xunlong/linux-orangepi` `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, under `bsp/drivers/npu/aw_nna_vip/vip2/` (cited as `vip2/…`) | Platform clock, reset, regulator and power code; register sequences for reset, initialization, MMU, submission, interrupt, cancellation and clock suspend; build configuration; feature database |
| Power-domain driver | same kernel, `bsp/drivers/pm_domain/pck600_domains.c`, `Makefile`, `Kconfig` | PCK-600 delay row, ON/OFF command and status poll |
| Clock driver | same kernel, `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c`, `ccu_gate.c`, `ccu_nm.c` | PLL_NPU, NPU module, bus, AHB and MBUS master gates, resets |
| Device tree | same kernel, `sun60iw2p1.dtsi`, `sun60i-a733-orangepi-zero3w.dts`, `sun60iw2p1-cpu-vf.dtsi` | NPU node, PCK-600 node, `npu-supply` on DCDC2, operating points |
| Generic power-domain core | same kernel, `drivers/base/platform.c`, `drivers/base/power/domain.c` | Domain attach at probe |
| Shipped kernel configuration | `orangepi-xunlong/orangepi-build` `a95129bf30ea11886cc35f08ebb3e69695a26656`, `external/config/kernel/linux-sun60iw2-{current,legacy}-a733.config` | Which driver branches are compiled |
| Board U-Boot | `orangepi-xunlong/u-boot-orangepi` `b791be842935b27268ae3d00e943a9075495f30a` | Shared SRAM routing |
| Boot ROM | `smaeul/sunxi-blobs` `42aabf4f2c93158a0cd88a000f9af56ce024882a`, `sun60iw2p1/fixs-brom` (binary SHA-256 `3a2a7d28…`) | Shared SRAM routing at reset |
| boot0, TF-A monitor, SCP firmware | orangepi-build `a95129bf…`, `external/packages/pack-uboot/sun60iw2/bin/` (`boot0_sdcard_orangepizero3w.fex` `829ea8e3…`, `monitor.fex` `8af16b86…`, `scp.fex` `07e6b976…`) | PLL setup, monitor literal scan, SCP domain loop |
| AI SDK user libraries | `ZIFENG278/ai-sdk` `fc90006d0f6569da2f6726c2d8395877686f5aca`, `libVIPhal.so` `3ed5357b…`, `libNBGlinker.so` `82f049b0…` | Whether user space touches hardware |
| A733 User Manual V1.00 | source lock `a733-user-manual-1.00` | Register defaults, memory map |

The vendor NPU kernel driver, its platform and power code, the clock driver
and the device tree were already cached for earlier NPU work; this analysis
added the shipped kernel configurations, the power-domain build rules and
the platform bus source. The A733 uses the Vivante VIPLite 2 driver with an
Allwinner platform layer, compatible string `allwinner,npu`
(`vip2/os/linux/platform/allwinner/vip_drv_device_platform.c:895`).
PCK-600 is the Arm power-control unit at `0x07060000` that switches the
A733 power domains; domain 4, at `0x07064000`, is the NPU's. FEL is the boot
ROM's USB loader mode, through which SunstoneOS images are uploaded and to
which they return.

These references are closed or unavailable:

- **VIP register specification.** Closed. Register meaning comes only from
  driver source.
- **A733 NPU low-power interface.** Not published. Without it, the drain and
  isolation contract of domain-4 power-off cannot be derived.
- **SYSCTRL `0x03000008` (U-Boot calls it `SRAM_CTRL_REG2`).** The manual
  does not document it. U-Boot source and the boot ROM are the only
  references.
- **boot0, SCP and TF-A monitor sources.** Closed; only binaries exist.
- **The writer that powers domain 4 at cold boot.** Unidentified. The board
  reads policy and status `0x8` (ON) before SunstoneOS writes anything.
- **A prebuilt `vipcore.ko`.** The AI SDK does not ship one; the kernel
  source above is the module source.

The user libraries reach hardware only through the kernel driver.
`libNBGlinker.so` imports no `viphal` register function, and the register
strings in `libVIPhal.so` name ioctl commands. They add no register sequence.

## The vendor build compiles out NPU power management

Both shipped board kernels set `CONFIG_AW_PCK600_DOMAINS=y` and leave
`CONFIG_AW_PM_DOMAINS` unset (`linux-sun60iw2-current-a733.config:892-893`,
`linux-sun60iw2-legacy-a733.config:892-893`). The NPU driver reads its
`power-domains` property only under `CONFIG_AW_PM_DOMAINS`
(`vip_drv_device_platform.c:1212-1214`), so `enable_pm` stays false. With
`enable_pm` false:

- probe calls `npu_clk_init()` directly (`vip_drv_device_platform.c:1332-1337`);
- `set_vip_power_clk()` does nothing for ON or OFF (`:423-439`), so the driver
  never calls `pm_runtime_get_sync()`, `pm_runtime_put()` or
  `npu_clk_uninit()`;
- the only power action left in `vipdrv_drv_device_init()` is a 2 ms sleep
  (`:1386-1387`; `vipdrv_os_delay()` takes milliseconds, `vip2/os/linux/vip_drv_os.c:506-517`).

The domain is still powered through the device core: the platform bus
attaches the NPU to its power domain with power-on before the driver's probe
runs (`drivers/base/platform.c:1399`, `drivers/base/power/domain.c:2774-2776`).
The clock-before-power branch of `set_vip_power_clk()`
(`vip_drv_device_platform.c:419-445`), which SunstoneOS cites as its order,
is therefore dead code on this board. The order that runs is power first,
then clocks and resets.

`CONFIG_NPU_SET_CLK_VOL` and `CONFIG_NPU_USER_IOMMU` are unset
(`:1052-1053` in both configurations), so the driver uses `set_clk_rate()`
and the NPU's own MMU. The driver is a module (`CONFIG_AW_NNA_VIP=m`).

The VIPLite defaults that apply are interrupt completion
(`vpmdENABLE_POLLING 0`, `vip2/inc/vip_lite_config.h:594`), the parallel
synchronous task manager (`vpmdENABLE_MULTIPLE_TASK 3`, `:419`), clock
suspend at idle (`:475`), a 10 s power-off timer (`:443`), user power
control and the Allwinner frequency hooks on this SoC (`:148-151`).

## Vendor sequence from cold boot to the first register access

### Boot firmware

1. **The ROM routes the shared SRAM to the CPU.** Before its boot dispatch,
   the boot ROM sets bit 1 of SYSCTRL `0x03000008`, then uses a stack at
   `0x000f3400` inside the shared SRAM (`fixs-brom` `3a2a7d28…`, ROM offsets
   `0x20c`–`0x220`; literals at `0x2d8` and `0x2dc`). The FEL scratchpad at
   `0x000ee800` also lies in that SRAM.
2. **boot0 sets bit 24 of every PLL control word.** The table at file offset
   `0x2490`–`0x24b4` lists ten CCU PLL words, including PLL_NPU at
   `0x020022a0`. The loop at load address `0x491d4`–`0x49220` (file offset
   `0x21d4`–`0x2220`) sets bit 24 of each (`boot0` `829ea8e3…`). SunstoneOS
   observes this bit as inherited (`docs/npu-pll.md`).
3. **U-Boot routes the shared SRAM to the NPU.** In normal boot and card
   product modes, U-Boot clears bit 1 of `0x03000008` and reads it back
   (`arch/arm/mach-sunxi/board_sun60iw2.c:119-133`, called from
   `board/sunxi/board.c:248-251`). Its comment reads "set sram to npu,
   default boot mode". USB product mode, which U-Boot uses after a USB load, skips this call.
4. **SCP firmware can request every domain ON.** A loop at `scp.fex`
   `07e6b976…` offsets `0x5ec6`–`0x5eec` ORs bit 3 into the policy word of
   domains 0–10 at `0x07060000 + index × 0x1000`. It then waits without a
   bound for status bit 3, except for domain 6. Its caller and entry
   conditions are unidentified.
5. **The TF-A monitor makes no NPU access.** `monitor.fex` `8af16b86…`
   contains none of the constants `0x03600000`, `0x07064000`, `0x07060000`,
   `0x02002b00`, `0x02002b04`, `0x020022a0`, `0x020025c0`, `0x020025e0` or
   `0x03000008`. The pinned TF-A source reports the PMIC as unsupported and
   has an empty power-down hook
   (`npu-low-power-source-20261024/source-search.json`).
6. **The PMIC rail.** DCDC2 supplies the NPU (`sun60i-a733-orangepi-zero3w.dts:2931-2934`).
   It is `regulator-boot-on` and `regulator-always-on`, 500–1,540 mV, with a
   ramp of 250 µV/µs and a 1,000 µs enable ramp (`:2123-2131`). DCDC2 also
   supplies `vdd-edp` (`:2843`).

### Kernel clock and power-domain registration

7. **Master gates on.** The NPU AHB master gate, CCU `+0x5c0` bit 6, and the
   NPU MBUS master gate, CCU `+0x5e0` bit 18, are `CLK_IS_CRITICAL`, so the
   clock framework enables them at registration. Each write carries a key
   field: `0x010000ff` for `+0x5c0` and `0x41055800` for `+0x5e0`
   (`ccu-sun60iw2.c:561`, `592-595`, `627`, `653-656`; key handling
   `ccu_gate.c:85-92`).
8. **PCK-600 registration.** For every listed domain, including domain 4,
   the driver writes the delay row: `+0x170 = 0x1f1f1f`, `+0x174 = 0x1f1f`,
   `+0xc00 = 0x08080808`, `+0xc04 = 0x808`, `+0xc10 = 0x8`
   (`pck600_domains.c:126-142`, called at `:269`; values `:552`). It
   registers each domain as already ON (`pm_genpd_init(…, false)`, `:283`),
   enables the PCK clock R_CCU `+0x1ac` (`:476`; clock `sun60iw2p1.dtsi:751`),
   and ignores OFF requests for 10 s (`:174-178`, `:488-490`). No reset
   applies on this SoC (`has_rst_clk = false`, `:570`).

### NPU driver probe

9. **Domain attach.** The platform bus attaches the NPU to domain 4 with
   power-on (`platform.c:1399`). The domain was registered ON, so the PCK-600
   driver issues no command (`pck600_domains.c:150-151`). Domain 4 stays as
   the boot firmware left it.
10. **Core reset released first.** Probe deasserts the core reset, CCU
    `+0xb04` bit 16, before any NPU clock is set
    (`vip_drv_device_platform.c:1285`).
11. **Operating point.** `npu-vfgear = <0>` selects 1,008 MHz
    (`sun60iw2p1.dtsi:2362`; `vip_drv_device_platform.c:655-686`).
    `check_freq_available()` keeps the highest table entry at or below that
    rate whose rounded rate is exact (`:703-734`): 1,008 MHz, listed at
    960 mV (`sun60iw2p1-cpu-vf.dtsi`, `opp-1008`). The device tree has no
    `npu-setvol`, so `set_vol` is 0 and the driver never sets the DCDC2
    voltage. It only enables the always-on regulator (`:131-156`, `:1316-1329`).
12. **PLL_NPU rate and parent.** `set_clk_rate()` sets the PLL_NPU rate, selects
    PLL_NPU as the module parent, then sets the module rate
    (`vip_drv_device_platform.c:226-259`). The PLL descriptor has
    `CLK_SET_RATE_UNGATE` (`ccu-sun60iw2.c:399-418`), so the clock core
    enables the PLL around the rate change. The enable helper closes the
    output gate (bit 27), enables the LDO (bit 30), the PLL (bit 31) and lock
    enable (bit 29), waits for LOCK, then opens the output gate
    (`ccu_gate.c:157-202`).
13. **Local clocks and resets** (`npu_clk_init()`, `vip_drv_device_platform.c:265-359`):
    1. AHB reset released, `+0xb04` bit 18 (`:268-273`);
    2. AXI reset released, bit 17 (`:275-280`);
    3. core reset released, bit 16 (`:282-287`);
    4. AHB master gate enabled, already on (`:290-295`);
    5. `set_clk_rate()` again (`:303`);
    6. MBUS master gate enabled, already on (`:308-313`);
    7. bus gate, `+0xb04` bit 0 (`:316-321`; `ccu-sun60iw2.c:1017-1019`);
    8. module gate, `+0xb00` bit 31, selector bits 24–26 at PLL_NPU and
       divider bits 0–4 at 1 (`:351-356`; `ccu-sun60iw2.c:1008-1015`).
       Enabling the module also enables its parent's output gate.
14. **Interrupt and register mapping.** Probe requests SPI 65 (GIC 97, level
    high, `sun60iw2p1.dtsi:2353`) and maps `0x03600000`, length `0x1000`
    (`vip2/os/linux/vip_drv_device_driver.c:927`, `:987`). The handler reads
    `+0x10` only after the mapping exists (`:378-382`).

The vendor never writes CCU `+0x1920` (PLL_NPU gate control). The
`pll-npu-auto` gate on its bit 0 has no consumer and no `CLK_IGNORE_UNUSED`
flag (`ccu-sun60iw2.c:2037-2039`). The kernel's unused-clock pass may
therefore clear bit 0 at late boot, which selects automatic gating; the
manual's default is `0x00010001` (§4.1.6.255).

### First register access and controller initialization

The first user-space open runs `do_init()` (`vip2/vip_drv_interface.c:81-250`):

15. **Power hook and 2 ms.** `vipdrv_os_init()` → `vipdrv_drv_init()` → the
    no-op power hook, then 2 ms (`vip_drv_os.c:131`,
    `vip_drv_device_driver.c:1322`, `vip_drv_device_platform.c:1386-1387`).
16. **First NPU register access** (`vip2/vip_drv_hardware.c:930-934`, through
    `vip_drv_context.c:385`). The driver writes `+0x000 = 0x00070900`, then
    reads `+0x020`, `+0x024`, `+0x030` and `+0x028`. The feature database
    entry for product `0x1000003b`, revision `0x9202`, gives page-descriptor
    MMU mode, 512 KiB of internal SRAM, no AXI SRAM and 32-bit virtual
    addresses (`vip2/inc/vip_feature_database.h:161-183`).
17. **Software reset** (`reset_vip()`, `vip_drv_hardware.c:104-235`). The
    driver writes `+0x000 = 0x70900`, then repeats a pass until two passes
    succeed, at most 10 tries:
    1. `+0x104 = 0`; `+0x10c = 0x015a0881`, then `0x015a0880`;
    2. `+0x000 = 0x70b00`, 5 µs, `+0x000 = 0x70900`, 20 µs;
    3. `+0x000 = 0xf0900` (isolate), `+0x3a8 = 1`, 20 µs, `+0x3a8 = 0`, 40 µs;
    4. `+0x000 = 0xf0900`, `+0x000 = 0x70900`, 50 µs;
    5. require `+0x004` bit 0, `+0x000` bits 16–17 and `+0x388` bit 0 clear.

    It then writes `+0x55c = 0x00ffffff`, clears `+0x090` bit 6 and requires
    `+0x004 = 0x7fffffff` (`:217-229`).
18. **Initialization command.** The driver builds the initialization stream
    in a read-only, contiguous, below-4-GiB buffer
    (`vip2/vip_drv_device.c:152-278`). It omits the AXI SRAM states
    `0x0e4f`/`0x0e50` because the AXI SRAM size is 0 (`:195-200`, `:245-257`;
    `vip_drv_device_platform_config.h:100`). The internal SRAM base is the
    virtual address `0x400000` (`vip2/memory/vip_drv_mmu.c:711`). The stream
    ends with END (`0x10000000`) and has no event.
19. **Internal frequency scale.** With user power control enabled, frequency
    scaling is supported (`vip_drv_context.c:429-435`). The driver then
    applies the scale through `vipdrv_set_internal_freq()`
    (`vip_drv_interface.c:210-212`, `vip_drv_device_platform.c:940-971`):
    1. save `+0x104` and write `0x00ffffff`;
    2. load scale 64 into `+0x000` bits 2–8 with a bit-9 pulse, then wait 1 µs;
    3. load scale 64 into `+0x10c` bits 1–7 with a bit-0 pulse;
    4. restore `+0x104` (`vip_drv_pm.c:107-136`).

    The resulting values equal those that the reset already wrote.
20. **Register initialization** (`init_vip()`, `vip_drv_hardware.c:442-490`):
    1. `+0x000 = 0x70900`, then `0x70100`; `+0x3a8 = 2`;
    2. `+0x03c = 0xffffffff`, then 0 (memory counters);
    3. `+0x100 = 0x140021` (bit 10 stays clear because the shader
       conformance feature is present);
    4. MMU in page-descriptor mode: `+0x3b4` = table base >> 12 << 4 | 1, then
       `+0x388 = 0x21` (`:255-268`);
    5. `+0x104 = 0`; `+0x10c` bit 16 clear and bit 17 set;
    6. `+0x090` bit 6 set (reorder bypass applies with no AXI SRAM, `:64-84`);
    7. `+0x414` bit 0 clear; read `+0x004`.
21. **Interrupts and first trigger** (`vip_drv_hardware.c:735-756`).
    1. `+0x014 = 0xffffffff` (`:509`); read `+0x388`; wait 10 µs.
    2. `vipdrv_hw_submit_initcmd()` links the MMU flush states (`0x08010061, 0xffffff7f`,
       semaphore and stall, `vip_drv_mmu.c:1154-1159`) ahead of the
       initialization stream.
    3. It triggers with `+0x654` = address, a barrier, then
       `+0x3a4 = 0x1ffff` (`:888-890`; fetch count `vip2/inc/vip_drv_share.h:137`).
    4. It polls `+0x004` bit 0 every 100 µs, at most 100 times, then waits
       10 µs (`:812-851`).

### Job, completion and shutdown

22. **Submit.** The power state machine enables the internal clock if it was
    suspended: `+0x000` bit 0 clear with a bit-9 pulse, 1 µs, then a
    `+0x10c` bit-0 pulse (`vip_drv_pm.c:191-209`). The driver prepends the
    MMU flush when the page table changed and triggers as above
    (`vip2/task/vip_drv_task_common.c:857-885`).
23. **Completion.** The interrupt handler reads `+0x010` and wakes the waiter
    (`vip_drv_device_driver.c:382-392`). The waiter treats the AXI and MMU
    error bits as faults and reads `+0x388` on an MMU fault
    (`vip_drv_task_common.c:1053-1075`, `:594-643`). It does not wait for
    full idle in interrupt mode.
24. **Idle.** The power state machine suspends the internal clock: `+0x000`
    bit 0 set with a bit-9 pulse, then 1 µs (`vip_drv_pm.c:157-168`). After 10 s
    idle the power-off action runs the no-op power hook
    (`vip_drv_pm.c:259-284`). The next job then repeats reset and
    initialization (`:286-325`).
25. **Cancel.** The driver sets `+0x3a8` bit 3, waits 100 µs, then clears the
    bit (`vip_drv_hardware.c:1136-1170`).
26. **Shutdown.** Module removal disables the always-on regulator, a no-op
    (`vip_drv_device_platform.c:1347-1366`). With `enable_pm` false, nothing
    gates the NPU clocks, asserts its resets or powers off domain 4.

### Accesses the vendor never makes

- It never writes PCK-600 domain 4 policy on this board; the domain stays as
  boot left it.
- It never writes CCU `+0x1920`, the PLL_NPU pattern and bias words
  `+0x2a4`–`+0x2b0`, the SRAM reset `+0xb04` bit 19, or the clock-management
  modes at `+0x1b10`–`+0x1b1c`.
- It never changes the DCDC2 voltage.
- It never disables the NPU MMU or clears `+0x014` at shutdown, and it never
  asserts NPU resets or closes NPU gates at runtime.
- It never reads the NPU window before step 15.
- TF-A makes no NPU call, and the driver makes no secure-monitor call on this
  SoC (`check_smc_set_freq()` for `CONFIG_ARCH_SUN60IW2`,
  `vip_drv_device_platform.c:654-686`, reads only the device tree).

## Divergences between SunstoneOS and the vendor sequence

Rows are ranked by the likelihood that the divergence stalls a bus access or
hangs the board. "Output" notes the separate risk to numerical results. The
SunstoneOS citations are in this repository at `bringup` `e55f64e0`.

| Rank | Reference step | Citation | Our step | Citation | Divergence class | Risk |
| ---: | --- | --- | --- | --- | --- | --- |
| 1 | U-Boot routes the shared SRAM to the NPU (SYSCTRL `0x03000008` bit 1 cleared); the ROM had set it | `board_sun60iw2.c:119-133`, `board.c:248-251`; ROM `3a2a7d28…` `0x210`–`0x220` | Never written. The NPU runs with the ROM's CPU routing under FEL | no reference in `drivers/`, `platform/` or `apps/` | Missing step | Hang: medium once fixed, because any CPU access to `0x74000`–`0xf3fff` while routed to the NPU, including FEL ROM return, has unknown behavior. The omission itself did not hang. Output: high; input-independent output fits a lost SRAM path |
| 2 | Domain 4 is ON (attach at probe) before resets are released and gates opened | `platform.c:1399`; `domain.c:2774-2776`; `vip_drv_device_platform.c:1212-1214`, `1332-1337` | Local open table, then the domain-4 ON request | `drivers/npu/a733_npu_startup.c:324-339`; comment `:43-48` | Order | Hang: medium if domain 4 reads OFF at entry, because resets are released into an unpowered domain; none for the observed inherited ON |
| 3 | No runtime gate closing or reset assertion | `vip_drv_device_platform.c:423-439` | Local stop closes module and bus gates and asserts AHB, AXI and core resets | `a733_npu_startup.c:56-59`, `367-379` | Extra step | Hang: medium-high for any later NPU window access (current code makes none); the stop itself ran once |
| 4 | Domain-4 delay row written at registration even when ON | `pck600_domains.c:126-142`, `269` | Adoption of an ON domain writes nothing; the row is written before the first command | `drivers/power/a733_power_domains.c:137-152`, `178-193` | Missing step | Hang: low; affects only a later transition |
| 5 | Shutdown leaves the MMU and interrupt mask as they are | `vip_drv_device_platform.c:1347-1366` | CLOSE writes `+0x014 = 0` and `+0x388 = 0` with readback | `drivers/npu/a733_npu_controller.c:256-269` | Extra step | Hang: low; ran once on idle hardware |
| 6 | PLL enabled with the old factors, then rate set; output gate opened with the module clock | `ccu_gate.c:157-202`; `vip_drv_device_platform.c:303`, `351-356` | Factors written before PLL enable (manual method 2); output gate opened before resets | `a733_npu_startup.c:38-42`, `49-53` | Order | Hang: low; the NPU module gate stays closed until the last step |
| 7 | Master gates `+0x5c0` bit 6 and `+0x5e0` bit 18 enabled with keys | `ccu-sun60iw2.c:592-595`, `653-656` | Checked set, refused if clear; not re-read at the final check | `a733_power_domains.c:55-60`, `404-415`; `a733_npu_startup.c:248-254` | Extra check | Hang: low; a refusal precedes every NPU access |
| 8 | Reset pass retried up to 10 times | `vip_drv_hardware.c:116-215` | First failed pass ends the attempt | `a733_npu_controller.c:95-119` | Missing step | Hang: low; failure is a refusal |
| 9 | Operating point 1,008 MHz; DCDC2 voltage untouched | `sun60iw2p1.dtsi:2362`; `vip_drv_device_platform.c:655-686`, `141-148` | 492 MHz (`0x00305100`); 800 mV verified with no write | `a733_npu_startup.c:14-24`; `a733_power_domains.c:234-238` | Value | Hang: low; slower clock at the table's 492 MHz voltage |
| 10 | `+0x014 = 0xffffffff`; interrupt completion | `vip_drv_hardware.c:509` | `+0x014 = 0xc0000001`; polled with GIC 97 masked; full idle required | `a733_npu_controller.c:32`, `152`, `192-238` | Value | Hang: low |
| 11 | Initialization stream omits AXI SRAM states and ends with END | `vip_drv_device.c:195-200`, `245-263` | Writes `0x0e4f`/`0x0e50` as 0, adds event 0 and a second END | `a733_npu_controller.c:283-287` | Extra step | Hang: low. Output: low |
| 12 | Internal frequency load writes | `vip_drv_pm.c:107-136` | Not performed; the reset writes the same scale | `a733_npu_controller.c:95-119` | Missing step | Hang: low; no net state change |
| 13 | Internal clock suspend at idle and resume before submit | `vip_drv_pm.c:157-209` | Not performed | `a733_npu_controller.c:158-168` | Missing step | Hang: low; power only |
| 14 | `+0x1920` never written; bit 0 may become automatic gating | `ccu-sun60iw2.c:2037-2039` | Bits 16 and 0 required set, written only if clear | `a733_npu_startup.c:38-39`, `328-329` | Value | Hang: low; the reset default matches SunstoneOS |
| 15 | Settle of 2 ms after power and clocks | `vip_drv_device_platform.c:1386-1387` | 2,000 µs settle | `a733_npu_startup.c:13`, `347-352` | None; the comment at `:348` wrongly says no source gives it | None |
| 16 | No NPU state observation before load | none | `npu-clock-words` reads the SID word and 26 CCU words, no NPU window | `tools/prepare-board-bundles.py:190-191`, `448-452` | Missing step (observation of SYSCTRL `0x03000008`, PCK domain-4 policy and status, R_CCU `+0x1ac`) | None; reads only |
| 17 | The vendor sequence | rows 1–14 | `npu-single-core`, `npu-service-os-image` and `accelerator-arbiter` run the startup and controller above | `tools/prepare-board-bundles.py:453-461`, `583-589`, `596-605` | Inherits rows 1–14 | As rows 1–3; the expected matching outputs are unlikely without row 1 |

The MMU format matches the vendor's: page-descriptor mode, `+0x3b4` encoding,
`+0x388 = 0x21`, flush states, the SRAM virtual window at `0x400000` kept out
of command addresses, and absent entries `2`
(`drivers/npu/a733_npu_mmu.c`, `a733_npu_controller.c:66-72`, `276-283`). The
identity read, reset values, `+0x100`, `+0x10c`, `+0x090`, `+0x414`, trigger
and cancel pulse match the cited vendor lines.

## Reads of the NPU register window

The vendor's first window access (step 16) follows this state, which matches
the condition under which GPU core reads hung when it was absent:

1. domain 4 policy and status ON;
2. PCK clock R_CCU `+0x1ac` bit 0 on;
3. AHB and MBUS master gates on;
4. PLL_NPU locked with its output gate open;
5. module gate on with PLL_NPU selected;
6. bus gate on, with AHB, AXI and core resets released and the SRAM reset
   released;
7. at least 2 ms elapsed since the power and clock hook.

SunstoneOS reaches READY, the startup sequencer's final state, only after a final readback of items 1, 4, 5 and 6
and the 2,000 µs settle (`a733_npu_startup.c:248-254`, `340-360`). It checks
items 2 and 3 earlier, at the register pass (`a733_power_domains.c:404-415`).
The service activates, and the controller makes its first window access,
only after `a733_npu_startup_prepared()` returns 1 (`a733_npu_startup.c:380-391`;
`drivers/npu/npu_service.c:102`).

The NPU register adapter does not itself require READY. It checks only its
own initialization and mapping (`platform/a733/npu_native.c:7-8`, `94-98`).
After local stop, the resets are asserted again, so any later window access
would read a held engine. No current path makes such an access. The entry
checkpoint check refuses NPU window, CCU and domain-4 addresses
(`apps/npu-finite/checkpoint-check.py:58`), and no host tool reads
`0x03600000` over FEL.

## Recommended fixes

Each fix follows a cited reference step; none is implemented here.

1. **Route the shared SRAM to the NPU for the NPU lifetime.** After READY and
   before the first window access, clear bit 1 of `0x03000008` and read it
   back, as `board_sun60iw2.c:124-132` does. Keep it clear through CLOSE.
   Set it again, with readback, before any CPU access to `0x74000`–`0xf3fff`
   and before ROM return, because the ROM uses that range
   (`0x000ee800`, `0x000f3400`). Bind the write to the NPU's SRAM ownership
   grant. Keep the model's SRAM footprint below `0x7a800` bytes so that the
   FEL scratchpad stays outside it. The manual does not define this bit, so
   the first run must also record the readback values.
2. **Power before clocks.** Request domain 4 ON, or adopt it ON, before the
   local open table, matching the board's effective vendor order. Correct
   the comments at `a733_npu_startup.c:43-48` and `:348` and the order
   statements in [npu-platform.md](npu-platform.md) and
   [npu-startup.md](npu-startup.md). The inherited-ON path is unchanged.
3. **Interlock the NPU register adapter on startup state.** Refuse window
   access unless the bound startup is READY and not STOPPED. This makes the
   precondition list above a code check instead of a calling convention.
4. **Re-read the master gates and the PCK clock at the final check.** Add CCU
   `+0x5c0` bit 6, `+0x5e0` bit 18 and R_CCU `+0x1ac` bit 0 to `opened()`.
5. **Extend the `npu-clock-words` row.** Add read-only words SYSCTRL
   `0x03000008`, PCK `0x07064000` and `0x07064008`, and R_CCU `0x070101ac`.
6. **Leave the rest as they are for the first run.** Rows 4–14 carry low hang
   risk. Matching the vendor's interrupt mask, initialization stream, retry
   count and clock suspend can follow a passing numerical run.

## First NPU board run and predicted outcome

The board must first be power-cycled: the earlier run left PLL_NPU enabled,
and the register pass refuses an enabled PLL.

1. **Read-only state row.** Run `npu-clock-words` with fix 5. Predicted
   values on a freshly powered board entered through FEL:
   - SYSCTRL `0x03000008` bit 1 set (ROM `0x210`–`0x220`);
   - domain-4 policy and status `0x8`;
   - CCU `+0xb04 = 0x00080000` and `+0xb00` bit 31 clear;
   - PLL_NPU `0x41104500` (LDO and bit 24 set, PLL disabled);
   - `+0x1920 = 0x00010001`;
   - `+0x5c0` bit 6 and `+0x5e0` bit 18 set.

   The row makes no NPU window access and carries no hang risk beyond the
   existing CCU reads.
2. **Numerical image with the SRAM routing fix.** Use the sequence of the
   numerical image that ran on port 3-1 (`9e4adabf…`), unchanged apart from
   fixes 1 and 3. Predicted outcome:
   - startup reaches READY, OPEN and both jobs complete, as in the earlier run;
   - if SRAM routing caused the mismatch, both outputs match the reference;
   - the two outputs then differ from each other wherever their inputs differ;
   - CLOSE and local stop pass, the routing bit reads back set, and the board
     returns to a responsive ROM.

   If the outputs still mismatch, SRAM routing is excluded as the cause. The
   reviewed stopped-memory capture, with the bit restored first, is then the
   next observation. The principal new hang risk is the routing write
   itself, so the run must stop at a failed readback and retain the board
   for whole-board power removal.

## Interrupt, idle-gating and combined-image paths (re-derived 2026-11-01)

This section re-derives the paths that changed after the cold-boot analysis
above: the GIC configuration of every interrupt the NPU paths use, the bind
refusal of the combined image, idle gating, the finite shared-timer
composition and the OS service start and stop. It made no board access. The
sources are open ones only: the vendor NPU driver (`vip2/…`), the mainline
Linux `irq-gic-v3.c` and `irq-gic-common.c` pinned at `e5f0a698` (the latter from the 2026-10-25 evidence copy; the pinned tree holds only `irq-gic-v3.c`), TF-A 2.5
(`drivers/arm/gic/v3/`), the vendor device tree and the GICv3 architecture
specification IHI0069D. No firmware blob was disassembled and no blob content
is quoted. The closed TF-A monitor of the vendor image is therefore not
described; the open TF-A and Linux drivers define the reference.

### GIC configuration diff

**Vendor sequence for a Nonsecure Group 1 SPI (Linux on TF-A).** TF-A
`gicv3_distif_init` clears the three group-enable bits, sets ARE_S and ARE_NS,
applies the SPI defaults, then enables the Secure groups
(`gicv3_main.c:187-218`); it never configures Nonsecure SPIs individually.
Linux `gic_dist_init` writes `GICD_CTLR = 0`, waits for RWP, writes
`IGROUPR = ~0` for every SPI bank, applies `gic_dist_config` (all SPIs level,
default priority, disabled and inactive) and then enables ARE_NS, G1A and G1
(`irq-gic-v3.c:972-1020`; `irq-gic-common.c:91-115`). A driver request then
runs three steps in this order: `gic_set_type` writes `GICD_ICFGR` through
`gic_configure_irq` (`irq-gic-v3.c:750-780`; `irq-gic-common.c:48-88`),
`gic_set_affinity` writes `GICD_IROUTER` with the interrupt masked
(`irq-gic-v3.c:1477-1515`), and `gic_unmask_irq` writes `GICD_ISENABLER`
(`irq-gic-v3.c:510-513`). Linux never writes the priority of an individual
SPI and never reads back group, priority or route. The NPU node requests SPI 65,
level high, INTID 97 (`sun60iw2p1.dtsi:2353`; `vip_drv_device_driver.c:956`,
after the clocks and resets of the platform probe). The virtual timer PPI is
`GIC_PPI 11`, INTID 27, level (`sun60iw2p1.dtsi:396-402`); Linux tolerates a
PPI whose `ICFGR` write does not stick (`irq-gic-v3.c:772-777`).

**Priority views.** With `GICD_CTLR.DS = 0` and `SCR_EL3.FIQ = 1`, a Nonsecure
write of a Group 1 priority or of `ICC_PMR_EL1` stores `(value >> 1) | 0x80`,
and a Nonsecure read returns `(stored << 1) & 0xff`
(`irq-gic-v3.c:92-110`, `:190-218`; IHI0069D 4.8.1). A Secure access, such as the
EL3 route installer, reads and writes the stored value. Priority 0xd8 written
at EL3 therefore reads 0xb0 at Nonsecure EL1, and the PMR value 0xb0 written at
Nonsecure EL1 is stored as 0xd8.

**SunstoneOS sequence.**

| Item | Where | Sequence | Match |
| --- | --- | --- | --- |
| INTID 97, OS and board images | `platform/a733-smp-cached/irq_table.c:53-91`; `tools/sunstone-manifest.py:346` | At EL3 with the groups off: IGROUPR bit set, IGRPMODR bit clear, ICFGR level, IPRIORITYR 0xd8 (Secure view), IROUTER core 1; one barrier; read back all; refuse when the source is enabled, pending or active; enable stays with the service | Same set of fields as Linux. The order differs: priority is written (Linux relies on its default) and the group is written (Linux writes it in bulk) |
| INTID 97, finite image | `apps/npu-interrupt/irq-route.c:60-73` | Same order at EL3, route 0, priority 0xd8 | As above |
| INTID 97 bind | `platform/a733/npu_irq.c` `bind_refusal` | At Nonsecure EL1, before the scheduler: checks listed below | Linux has no analogue |
| INTID 97 enable | `npu_irq.c` `rearm` through `gicv3_spi_rearm` | ISENABLER after the device mask is armed | Same last step as Linux |
| PPI 27, OS image | `platform/a733-smp-cached/continue.c:112-125`; `platform.c:121` | EL3 reads IGROUPR0 (group 1 set) and IGRPMODR0 (clear), clears the ICFGR1 edge bit and reads it back; EL1 then writes priority byte 0xd8 and ISENABLER0 | Group by the base monitor, as for Linux's bulk write; priority written at Nonsecure EL1, stored as 0xec |
| PPI 27, idle benchmark | `apps/npu-finite/monitor.c:49-79` | EL3: captures priorities, base init, clears ICFGR edge bit, writes Secure priority 0xd8, waits for RWP, reads back | Matches the Secure view |
| PPI 27, setup check | `apps/npu-finite/deadline.c` | EL1 reads the priority byte and requires the Nonsecure view 0xb0 | Matches the view, fixed in 5dd7cb4b |
| SGI | `platform/a733-smp-cached/platform.c:191` | The FreeRTOS cross-core yield; no NPU code configures it | Not an NPU path. Its priority is set by the port and is not derived here |

**Answer on the API mask.** The three views agree. The platform builds with
`SUNSTONE_SECURE_MONITOR`, so `configUNIQUE_INTERRUPT_PRIORITIES` is 16,
`portPRIORITY_SHIFT` is 4 and `configMAX_API_CALL_INTERRUPT_PRIORITY` is 11
(`include/FreeRTOSConfig.h:63-66`; FreeRTOS `portmacro.h:183`). The port writes
`11 << 4 = 0xb0` to `ICC_PMR_EL1` at Nonsecure EL1 (`patches/freertos-smp-c.patch:109`;
`freertos-smp-asm.patch:176`). The monitor sets `SCR_EL3.FIQ = 1`:
`scr_el3 = 0x505` at `platform/a733/monitor.c:124-125` and `0x535` at
`platform/a733-smp-cached/continue.S:98`, and initializes the Secure PMR to 0xff
so the Nonsecure PMR write takes effect (`continue.S:82-86`). The PMR is thus
stored as `(0xb0 >> 1) | 0x80 = 0xd8`. The GIC signals an interrupt only when its
stored priority is numerically below the stored PMR. The stored priority of
INTID 97 is 0xd8 (route table) and of PPI 27 is 0xd8 (EL3) or 0xec (EL1 write of
0xd8 at `platform.c:121`). None is below 0xd8, so each is masked whenever the
kernel raises the PMR to the API mask and sits at or below the mask: the
handlers may call FreeRTOS `FromISR` functions. The comparison is meaningful
only in one view. The image-level assertions
now state it: `apps/os/npu-irq.c` and `apps/npu-board/irq.c` assert
`((mask << 4) >> 1 | 0x80) <= 0xd8`, the Secure-view comparison.

**One mixed-view comparison remains outside the NPU paths.** The route table
requires `priority >= A733_IRQ_TABLE_MIN_PRIORITY = 0xb0`
(`platform/a733-smp-cached/irq_table.h:25`), which compares a Secure-view value
with the Nonsecure-view mask. A route priority between 0xb0 and 0xd7 passes the
check but is stored above the stored PMR 0xd8, so its handler preempts API
critical sections. The generated table holds the USB host route at 0xb0
(`tools/sunstone-manifest.py:345`) under `SUNSTONE_BOARD_USB2_IRQ`. Whether that
handler calls `FromISR` functions is not derived here; the minimum should read 0xd8
for the Secure-view table. This change is left for the owner of the USB route.

### Bind refusal of the combined image

The per-clause failure record (`0xb01`–`0xb09`) did not exist when the refusal
was recorded, so that run holds only the call site (`apps/os/npu.c:434`) and
value 0. `bind_refusal` evaluates in this order, with the vendor comparison:

| Clause | Condition that refuses | State at that point | Vendor |
| --- | --- | --- | --- |
| 1 | not EL1, already bound or faulted | EL1, fresh state | Not applicable |
| 2 | scheduler started | `board_application_start` runs before the scheduler (`apps/os/start.c:29-33`) | Not applicable; Linux requests the IRQ in probe |
| 3 | distributor write pending | no write since the installer's RWP wait (`irq_table.c:90`) | Linux waits RWP after distributor writes (`irq-gic-v3.c:1018`) |
| 4 | source enabled | common boot cleared every SPI (`platform/a733/monitor.c:96-99`) | Linux disables all SPIs (`irq-gic-common.c:91-115`) |
| 5 | source pending | the installer refused otherwise (`irq_table.c:58-64`, `:84-88`); the NPU has not started | Satisfied: the vendor requests the IRQ with the device mask at reset |
| 6 | source active | as for 5 | Satisfied |
| 7 | ICFGR not level | installer wrote and verified level | Satisfied: level high |
| 8 | priority byte differs from the expected value | installer wrote Secure 0xd8; Nonsecure read returns 0xb0 | Linux writes 0xa0 in the Nonsecure view and reads nothing back |
| 9 | IROUTER differs from core 1 | installer wrote core 1 and verified it | Linux writes IROUTER and reads nothing back |

**Prediction.** Clause 8 refused. Before the fix the expected value was
`NPU_IRQ_PRIORITY = 0xd8`, the Secure-view number, compared with the Nonsecure
read, which the architecture and the PPI 27 board read (0xb0)
both fix at 0xb0. Clauses 3 to 7 and 9 read values that
the EL3 installer verified in the same boot, so they are not predicted to
refuse. The finite single-core image passed this clause because its default
expectation 0xb0 is already the Nonsecure view, and the host fixtures for
the board image used `0xd8 << 8` for the read, which repeated the assumption
instead of testing it. The expectation is now the Nonsecure view of the
route priority (`NPU_IRQ_NS_PRIORITY` in `platform/a733/npu_irq.c`), and the
board provider fixture models the read as `0xb0 << 8`. The next run's record
discriminates as follows: `0xb08` confirms a remaining view error, `0xb09`
shows a route readback different from core 1 (`0x100`), and `0xb05` or `0xb06`
shows a device-originated or inherited interrupt state that the installer did
not see.

### Idle gating

**Vendor sequence.** Clock suspend runs when the power state leaves the CLOCK
states for IDLE (`vip_drv_pm.c:455-467`, `:560-575`): first
`vipdrv_pm_set_frequency(1)` (`:66-139`: save `+0x104`, write `0x00ffffff`, set
scale 1 in `+0x000` bits 8:2 with a bit-9 pulse, 1 µs, scale the shader through
`+0x10c`, restore `+0x104`), then `vipdrv_pm_disable_clock` (`:142-175`):
read `+0x000`, set bit 0 and bit 9, write, clear bit 9, write, 1 µs. Resume
(`power_clock_state_in`, `:438-452`; `vipdrv_pm_enable_clock`, `:177-210`) reads
`+0x000`, clears bit 0, sets bit 9, writes, clears bit 9, writes, 1 µs, reads
`+0x10c`, pulses bit 0 and clears it, then sets the frequency again. The vendor
reads `+0x004` only in `vipdrv_hw_wait_idle`, with a mask compare against
`0x7fffffff` (`vip_drv_hardware.c:1045-1076`; `vip_drv_hardware.h:69`), never in
the suspend or resume path. The only timer is the 10 s power-off timer
(`vip_lite_config.h:443`); the clock gate follows the transition to idle.

| Divergence | Ours | Can refuse on hardware |
| --- | --- | --- |
| Gate hysteresis | 10 ms (`a733_npu_idle.h`) | No. A design choice, not a vendor value |
| Scale 1 before the gate | Not written; the scaler stays at 0x100 and 0x80 | No |
| Gate requires `+0x000` bits 0 and 9 clear and scaler field 0x100 | `npu_idle.c` `npu_idle_gate` | Yes. Evidence: startup writes `0x70900` (step 17 above), whose masked field is 0x100; unproven after a completed job |
| Gate requires `+0x10c` bit 0 clear and field 0x80 | same | Yes. Startup writes `0x015a0880`; unproven after a job |
| Idle word compared by equality with `0x7fffffff` before the gate and after resume | `npu_idle.c` | Yes. Vendor masks with `0x7fffffff`, so bit 31 is ignored; equality refuses if bit 31 is set. Board evidence: the numerical run completed with the exact value, after jobs only |
| Resume requires `+0x000` equal to the gated value, and the second read equal to the pre-gate value | `npu_idle_resume` | Yes. The vendor read-modify-writes whatever it reads |
| Resume requires `+0x10c` equal to the pre-gate value | same | Yes, same reason |
| Idle word read after resume | same | Yes. Not read by the vendor; expected full idle with no work |
| Resume budget 100 µs, one-microsecond delay per pulse | `settle` | Yes, in strict mode. The vendor uses 1 µs and has no budget |

No gated-state read other than `+0x000` occurs, as in the vendor. The
divergences that can refuse on hardware are changed as follows. The idle word
is compared under the mask `0x7fffffff`. The scaler fields, the pulse and gate
bits before the gate, and the resume readbacks of `+0x000`, `+0x10c` and the
idle word are recorded and do not refuse; the resume writes back the value that
it read with the gate and pulse bits cleared. A gate bit still set after the
ungate pulse refuses, because the clocks would stay off. The 100 µs budget is a
measured value with its limit in the benchmark images. The gate, the pulses and
the resume have not run on hardware; the first run of the control and gating
images tests them.

### Finite shared-timer composition

The vendor has no composition; the reference is the Linux timer node, which
owns INTID 27 as a level interrupt, and the EL3 and EL1 views above. The
idle-benchmark monitor (`monitor.c:49-79`) requires CNTV disabled and PPI 27
disabled, not pending and not active at entry, clears the CNTV enable, clears
the edge bit, writes the Secure priority 0xd8 and requires the read back.
The following refuse on hardware and depend on inherited state: CNTV enabled
(`:59-60`), PPI 27 enabled, pending or active (`:61-62`). Both are entry states
that a prior stage could leave; the project rule is to establish them rather
than refuse. The smoke entry reached the EL1 priority assertion, so the base
monitor and the readback at `:74` passed on the board with the Secure view 0xd8.
The return path restores the saved priority word and compares it
(`return_gic` in `monitor.c`, idle-bench branch), and the receipt checks at
SMC 0x733 (`monitor.c:198-228`) form the refusals listed under assumptions.

### OS NPU service start and stop

The vendor has neither phase: the module requests the IRQ after probing the
clocks and resets, and nothing gates or tears down at runtime. SunstoneOS
binds INTID 97 in `os_npu_start` (`apps/os/npu.c:364-437`) before the startup
sequencer has run (the bootstrap task polls it later), so the bind sees the
installer's state and not the device's. Divergences: the bind reads and
requires route state the vendor never reads (clauses above); the stop path
masks, clears the pending bit of INTID 97 and requires enabled, pending and
active clear (`npu_irq.c` `stop`), which the vendor never does; `restart`
requires a stopped, masked, fault-free binding and clean GIC state. These
refuse only on a device-originated interrupt left pending, which CLOSE's full
idle proof precedes.

### Service reply protocol and the combined-image run

The service replies to a request with a result, the controller fault and a
state. Clients treat a state as final only when it is the one they wait for.
Two modes exist.

- **Exclusive mode** (the finite, convolution and earlier runtime images).
  OPEN replies as soon as the controller bootstrap starts, with state
  BOOTING (`drivers/npu/npu_service.c:323-345`). WAIT holds its reply while the
  controller is active and answers once it is READY, or with a fault
  (`npu_service.c:356-357`, `:476-480`). The clients that follow this are
  `apps/npu-finite/startup.c:174-175`, `apps/npu-convolution/main.c:93-94` and
  `apps/npu-runtime/main.c:39-41`: they send WAIT after a BOOTING reply.
- **Runtime mode** (the OS service and `apps/npu-board`). OPEN, STATUS and CLOSE
  go through the shared member logic (`npu_service.c:509-529`, `:651-657`).
  The reply state is the member view (`:482-490`, `:583`): BOOTING while the
  controller bootstrap runs, READY afterwards. The runtime accepts no WAIT:
  `npu_runtime_send` refuses every operation except OPEN, STATUS, CLOSE, LOAD
  and SUBMIT (`drivers/npu/a733_npu_runtime.c:893-908`). LOAD and SUBMIT are
  accepted at once and their reply is held until the work completes: LOAD
  with state READY (`a733_npu_runtime.c:401`), SUBMIT with state DONE
  (`:599`). A SUBMIT starts only while the controller is READY (`:642-649`),
  so it waits for the bootstrap. `apps/npu-board/client.c:84-86` takes the
  OPEN reply as it comes and issues LOAD next.

The first combined-image run that passed the bind (queue entry 7c8b2818)
refused the OPEN reply because `call` in `apps/os/npu.c` required READY. The
reply state was BOOTING (1). WAIT is not an option in runtime mode, so the
wrapper now accepts BOOTING or READY for OPEN and nothing else
(`apps/os/npu.c:255-261`). The other replies the wrapper requires match the
service: LOAD READY, SUBMIT DONE, CLOSE CLOSED, each with result OK and no
fault. `checks/os-npu-first-failure.c` cases 20 and 21 run the four
operations through the linked wrapper with those reply states and refuse an
OPEN reply of RUNNING.

**Prediction for the next combined-image run.** The bind passes as before and
the sequence continues in this order. Each stage names the record that
identifies a refusal. A refusal carries the group `npu`, the site
`apps/os/npu.c` line, the request operation and the observed word (the
decoded `board_failure` of the result, as in the entry 7c8b2818 record).

1. OPEN returns BOOTING or READY. A refusal shows request operation OPEN with
   observed state other than 1 or 2 (line 261), or a result or fault value
   (lines 253-254).
2. LOAD of model 0 returns READY. A refusal shows request operation LOAD
   and the observed word is the state, result or fault.
3. The first job: SUBMIT returns DONE after the bootstrap and the job complete.
   A refusal shows request operation SUBMIT. A fault value 6
   (`A733_NPU_TIMEOUT`) with the 1 s job timeout, or a ticket or collect
   refusal at lines 251-252, means the NPU interrupt did not complete the
   job. Four such jobs run while the eight CPU workers run.
4. The overlap with the CPU workers is not a service reply. A different
   output is reported in the receipt (outputs match, overlap mask 15, eight
   CPU digests); the failure record's stage names a CPU-side refusal.
5. CLOSE returns CLOSED. A refusal shows request operation CLOSE with state
   BUSY (result 3) if the controller is not READY, or observed state other
   than CLOSED. The bootstrap task then stops the platform; a retained stop is
   recorded as operation CLOSE with the expected value `A733_NPU_PLATFORM_READY`
   (line 342), and the local stop and normal return follow.

A normal return with the receipt of `expect.json` ends the run.

### Assumptions that need hardware state

| Assertion or refusal | Value assumed | Evidence |
| --- | --- | --- |
| Bind clause 8 value | Nonsecure read 0xb0 | Architecture and the PPI 27 board read; the INTID 97 read itself is unproven until the next run |
| Bind clause 9 | Nonsecure read of IROUTER returns the stored route | IHI0069D permits it for Nonsecure Group 1; unproven on this GIC |
| Bind clause 7, ICFGR readable at Nonsecure EL1 | level bits read as written | Same reasoning; unproven |
| Bind clauses 4 to 6 | Nonsecure read of ISENABLER, ISPENDR, ISACTIVER shows Group 1 state | Standard; unproven for 97 specifically |
| Distributor `DS = 0` and `SCR_EL3.FIQ = 1` | Priority halving applies | Board read 0xb0 for stored 0xd8 on PPI 27 |
| PPI 27 ICFGR edge bit clear after the write | Level or writable | The idle-benchmark monitor passed its readback on the board (`:74`) |
| PPI 27 priority stored 0xec in the OS image | EL1 write is stored shifted | Architecture; no readback in the OS image |
| CNTV disabled and PPI 27 quiet at entry | Inherited state | Unproven; refusal at `monitor.c:59-62` |
| Idle word 0x7fffffff, bit 31 clear | Full idle | Numerical board run; not after gate and resume |
| Scaler fields 0x100 and 0x80 at idle | Startup values persist | Vendor init writes them; unproven after jobs |
| SMC 0x733 receipt conditions | Status, receipt fields, deadline and `control & 0x1005` | The smoke failure recorded stop 6 for operation 0x733 (`monitor.c:198-228` holds every stop-6 condition). The failing priority assertion at EL1 precedes the return, which would carry a nonzero status without a complete receipt, but the record does not name the condition: unproven |

### Predictions

- **Next combined-image run.** With the Nonsecure-view expectation the bind
  passes clauses 1 to 9 and `os_npu_irq_bind` returns 1. The first possible
  later refusal is the service activation, which this derivation does not
  change. If the bind still refuses, the record value names the clause:
  `0xb08` would mean the Nonsecure priority read is not the shifted Secure
  value, in which case the PPI 27 board read does not transfer to SPIs.
- **Next idle-gating run** (control, then gating, on port 3-3 at 852 MHz and
  800 mV; no rail, PCK4, PLL or voltage change). The gate and resume follow the
  vendor's writes and delays (this section, "Idle gating"). The control makes
  every decision of the gating image and writes no register. The checks added
  by SunstoneOS that could refuse are reduced to: the idle word under the mask
  `0x7fffffff` before the gate (a unit not idle), a gate bit still set in
  `+0x000` after the ungate pulse, a settle wait beyond its 1 ms bound, and a
  failed register access or lifetime check. Every other readback is recorded
  and does not refuse, and the 100 µs budget is a measured value. Prediction:
  the gating image returns normally with output matching in every window and
  cold group; 30 cycles after the 100 ms gap and 100 after the 11 ms gap gate
  and resume, none after the 9 ms gap; `register_values.deviation_mask` is 0
  (the values of `+0x000` and `+0x10c` equal those written at startup, and the
  idle word has bit 31 clear or ignored); the largest resume is near the 1 µs
  settle plus register latency, below 100 µs. A nonzero deviation mask names
  which value differed (`1` and `2` before the gate, `4`, `8`, `16`, `32` at
  resume entry, clock, shader and idle word) and carries the words. A resume
  above 100 µs is shown by `resume_budget.within_limit` false and fails only the
  comparison. A retained failure shows `NPU_FAILURE` with the site: `FULL_IDLE`
  for the idle mask, `GATE_VALUE` for a gate bit still set, `SETTLE` for the
  wait. The PPI 27 composition refuses only on inherited CNTV or PPI 27 state,
  and the setup assertion expects 0xb0.
