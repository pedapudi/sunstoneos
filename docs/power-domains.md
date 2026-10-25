# One service owns PCK600 domains 1, 4 and 5

Status: **implemented and host-modeled**. On port 3-3 on 2026-10-24, two
returning images ran it: one made 32 PMIC snapshots and the adoption reads
of domains 1, 4 and 5 without a PPU write, and one adopted domain 1, which
was already ON, without a transition. The board evidence still needed to
qualify it is listed at the end.

The A733 PCK600 power controller has one 4 KiB block per power domain at
`0x07060000 + 0x1000 × domain`. Each block has a policy word (`+0x0`), whose
bits 3:0 request OFF (0) or ON (8), a status word (`+0x8`) that reports the
reached state, and a delay row (`+0x170`, `+0x174`, `+0xc00`–`+0xc10`) that the
pinned vendor driver initializes when it registers each domain.
[`drivers/power/a733_power_domains.c`](../drivers/power/a733_power_domains.c)
is the only writer of domain 1 (DE_SYS, the display engine and G2D),
domain 4 (NPU) and domain 5 (GPU_TOP). Domains 2, 3 and 10 keep their own writers: the video platform,
HDMI and DisplayPort. The design and its review are in
[docs/designs/power-domains.md](designs/power-domains.md).

## Consumers hold rows

A consumer registers one **row** for its domain before the scheduler starts:
the domain, its owner identity and a pointer to its rail token, which must
hold the domain's supply (DCDC2 for domains 1 and 4, DCDC4 for domain 5). The
row lives in the consumer's storage
([`a733_power_domain_row.h`](../include/a733_power_domain_row.h)). The
consumer runs its own authorization, then stores ON in the row with release
order and notifies the service task. The service answers by publishing ON,
OFF, REFUSED or FAILED with release order. The consumer reads the answer with
one acquire load from its existing per-tick state machine: 12 AArch64
instructions and no lock. A domain is ON while an admitted row requests it.
Only domain 5, whose one bus master is the GPU, is ever powered off.

| Consumer | Domain | Row owner and rail token | Reads the answer in |
| --- | --- | --- | --- |
| GPU controller ([`a733_gpu_power.c`](../drivers/gpu/a733_gpu_power.c)) | 5 | The GPU platform's DCDC4 consumer; `a733_gpu_platform_bind_pmic` registers the row on the owner bound to the PMIC provider | `a733_gpu_power_poll` |
| Framebuffer ([`a733_display_framebuffer.c`](../drivers/display/a733_display_framebuffer.c)) | 1 | The output controller's DCDC2 token, lent through the configuration | Its cold-start step |
| G2D (`apps/g2d/native.c`) | 1 | G2D's DCDC2 token | Its setup step and register guards |

The GPU controller keeps its own preconditions. Before the row request it
checks both MBUS gates (CCU `0x5c0` bit 7, `0x5e0` bit 16), the PPU bus clock
(R_CCU `0x1ac` bit 0), a closed GPU clock gate (`0x0b20` bit 31) and an
asserted GPU reset (`0x0b24` bits 0 and 16). It opens the clock and releases
the reset only after the row reads ON. Power-off closes the reset and clock
first and then requests the row OFF. The framebuffer and G2D keep their
authorization functions, which check the designated task, the exclusive
DE_SYS, IOMMU_SYS, VI, VE_DEC and VE_ENC clock tokens, the DCDC2 rail token and
the PCK lease, and for the framebuffer its media ownership and memory. The
framebuffer calls `a733_power_domain_check(domains, 1, NULL)` before its first reset write,
from its output task. The check makes only reads through the owner's native
adapter, which verified at init, before the scheduler started, that the PPU,
CCU and R_CCU windows are identity Device-nGnRnE mappings; a failed mapping
check refuses every access.

## The service admits rows and runs every transition

Admission of an ON request, in order:

