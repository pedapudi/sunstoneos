# Design: one power-domain owner and one PMIC service task

Status: **implemented and host-tested** for domains 1, 4 and 5 and the service
task; **qualified on the board** only for the PMIC snapshot step times of the
service (bundle 6, read-only); **open** for domain transitions on the board.
[docs/power-domains.md](../power-domains.md) describes the component.
[power-authority.md](power-authority.md) covers the rail ledger, the state
word and the lock-free PMIC time reads this design builds on. Section numbers
refer to [docs/performance-architecture.md](../performance-architecture.md).
Measurements and their conditions are in
[power-domains.jsonl](../evidence/power-domains.jsonl).

## Ownership and data flow

A PCK600 power domain is one 4 KiB block at `0x07060000 + 0x1000 × domain`.
`drivers/power/a733_power_domains.c` is the only writer of domains 1 (DE_SYS),
4 (NPU) and 5 (GPU_TOP). A tree check (`checks/power-domains-tree.py`) fails
if another file writes a block the owner holds.

- **Rows.** A consumer registers one static row before the scheduler starts:
  domain and rail token. Rows are the framebuffer and G2D (domain 1), the NPU
  startup (domain 4) and the GPU (domain 5). A domain is ON while an admitted
  row requests it. Only domain 5, whose one bus master is the GPU, is ever
  powered off; OFF requests for domains 1 and 4 are refused at the request
  call, because no consumer supplies the required quiescence and ownership
  proof.
- **Row request.** The consumer runs its existing authorization
  (`domain_authorize`: the designated task, whole-domain exclusion through the
  exclusive DE_SYS, IOMMU_SYS, VI, VE_DEC and VE_ENC clock tokens, the DCDC2
  rail token, the PCK lease and, for the framebuffer, media ownership and
  memory). It then stores ON in its row with release order and notifies the
  service. The service admits the row, reads the domain-level registers,
  writes the delay row and the ON command if the domain is OFF, and polls
  status. It publishes the row state with release order, and the consumer
  reads it with one acquire load from its per-tick state machine: the GPU
  through `a733_gpu_power_poll`, the framebuffer and G2D through
  `a733_power_domain_state`. No row API blocks.
- **Read-only check.** `a733_power_domain_check` runs the domain-level reads
  and the adoption check and writes nothing. Each consumer calls it before its
  first reset or clock write. Reads whose expected value depends on the block
  or session stay in the consumer, such as the GPU reset idle check and the
  framebuffer's check for inherited scanout DMA.
- **Service task.** One task per image runs every PMIC transaction and domain
  transition. The image's loop calls `a733_power_service_step()`, which
  advances all queued work and returns the ticks to block in
  `ulTaskNotifyTake(pdTRUE, ticks)`. It returns one tick while a slot is
  QUEUED or ACTIVE, the I2C mutex is held, a poll returned BUSY, or a domain
  transition is pending; otherwise it returns the ticks to the session
  deadline. Park and stop requests from task context notify the service.
  Fail functions that run from interrupt and fault paths (`gpu_application_fail`,
  `board_fail`) do not notify; the service observes a revocation within one
  tick or at its next timed wake. The integrated OS image's watchdog requires
  the PMIC service to report progress within 1 s, so that image's service blocks
  at most 500 ms when idle.
- **PMIC request.** `a733_pmic_submit` calls a kick hook (set with its own
  setter) that notifies the service. Submitters poll `a733_pmic_result` once
  per tick: the USB2 supply snapshots in the GPU display and HDMI audio
  images, the CPU clock selector writes, PWM, PCIe, the radio, ISP0 and JPEG
  encode. No submitter blocks.
- **Supply verification once per boot.** At provider start plus 5,160 µs (a
  1,000 µs enable delay and a 1,040 mV ramp at 250 µV/µs, board device tree
  lines 2123–2146), the service snapshots DCDC2 and DCDC4: 12 transactions. A
  rail enabled at 800 mV (DCDC4 also at 960 mV) sets its verified bit in the
  state word, then the done bit is set. The budget is 100 ms after the
  provider is READY. Row admission makes one acquire load of the state word
  and requires the supply's verified bit set and every
  `A733_POWER_PMIC_TRANSFER` bit clear. DCDC2 supplies domains 1 and 4
  (`npu-supply`, line 2932); DCDC4 supplies domain 5 (`gpu-supply`, line
  3421). A terminal failure sets the done bit with no verified bit, so rows
  are refused within a bounded time instead of staying PENDING. The service
  releases the verification token after success and keeps it after a failure.
  An image that registers no row, such as the integrated OS image, verifies
  nothing and makes no PMIC read at boot. The scheme relies on no agent
  writing DCDC2 or DCDC4 after boot; firmware writes and a second S_TWI0
  master are excluded only by board evidence (open work).
