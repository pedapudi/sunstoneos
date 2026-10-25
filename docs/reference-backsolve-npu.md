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