1. Supply verification has ended, the domain's supply has its verified bit,
   and no provider, clock-registry, ledger or domain fault bit is set.
2. The rail token belongs to the row's owner and holds the supply.
3. Domain-level reads: both MBUS gates of the domain (CCU `0x5c0` and `0x5e0`
   bits 5 and 11, 6 and 18, 7 and 16 for domains 1, 4 and 5) and the PPU
   clock. A domain-5 ON from OFF reads the PPU clock alone: a TOP5 OFF
   closed the GPU AHB master gate on a board, and the GPU controller
   requires both gates after TOP5 ON. Domain 5 found ON still requires them. Domain 5 changes whatever the GPU clock gate and reset read: the
   vendor powers it on after the GPU clocks open and off before they close
   (`sunxiPrePowerState`, `sunxiPostPowerState`;
   [docs/designs/gpu-vendor-default.md](designs/gpu-vendor-default.md)).
4. Policy and status agree at OFF or ON. The first admission adopts that
   state; later admissions require it unchanged.
5. The ledger pins the rail token, so its release returns BUSY while the row
   is counted (`a733_rail_pin`, [docs/rail-leases.md](rail-leases.md)).

If the domain is OFF, the service writes the delay row, reading each word
back, and then writes the policy word with bits 3:0 set to 8 and the other
bits preserved. The write accessor completes with `DSB SY`, so the bound is
timed from the command reaching the PPU. The service reads status for up to
50 µs after the command, then once per step with a one-tick block between
steps, until status reads 8 or the transition deadline expires. That
deadline is the earlier of 10 ms after the command and the service session
deadline. Time is checked before the status read and again after a target
status, so a read that crosses the deadline cannot complete the transition.
A command admitted at or after the session deadline writes no delay or
policy register and retains the already counted row and pin. A domain-5
OFF request at count zero rereads the GPU clock gate and reset closed, writes the OFF command and
waits the same way for status 0, then unpins the token.

Adopting an inherited ON domain remains read-only. Before its first OFF command,
the owner initializes and reads back all five delay words if this owner has not
already done so. The reference calls `sunxi_pd_init` at registration even when
the domain is ON; relying on unknown inherited delays omitted that prerequisite.
Every ON retains its existing delay writes. The `delays_ready` flag is set only
after all five readbacks match and uses existing structure padding. After
initialization, another monotonic time read must precede the session deadline
before the policy write is permitted. A partial write, failed readback or expired
clock check retains the counted row and rail pin and prevents the policy write.

This adds five writes, five readbacks and one time sample to the first OFF of an
inherited ON domain. ON keeps its existing register accesses and adds one time
sample; subsequent OFF operations add no access. Unrequested domains, including
a retained NPU domain, receive no initialization or command.

The PCK600 acknowledgement is polled because the pinned `pck-600@7060000`
device-tree node has no interrupt, and the PPU interrupt registers have no
known GIC wiring.

## Accelerator shutdown requires the consumer's quiescence proof

The shared owner checks domain policy, status, clocks and rail ownership.
Those observations alone do not establish that an accelerator has stopped
issuing memory transactions. The existing [healthy GPU shutdown](gpu-isolation.md)
provides that proof before requesting domain 5 OFF: fresh firmware idle/OFF
states, queue retirement, BIF/MMU/cache idle polls and reset readback. The
GPU controller then writes policy zero to the GPU core PPU (CORE6, at
`0x07066000`) and checks CORE status zero while GPU_TOP and the local clocks
remain available. It then asserts external reset, closes its local gates and
requests OFF through the existing row. Successful OFF permits its normal
resource release; a deadline or clock failure preserves the row and rail pin.