- **PPU clock.** The owner reads R_CCU `0x1ac` bit 0 at every admission and
  transition and holds no lease; every consumer already holds a stable PCK
  lease of the GPU resource manager before it requests its row.

## Transitions and polling

- **Adoption.** Command and status both OFF give OFF; both ON give ON, and
  domain 5 also needs its gate and reset bits clear; anything else gives
  FAILED without a write. A domain is UNKNOWN, OFF, ON_WAIT, ON, OFF_WAIT or
  FAILED. Commands are written only from OFF or ON.
- **PCK600 acknowledgement is polled.** The pinned `pck-600@7060000` node has
  no `interrupts` property (`sun60iw2p1.dtsi` lines 745–753). The PPU has
  `PPU_IMR` (`0x030`) and `PPU_ISR` (`0x038`), but their GIC wiring is
  unknown. The service spins up to a provisional 50 µs after the command, then
  reads status once per one-tick step until FAILED at 10 ms, the vendor bound
  (`pck600_domains.c` lines 112–121). Section 1.3 permits this tick wait for a
  device without qualified interrupt delivery.
- **Deadline.** A transition's deadline is the earlier of the service session
  deadline and 10 ms after the completed policy write. The owner rejects a
  status observation at or after the deadline and checks time again after a
  target read. Before programming delays or the policy it checks that the
  session remains open; this does not erase an adopted ON state or reclaim
  retained resources. The consumer's own earlier deadline remains its
  responsibility.
- **Domain 4 keeps the pinned NPU order.** The NPU startup calls
  `a733_power_domain_check(domains, 4, &state)` before local reset or clock
  writes, then configures PLL_NPU and the local clocks and resets, requests ON
  with `a733_power_domain_request(row, 1)` and polls
  `a733_power_domain_state(row)` under its own deadline. A coherent inherited ON
  state is adopted without a PCK write. The NPU issues a `DSB SY` after its last
  CCU write and before the row request. The existing 2,000 µs settle and
  readback follow ON. The sequencer writes no PCK register and retains the row,
  rail pin and PLL after healthy local CLOSE.
- **Power-off only for domain 5.** At count zero, after the GPU drains its
  DMA, the owner rereads the GPU gate and reset bits (`0x0b20` bit 31, `0x0b24`
  bits 0 and 16); if any is set, release returns RETAINED.
- **PMIC byte phases spin within a bounded step.** The service waits for the
  pending phase's expected duration (two bit times for START, repeated START
  and STOP, nine for a byte), timed from the provider's time of the
  phase-start write ([pmic-access.md](pmic-access.md)), by reading the raw
  counter without the provider lock, then calls `a733_pmic_poll` once. Spinning
  stops after 1 ms in one step, and the step returns one tick. The S_TWI0
  interrupt (GIC SPI 203, `sun60iw2p1.dtsi` line 1962) is unqualified.
- **Retention.** An unexpected PCK600 status or a passed deadline retains the
  domain, its rows, their tokens and the PCK lease, and sets the domain's
  fault bit. A failed or regressed completion clock fails the service and
  every pending domain.

## Memory placement and synchronization

Owner and service state is static in kernel `.bss`, Normal write-back memory:
the domain table, and the rows in consumer storage. The PCK600, CCU and R_CCU
windows are Device-nGnRnE and are verified once with `a733_mapping_check.h` by
the native adapter's init before the scheduler starts. No buffer is shared with
a device, so no cache maintenance occurs.

- Each field has one writer. The consumer writes its row's request word; the
  service writes domain state, row state and verified bits. Readers make one
  acquire load.
- Only the service task touches the PCK600 blocks it owns, so a transition
  takes no owner lock. The CCU and R_CCU accesses are reads.
