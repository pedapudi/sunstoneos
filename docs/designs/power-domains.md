# Design: one power-domain owner and one PMIC service task

Status: **design, not implemented**. It completes the planned parts of
[power-authority.md](power-authority.md), whose rail ledger, state word and
lock-free PMIC time reads exist. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). Baselines
were counted on commit `b3b1289` with the host models in the budget table; no
figure comes from the board.

## Ownership and data flow

A PCK600 power domain is one 4 KiB block at `0x07060000 + 0x1000 × domain`.
Six drivers write the same domain sequence: the GPU power controller
(domain 5), the DE_SYS provider of the framebuffer and G2D (domain 1), the NPU
platform (domain 4), the video platform (domains 2 and 3), HDMI and DisplayPort
(domain 10). The accelerator image links the GPU and NPU copies; the GPU
display image links the GPU, framebuffer and HDMI copies. Each copy reads its
own supply through the image's per-tick PMIC loop.

- **Domain owner.** `drivers/power/a733_power_domains.c` replaces
  `a733_de_sys_power.c` and becomes the only writer of domains 1 and 5;
  domain 4 joins in the NPU stage. A tree check fails if another file writes a
  block the owner holds.
- **Rows.** A consumer registers one static row before the scheduler starts:
  domain, rail token and wake semaphore. This package's rows are the
  framebuffer and G2D (domain 1) and the GPU (domain 5). A domain is ON while
  an admitted row requests it. Only domain 5, whose one bus master is the GPU,
  is ever powered off.
- **Service task.** One task per image runs every PMIC transaction and every
  domain transition. Each image keeps its PMIC task with its liveness and
  progress hooks. The loop body becomes `a733_power_service_step()`, which
  advances all queued work and returns the ticks to block in
  `ulTaskNotifyTake(pdTRUE, ticks)`.
- **Row request.** The consumer stores ON in its row with release order and
  notifies the service. The service reads the domain-level registers, writes
  the delay row and the ON command if the domain is OFF, and spins on status.
  It then publishes the row state with release order and gives the row's
  semaphore. The consumer blocks on that semaphore until its deadline.
- **PMIC request.** `a733_pmic_submit` keeps its interface and calls a kick
  hook that notifies the service. The hook is set through its own setter,
  because checks outside this package build `struct a733_pmic_ops` with
  positional initializers. When a slot leaves ACTIVE, the service gives that
  slot's semaphore; `a733_power_wait()` takes it, bounded by the request
  deadline, and then calls `a733_pmic_result`.
- **Supply verification once per boot.** At provider start plus 5,160 µs (a
  1,000 µs enable delay and a 1,040 mV ramp at 250 µV/µs, board device tree
  lines 2123–2146), the service snapshots DCDC2 and DCDC4. A rail enabled at
  800 mV sets its verified bit in the state word; then the done bit is set.
  A row is admitted only with its supply's verified bit: DCDC2 for domains 1
  and 4 (`npu-supply`, line 2932) and DCDC4 for domain 5 (`gpu-supply`,
  line 3421). The GPU's PMIC consumer then completes from that record with no
  bus transaction. The ledger accepts selector writes to DCDC3 and DCDC5 only;
  firmware writes to DCDC2 and DCDC4 after boot are excluded only by board
  evidence.
- **One PPU clock lease.** The owner holds one stable lease of the PPU clock
  (R_CCU `0x1ac` bit 0) through the GPU resource manager until power-off.

## Interrupts and polling

- **PCK600 acknowledgement is polled.** The pinned `pck-600@7060000` node has
  no `interrupts` property (`sun60iw2p1.dtsi` lines 745–753). The PPU has
  `PPU_IMR` (`0x030`) and `PPU_ISR` (`0x038`) in the PCK-600 TRM
  (101150_0005_01) register summary, but their GIC wiring is unknown. The
  service spins up to a provisional 50 µs after the command, then blocks one
  tick per retry until FAILED at 10 ms, the vendor bound
  (`pck600_domains.c` lines 112–121). Section 1.3 permits this tick wait for a
  device without qualified interrupt delivery.
- **PMIC byte phases spin.** S_TWI0 runs at 400 kHz (board device tree lines
  1946–1955), 22.5 µs per byte. Its interrupt, GIC SPI 203 (`sun60iw2p1.dtsi`
  line 1962), is unqualified. The service calls `a733_pmic_poll` back to back
  within a request and reads the raw counter between calls.