CORE status zero is not a quiescence proof. GPU startup writes the CORE6
power configuration register (PWCR, offset `0x20`) to zero, which disables
the PPU's device interface channels and so its handshake with the GPU.
CORE6 transitions then proceed without issuing requests to the GPU or
waiting for its responses
([DEN0051E](https://documentation-service.arm.com/static/5f873177f86e16515cdb6d99),
page 4-13, "Device Interface Channel Enables"). The firmware-stop proof above
therefore remains the only evidence that the GPU has stopped issuing
transactions before CORE6 and domain 5 power off.

NPU local CLOSE retains domain 4 ON and its rail pin. The pinned NPU
platform calls `pm_runtime_put()` before disabling local clocks; those
calls alone do not establish when the power-domain transition completes. The A733 memory map also requires NPU power while
shared SRAM at `0x00074000`–`0x000f3fff` is in use. A domain-4 OFF consumer
must establish the NPU-specific stop/order contract and exclude every live
CPU and DMA user of that SRAM. The current thermal CLOSE consumer has no
such power-off contract, so the shared API continues to refuse domain-4
OFF requests. The [design assessment](designs/power-domains.md#accelerator-lifecycle-assessment)
records the primary-source anchors and missing physical evidence.

## Failures retain what is uncertain

| Failure | Answer | Retained |
| --- | --- | --- |
| Consumer authorization refused | No request is stored | Nothing |
| Supply not verified, fault bit set, wrong token, domain-level read wrong | REFUSED, before any write | Nothing |
| Policy and status disagree at adoption | REFUSED, domain FAILED, fault bit set, no write | Nothing; later rows refused |
| Delay-row readback wrong, unexpected status or acknowledgement at/after the transition deadline | FAILED, fault bit set | The domain, its rows and their rail pins |
| Rail unpin fails after domain-5 power-off | FAILED; a later ON request from the row is answered FAILED | The row, counted, and its pin |
| A service time read fails | Transitions FAILED; later requests REFUSED | As above |

The fault bits (`A733_POWER_DOMAIN1_FAULT`, `…4_FAULT`, `…5_FAULT`) are part of
the power-authority state word ([docs/power.md](power.md)).

## Supplies are verified once per boot

Verification starts 5,160 µs after the service's first step: the board device
tree's 1,000 µs enable delay plus a 1,040 mV ramp at 250 µV/µs. An owner
marked settled with `a733_power_domains_settled_supply` before its service
first runs verifies at that first step instead. NPU startup init sets that
mark. No image enables DCDC2 or DCDC4 or changes their voltage, and
verification passes only for a rail found enabled at its target selector.
The board kernel's regulator core likewise applies its enable delay only to
an off-to-on enable and its ramp delay only to a selector change
(`drivers/regulator/core.c` lines 2905–2930 and 3660–3713, linux-orangepi
`2ac08e8c`, SHA-256 `65b39b20…`, cached under
`~/.cache/sunstoneos/sources/linux-orangepi-2ac08e8c-regulator/`). A settled
owner also publishes each rail's verified bit as soon as that rail is
verified, and admits a row whose rail is verified before the done bit. A
later failure of the other rail sets the done bit without that rail's bit
and leaves the published one in place. The service
acquires one shared DCDC2 and DCDC4 rail token and snapshots both outputs
through the PMIC provider, 12 single-register transactions. After success it
releases the token, so verification holds a rail-ledger slot for a few
milliseconds only. An image that registers no row skips verification and makes
no PMIC read. A rail enabled at 800 mV (selector 30) sets its
verified bit (`A733_POWER_DCDC2_VERIFIED`, `A733_POWER_DCDC4_VERIFIED`), and
then the done bit is set.

Verification has a budget of 100 ms, which starts at the first step in which
the settle time has passed, or at once for a settled supply, and the provider is READY; before READY the
provider's startup deadline bounds the wait, and a provider fault ends it. Any
terminal failure sets the done bit with no verified bit at once, so
rows waiting for verification are refused within that bound. A verification
request whose result is certain, complete or expired, is then reaped, and the
token is released once no request is outstanding. A request whose outcome is
uncertain, after a provider, clock or ledger fault, keeps its PMIC slot and
the token, which it may still borrow.

The GPU's DCDC4 consumer ([docs/pmic-consumers.md](pmic-consumers.md))
completes from the boot reading of DCDC4 (`dcdc4` in the owner), which the
DCDC4 verified bit publishes, with no bus transaction and no PMIC request
slot. The DCDC2 reading is not kept: domain-1 and domain-4 admission needs
only its verified bit. Only
selector writes to DCDC3 and DCDC5 are accepted; that no firmware writes DCDC2
or DCDC4 after boot is established only by board evidence.

A row may carry a consumer wake (`wake`, `wake_context` in
`a733_power_domain_row.h`), set before the service first runs. The service
calls it after it publishes an answer to that row and after it publishes a
supply result, from the service task. NPU startup uses it to notify its
bootstrap task.

## One service task per image

`a733_power_service_step()` replaces the per-tick PMIC loop in five images. It
advances PMIC provider startup, every queued PMIC request, supply verification
and every row and domain transition, then returns the ticks for
`ulTaskNotifyTake(pdTRUE, ticks)`:

- one tick while a PMIC request is queued or active, the I2C mutex is held, a
  lock returned BUSY, verification is in progress or a domain transition is
  pending;
- otherwise the ticks to the next timed event: verification, the session
  deadline (when the image's liveness check parks the task), or a configured
  idle cap.

`a733_pmic_submit` calls a notification hook that the owner binds
(`a733_pmic_bind_service`), and a row request notifies through the row. A
notification from the service task to itself is dropped: the step handles its
own submissions. During a PMIC transfer the service spins on the raw counter,
without the provider object, until the expected duration of the bus phase (two
bit times for a START, repeated START or STOP, nine for a byte, from the
inherited S_TWI0 divisor) has passed since the provider's time of the write
that started the phase, and at least a quarter bit time after a poll that
found the phase unfinished, then polls once
([pmic-access.md](designs/pmic-access.md)). It spins at most 1 ms and makes
at most 256 polls in one step.
The owner's code uses no critical section, scheduler suspension, delay or
notification wait. The PMIC poll that the step calls can block when it
activates a request: the pin check takes the register lock and the bus entry
takes the I2C mutex, each in one-tick waits until that request's deadline. A
higher-priority task preempts the service at any point, including those waits.

When a client on another task latches a provider fault while the service holds
the I2C mutex for a transfer, `a733_pmic_work` reports FAILED with RETAIN. The
service then polls once more, and that poll marks the mutex retained, as the
provider requires (`include/a733_pmic.h`); it keeps one-tick steps only while
that poll returns BUSY. `a733_pmic_work` takes the fault from one acquire load
of the state word, not from the provider's plain fields that other tasks
write.

| Image (loop source) | Service task | USB0 and audio tasks | Owner |
| --- | --- | --- | --- |
| GPU (`apps/gpu-render/main.c`) | priority 1, core 0 | `gpu-usb` 2, core 0 | `app.domains` |
| Accelerator (`apps/npu-convolution/native.c`) | 3, core 0; ties with `gpu-service` (3, core 0) and preempts `gpu-compute` (2, core 0) | `accelerator-usb` 4, core 0 | `app.domains` |
| GPU display (`apps/gpu-display/main.c`) | 2, core 0 | USB 3 | `app.domains` |
| HDMI (`apps/hdmi/main.c`) | 3, core 0 | output and audio USB 4 | `app.domains` |
| DisplayPort and G2D (`apps/board/main.c`) | 3, core 0 | `board-usb0` 4 | `providers.domains` |

In the accelerator image a service step can therefore delay the GPU compute
client by up to 1 ms of bus-phase spin per step while a PMIC request is
active, and shares core 0 in turns with the GPU service worker at the same
priority. The board evidence below includes the measured step duration.

The board image is also the base of the integrated OS image, whose resident
watchdog requires every service to report progress within 1 s. Its service
blocks at most 500 ms when idle (`SUNSTONE_BOARD_RESIDENT`). The other images
block until the session deadline when idle.

Park and stop requests notify the service: the GPU display park, the GPU
finite-return stop and the accelerator's finite quiescence. They complete only
when `a733_power_service_idle()` holds: the PMIC provider is idle, no
verification request is outstanding, and every domain is ON, OFF or FAILED
with no row request unanswered. The fail functions do not notify, because
some run in exception context. An idle service holds no bus or lock, and a
pending transfer keeps the service on one-tick steps, so it observes a
revocation within one tick or at its next timed wake.

`a733_pmic_live` takes no lock: it compares the immutable provider lifetime
and makes one acquire load of the state word, so it never returns BUSY. A
fault it sees is latched by the service's next poll. `a733_pmic_result` and
`a733_pmic_reap` keep the provider object's try-lock, which the service holds
only inside one poll and never across a bus-phase spin.

## Domain 4 and the NPU startup

The [NPU startup sequencer](npu-startup.md) uses domain 4 through this
interface:

1. `a733_power_domain_row_init(row, domains, 4, owner, rail_token)` before the
   scheduler starts; the token must hold DCDC2.
2. `a733_power_domain_check(domains, 4, &state)`, which reads the MBUS gates
   (CCU `0x5c0` bit 6, `0x5e0` bit 18), the PPU clock and the policy and
   status words, writes nothing, and returns the status read in `state`, 0
   for OFF or 8 for ON. The sequencer calls it before its first NPU reset or
   clock write, accepts coherent OFF or ON, and does not read the policy and
   status words again in its register pass. Both states must pass the same
   NPU gate, reset and disabled-PLL checks. An ON request for an inherited ON
   domain adopts it and pins the rail without writing PCK600.
3. The sequencer releases the NPU resets (`0xb04` bits 18, 17, 16), sets the
   module rate and enables its clocks, in the order of the pinned VIPLite
   driver (`vip_drv_device_platform.c` lines 419–428).
4. `a733_power_domain_request(row, 1)`, then `a733_power_domain_state(row)`
   once per step until it reads ON, followed by the sequencer's 2,000 µs
   settle and readback. REFUSED and FAILED are terminal.

Domain 4 never powers off: `a733_power_domain_request(row, 0)` is refused.
The sequencer reads domain-4 policy and status but writes no PCK600 register
and reads no delay row. It waits for the supply by reading
`A733_POWER_SUPPLY_DONE` and `A733_POWER_DCDC2_VERIFIED` in the authority
word; it issues no PMIC transaction. `checks/power-domains-pck.c` covers the
check and a domain-4 row; `checks/npu-startup-host.c` runs the sequencer with
the actual owner.

## Measurements

The host models, the merge-base comparison and the commands are recorded in
the design document's implementation section
([docs/designs/power-domains.md](designs/power-domains.md)). The figures are
modeled, except the service steps of the read-only board runs.

On the board, 400 kHz snapshot steps took 146% of their wire time against a
110% target, and 123.7% and 124.4% (779 µs and 784 µs, medians of two
runs) after the provider follow-up.
The provisional median limit is 1,268 permille of each step's measured
wire time, with another 40 µs for the first step, at CPU-L 494 MHz under
bundle 6's conditions. At nominal wire time those limits round to 799 µs
and 839 µs. The measured access costs put 110% out of reach of a design
that polls each bus phase: with no software cost at all, the model's
register accesses alone give 113% with `DSB SY` pairs and 110.2%
with the lighter placement. Section "Board result of the service steps" of
the [design](designs/power-domains.md) gives the evidence and the limits of
the cost model.

## Checks

- `checks/power-domains-pck.c` composes the actual provider, ledger and owner
  over modeled PPU, CCU and R_CCU words: the delay row and command, adoption,
  incoherent and domain-level refusals, verified supplies, the verification
  budget started at provider READY, an expired verification request reaped
  with its token released, the slow acknowledgement bound with retention,
  domain-5 power-off and reopen, a failed unpin answered FAILED, and the NPU
  interface. `--deadlines` selects ON/OFF samples before, at and after the
  bound, status reads crossing the bound, failed/regressed completion clocks,
  earlier session cutoffs and expired command admission. `--delays` selects
  inherited OFF/ON and repeated transitions, exact first-OFF write/readback order,
  18 initialization failures with retained pins, and preservation of an untouched
  domain-4 register block. With an output
  path it writes the GPU power-on and
  power-off register trace that `checks/a733-gpu-power-reference.py`
  compares with the pinned PCK functions.
- `checks/power-domains-service.c` runs the service step with a shim that
  records its ticks, wakes and notifications, at 400 kHz and 100 kHz, with one
  modeled core and with two, and the retention of the I2C mutex after a fault
  that a client latches during a transfer.
- `checks/power-domains-tree.py` checks the source facts: one writer of
  domains 1 and 5, and five delay-row writers. The GPU controller is the
  writer of domain 6 (GPU_CORE), and the GPU native adapter is the only
  native allowlist that admits the delay row, for that domain alone.
  It rejects direct NPU delay-row writes alongside a domain-4 row consumer,
  checks the five service tasks and their priorities, and rejects critical
  sections, scheduler suspension or waits in the owner's code.
- `checks/rail-leases-cost.py` measures the row read, counting load/store
  exclusive and LSE atomic instructions as exclusive accesses;
  `checks/rail-leases.c` covers the rail pin.
- `checks/g2d-power-native.py` runs the G2D task of a linked G2D image with
  the row answered by the model: ON, REFUSED, FAILED and never answered.

The [live accelerator composition check](../checks/accelerator-shared-owner.py)
exercises the actual NPU startup, GPU power and resource manager, PMIC consumers,
rail ledger, clock registry and domain owner together. Five focused sanitizer
cases cover overlapping domain-4/domain-5 admission, each domain's local timeout,
shared PMIC transport failure, and, with the PMIC bus at 100 kHz, domain-4
admission on DCDC2 verified while the owner still reads DCDC4. They also verify that local NPU stop retains
its domain, PLL and rail ownership while the GPU shuts down. Domain-local failure
can leave the other admitted consumer progressing; global transport uncertainty
retains ownership and prevents further device access.

Evidence is under `accelerator-shared-owner-20261024/snapshot/manifest.json`.
Hardware effects, task context and scheduling are modeled. These cases establish
shared-owner behavior, not firmware drain, thermal orchestration, multicore
concurrency or physical operation. No production change was needed.

## Board evidence required

- The inherited S_TWI0 divisor (`TWI+0x14`) and the bus rate it gives.
- PCK600 time to ON for domains 1, 4 and 5 and to OFF for domain 5, which
  replaces the 50 µs spin bound with the longest time plus 50%.
- A trace of the status word (`+0x8`) read back to back during each of those
  transitions. The owner fails a transition on any status other than the
  starting or the target value, while the pinned `pck600_domains.c` waits
  only for the target value. The trace shows whether an intermediate value
  occurs.
- GPU and framebuffer starts through the owner with the same register trace as
  before.
- Reads of `0xc00`–`0xc10` before and after the delay-row write; the PCK-600
  technical reference manual does not name these offsets.
- DCDC2 and DCDC4 selectors unchanged after PSCI CPU_ON and over a session,
  and a bus capture with no second S_TWI0 master.
- USB0 and audio latency with PMIC requests running, SMP and single core.
- The duration of each service step during a DCDC3 selector write at the
  inherited divisor, and the split of a provider register access's cost
  (section "Board result of the service steps" of the design).
- Delivery of the S_TWI0 interrupt (GIC SPI 203, `sun60iw2p1.dtsi` line 1962)
  for each bus phase, qualified as the shared interrupt rules require. Only
  with that qualification can the service block on the interrupt instead of
  spinning for each bus phase.