- The provider's try-lock is held only inside one `a733_pmic_poll`, never
  across a byte wait. `a733_pmic_live` takes no lock. `a733_pmic_result` and
  `a733_pmic_reap` keep the lock. A fault that a client latches while the
  service holds the I2C mutex sets the state word; the work query then reports
  FAILED with RETAIN, and the service polls once more so the provider marks its
  mutex retained.
- The service task runs below the image's USB0 and audio tasks, on A55 core 0
  (`xTaskCreateStaticAffinitySet`, mask 1); single-core builds use
  `xTaskCreateStatic` at the same priority. A higher-priority task preempts it
  at any point. The owner's code has no critical section or scheduler
  suspension.

| Image (PMIC loop) | Service task priority |
| --- | --- |
| GPU (`apps/gpu-render/main.c`) | 1, core 0 |
| Accelerator (`apps/npu-convolution/native.c`) | 3, core 0 |
| GPU display (`apps/gpu-display/main.c`) | 2, core 0 |
| HDMI (`apps/hdmi/main.c`) | 3, core 0 |
| DisplayPort and G2D (`apps/board/main.c`) | 3, core 0 |

In the accelerator image the service at priority 3 on core 0 ties with
`gpu-service` and preempts the `gpu-compute` client (priority 2, core 0). While
a PMIC request is active, each service step can spin up to 1 ms on core 0,
which delays the compute client by that much per step.

## Register sequences and sources

The owner's domain checks, adoption, delay row (`+0x170`, `+0x174`, `+0xc00`,
`+0xc04`, `+0xc10`, each read back), ON and OFF commands, the GPU preconditions
and the NPU order follow `pck600_domains.c`, `ccu-sun60iw2.c`,
`ccu-sun60iw2-r.c`, `a733_gpu_power.c` (from `pvr_power.c`) and
`vip_drv_device_platform.c`. The sources are pinned in `sources.lock.json`.
The registers, values, source line references and statuses are tabulated in
record `power-domains-register-sequences`. The delay-row registers `0xc00`
to `0xc10` are not in the PCK-600 TRM and are unproven on the board. The GPU
keeps its preconditions in `a733_gpu_power.c`; the native adapters keep
`DSB SY` before and after each access, and section 1.2 places one `DSB SY`
after the command write because the spin bound is timed from the command
reaching the PPU.

## Code ownership

- **Owner and adapters.** `drivers/power/a733_power_domains.c` and its native
  adapter `a733_power_domains_native.c`; the row header
  `include/a733_power_domain_row.h`, whose request and state calls are inline so
  that targets linking only the GPU controller or the supply consumer do not
  need the owner.
- **Provider.** The service binding with its notification hook, the work query
  that reports the pending bus phase and its expected duration, the bit time
  recorded from the admitted divisor, a lock-free `a733_pmic_live`, and a count
  of queued requests.
- **Rail ledger.** `a733_rail_pin` and `a733_rail_unpin`.
- **Consumers.** The GPU controller's domain steps are one row step, with its
  preconditions kept; the GPU platform registers its row through
  `a733_gpu_platform_bind_pmic`, and its native adapter has no PCK write
  offset. G2D and the framebuffer call `a733_power_domain_row_init`,
  `a733_power_domain_request` and `a733_power_domain_state`. The GPU supply
  consumer completes from the boot record with no bus transaction.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| Designated task, clock tokens, PCK lease, media or memory wrong | 1, in the consumer's `domain_authorize` before the request | Nothing; no request stored |
| Verified bit clear; provider, clock, ledger or domain fault bit set; bad rail token; domain-level read wrong; GPU gate or reset open | 1, at admission before any write | Nothing; row uncounted |
| Command and status disagree at adoption | 1; domain FAILED without a write | Nothing; later rows refused |
| Status not reached in 10 ms, or unexpected | 3 | Domain FAILED with fault bit, its rows and tokens, the PPU clock lease |
| PMIC transaction fault or deadline | 3, existing rule | I2C mutex, request, borrow, leases |
| Supply mismatch or timeout | 1 | Done bit with verified bit clear; rows refused |

## Accelerator lifecycle assessment