- **Requesters never poll.** A semaphore per row and per slot carries
  completion. Task notifications would need a second index, because interrupt
  bindings use index 0 and `configTASK_NOTIFICATION_ARRAY_ENTRIES` is 1. A
  stale give from a reaped request causes one extra wake; each wait rechecks
  the published state.

## Memory placement and cache policy

Owner and service state is static in kernel `.bss`, Normal write-back memory:
the domain table, eight slot semaphores and the rows in consumer storage. The
PCK600, CCU and R_CCU windows are Device-nGnRnE and are verified once at
service start with `a733_mapping_check.h`. No buffer is shared with a device,
so no cache maintenance occurs. Transitions occur a few times per boot, so no
field gets its own cache line.

## Synchronization across cores

- Each field has one writer. The consumer writes its row's request word; the
  service writes domain state, row state and verified bits. Readers make one
  acquire load.
- Only the service task touches the PCK600 blocks it owns, so a transition
  takes no owner lock and no platform register lock. The CCU and R_CCU
  accesses are reads.
- PMIC slots keep the provider's try-lock; a slot's semaphore is given only
  after the provider leaves its object.
- The service runs at priority 3, below USB0, audio and thermal, on the core
  its image already uses for the PMIC task. A PMIC request spins at most
  0.8 ms and a transition 50 µs; higher-priority tasks preempt both.

## Register sequences and sources

Sources are pinned in `sources.lock.json`: `pck600_domains.c` (SHA-256
`2d9c2b60…`), `ccu-sun60iw2.c` (`1d32bb85…`), `ccu-sun60iw2-r.c` (`9285e1b3…`),
`sun60iw2p1.dtsi` (`1c7313f6…`), `sun60i-a733-orangepi-zero3w.dts`
(`31145367…`) and `vip_drv_device_platform.c` (`a47f85f2…`).

| Step | Registers | Source | Status |
| --- | --- | --- | --- |
| Domain-level checks | CCU `0x5c0` and `0x5e0` bits 5 and 11 (domain 1), 6 and 18 (domain 4), 7 and 16 (domain 5); R_CCU `0x1ac` bit 0 | `ccu-sun60iw2.c` lines 587–601 and 653–677; `ccu-sun60iw2-r.c` lines 144–146 | Host model |
| Adoption | Policy `+0x0` and status `+0x8` bits 3:0 both 0 or both 8 | `pck600_domains.c` lines 87–101; TRM `PPU_PWPR`, `PPU_PWSR` | Host model |
| Delay row, each read back | `+0x170` `0x1f1f1f`; `+0x174` `0x1f1f`; `+0xc00` `0x08080808`; `+0xc04` `0x0808`; `+0xc10` `0x8` | `pck600_domains.c` lines 126–142 and 547–569 | **Unproven.** The TRM names `0x170` and `0x174` (`PPU_DCDR0`, `PPU_DCDR1`); `0xc00`–`0xc10` are absent from it |
| ON or OFF | Policy bits 3:0 to 8 or 0, `DSB SY`, status polled for 10 ms | `pck600_domains.c` lines 104–121 | Host model |
| GPU before ON | MBUS gates set; `0x0b20` bit 31 and `0x0b24` bits 0 and 16 clear | `a733_gpu_power.c`, from `pvr_power.c` ([gpu-power.md](../gpu-power.md)) | Host model |
| Domain 5 OFF | The same three bits reread clear first; status must reach 0 | As above | Host model |
| NPU order | Resets and clocks enabled before the domain request | `vip_drv_device_platform.c` lines 419–428 | Host model |

The GPU keeps its preconditions in `a733_gpu_power.c`; its domain steps become
one row step. Barriers stay on the qualified `a733_mmio_*_dsb_sy_both`
accessors. After qualification, section 1.2 places one `DSB SY` after the
command write, because the spin bound is timed from the command reaching the
PPU.

## Interface for the NPU startup

1. `a733_power_domain_row_init(row, 4, owner, rail_token)` before the
   scheduler starts.
2. `a733_power_domain_check(4)`, which reads and never writes, before the
   first NPU reset or clock write.
3. Resets, module rate and clocks, then the NPU's existing `DSB SY`.
4. `a733_power_domain_request(row)` and `a733_power_domain_wait(row, deadline)`,
   followed by the NPU's existing 2,000 µs settle and readback.