GPU healthy OFF/ON has a software contract and host evidence; its physical
lifecycle is unqualified. NPU startup and local CLOSE use a retained ON
domain. The references establish different responsibilities, and the table
with their limits is in record `power-domains-accelerator-assessment`:
register status alone does not prove accelerator drain; GPU firmware
handshakes and local drain precede reset, gate teardown and platform
power-off; the NPU platform source does not establish when runtime power
management completes OFF; the generic PCK-600 TRM supplies no A733 isolation
register; and shared SRAM `0x00074000`–`0x000f3fff` requires NPU power while in
use.

- **GPU.** The stop controller closes admission, obtains fresh firmware
  acknowledgements and independent power states, retires queues and checks
  BIF, MMU, cache and wrapper idle before reset readback. The platform
  controller then asserts external reset, closes local gates and requests
  domain 5 OFF; the owner acknowledges OFF before the platform releases
  resources. Healthy reopening uses the ON row request with gates closed and
  reset asserted.
- **NPU.** Orderly CLOSE drains accepted work through its controller bounds,
  stops interrupts, closes local module and bus gates and asserts the local
  engine resets. It retains domain 4, the SRAM reset state, PLL and leases.
  Full NPU OFF needs a caller that proves no outstanding device work and no
  shared-SRAM users, plus a source-backed ordering between power completion and
  clock teardown. The thermal consumer supplies none, so the domain stays
  retained.

## Qualified figures

| Figure | Value | Evidence |
| --- | --- | --- |
| Snapshot step on the board, provider before the access changes | 917 µs median, 146% of 630 µs wire time | `power-domains-bb6-service-steps` |
| Snapshot step with the access changes | 779 to 796 µs, 124% to 126% | [pmic-access.jsonl](../evidence/pmic-access.jsonl) |
| Provisional step bound | median at most 1,268 permille of measured wire time; first step plus 40 µs | `pmic-access-bound-selection` |
| Model, 200 ns per access, 400 kHz | snapshot 687 µs, selector write 885 µs, one step each (107% of model wire time) | `power-domains-merge-base-comparison` |
| PMIC transactions per row request | 0 (12 per boot) | `power-domains-merge-base-comparison` |
| Row-state read | 12 instructions, one acquire load | `power-domains-merge-base-comparison` |
| Register accesses per domain-5 ON | 14 PPU accesses, plus 5 domain-level reads | `power-domains-merge-base-comparison` |
| Slow acknowledgement | FAILED 10,092 µs after the command | `power-domains-merge-base-comparison` |

All figures except the board snapshot steps are host-modeled. The 110%
wire-time target (693 µs) is not reachable by a provider that polls each bus
phase; see [pmic-access.md](pmic-access.md).

## Left out

- Domains 2, 3 and 10: their writers are outside this design, and no image
  links two writers of one of them. HDMI and DisplayPort power domain 10
  themselves; CLDO2 and CLDO5, which only domain 10 needs, are not verified.
- Power-off of domains 1 and 4, which have several masters; no caller needs it.
- The S_TWI0 packet engine and both interrupts: delivery is unqualified, and
  the spin is capped at 1 ms per step at a few requests per second.

## Open work

1. The deinterlace consumer of domain 1 still requires the domain already ON
   and reads its policy and status; it moves onto a row in its own package.
2. Board evidence (record `power-domains-board-evidence-required`): the
   inherited S_TWI0 divisor; PCK600 time to ON for domains 1, 4 and 5 and to
   OFF for domain 5, which replaces the 50 µs spin bound with the longest time
   plus 50%; GPU and framebuffer starts through the owner with the previous
   register trace; reads of `0xc00`–`0xc10` around the delay-row write; DCDC2
   and DCDC4 selectors unchanged after PSCI CPU_ON with a bus capture showing no
   second S_TWI0 master; USB0 and audio latency with PMIC requests running; a
   back-to-back status trace during each transition; the DCDC3 selector-write
   step duration; qualified S_TWI0 interrupt delivery.
3. GPU: one complete healthy start, numerical operation, shutdown and reopen
   with owned memory and recovery intact, recording fresh firmware states,
   local idle and reset readbacks, PPU policy and status transitions and
   retained ownership on failure. NPU: a source-reviewed OFF/ON procedure that
   excludes every shared-SRAM user and establishes power-completion and clock
   ordering.