The NPU package removes its delay-row and ON writes, the PCK write offsets in
`a733_npu_platform_native.c`, its DCDC2 snapshot and the direct S_TWI0 path in
`a733_npu_startup.c`.

## Code reused and removed

- **Reused:** the PMIC provider and native adapter, rail tokens and borrow
  pins, the state word (gaining domain fault, verified and done bits),
  `a733_range.h` deadlines and the raw counter time read.
- **Removed:** the PCK600 sequences in `a733_gpu_power.c` and
  `a733_de_sys_power.c`, the unused domain-10 entry, the GPU's DCDC4 snapshot
  per start and the per-tick PMIC loop bodies of images with a row consumer.
  The display callers keep the `a733_de_sys_power_*` signatures for now; the
  owner no longer calls their register callbacks, which the display packages
  delete.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| Verified bit clear, fault bit set, bad rail token, domain-level read wrong, GPU gate or reset open | 1, before any write | Nothing; row uncounted |
| Command and status disagree at adoption | 1; domain FAILED without a write | Nothing; later rows refused |
| Status not reached in 10 ms, or unexpected | 3 | Domain FAILED with fault bit, its rows and tokens, the PPU clock lease |
| PMIC transaction fault or deadline | 3, existing rule | I2C mutex, request, borrow, leases |
| Requester wait ends first | 1 | Nothing; slot kept until reap |
| Supply mismatch or timeout | 1 | Done bit with verified bit clear; rows refused |

## Budgets and measurement

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Polls and tick waits per snapshot; per selector write | 44 and 56 polls; up to one tick each in images that sleep after every poll | 0 tick waits; spin ≤ 0.6 ms and ≤ 0.8 ms at 22.5 µs per byte | `checks/a733-pmic-cost.c`; new `checks/power-domains-service.c` with a FreeRTOS shim counting ticks, wakes and gives |
| Requester wakes per PMIC or row request | One per tick of its own polling | 1 | `checks/power-domains-service.c` |
| Service wakes per second with no work | 1,000 | 0, or the image's watchdog period | Same |
| PMIC transactions per row request | GPU: 6 per start (one DCDC4 snapshot) | 0; 12 per boot | `checks/power-domains-pck.c` |
| Consumer cost of a row request | DE_SYS: platform register lock and 2 reads per poll | ≤ 100 instructions, 1 acquire load, no lock | `checks/rail-leases-cost.py` (Unicorn) |
| Tick waits, row request to ON, acknowledgement within 50 µs | DE_SYS: at least 1; GPU: 0 | 0 on one core and on two | `checks/power-domains-service.c` |
| Slow acknowledgement | One caller poll per tick | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick | `checks/power-domains-pck.c` |
| Register accesses per domain-5 ON | 14 (counted from the GPU steps) | 14, once per transition | `checks/a733-gpu-power.c` trace |
| Files that write the delay row | 6, plus 2 native allowlists | 5 after this package; 4 after the NPU stage | New `checks/power-domains-tree.py` |

## Left out

- Domains 2, 3 and 10: their writers are outside this package, and no image
  links two writers of one of them. CLDO2 and CLDO5, which only domain 10
  needs, are not verified.
- Power-off of domains 1 and 4, which have several masters; no caller needs it.
- The S_TWI0 packet engine and both interrupts: delivery is unqualified, and the
  spin costs at most 0.8 ms per request at a few requests per second.
- Lighter barriers, which need the board qualification in
  [docs/shared-helpers.md](../shared-helpers.md).

## Board evidence required

- PCK600 time to ON for domains 1, 4 and 5 and to OFF for domain 5, which
  replaces the 50 µs spin bound with the longest time plus 50%.
- GPU and framebuffer starts through the owner with the same register trace as
  the current drivers.
- Reads of `0xc00`–`0xc10` before and after the delay-row write.
- DCDC2 and DCDC4 selectors unchanged after PSCI CPU_ON and over a session; a
  bus capture with no second S_TWI0 master.
- USB0 and audio latency with PMIC requests running, SMP and single core.

## Changes outside this package

The PMIC task loops of the GPU, accelerator, GPU display, HDMI, DisplayPort and
G2D images call the service step, and the GPU and NPU native adapters drop
their PCK write offsets. These edits go to the primary agent as one patch. The
FreeRTOS configuration does not change.
