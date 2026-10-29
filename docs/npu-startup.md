# Start the NPU through one sequencer on the power-domain owner

The [startup sequencer](../drivers/npu/a733_npu_startup.c) powers, clocks and
releases the A733 NPU from reset, then lets the [NPU service](npu-service.md)
activate. One privileged bootstrap task advances the sequencer until READY,
waiting on provider notifications and the settle deadline between steps. The sequencer borrows the image's providers: the
counter clock, the clock registry, the register lock, the image's one GPU/PCK
resource manager, the rail ledger with its power-authority word, and the
[power-domain owner](power-domains.md). It constructs none of them.

Status: implemented and board-qualified in the finite interrupt image on
port 3-3. Startup took 4.0 ms, and all four ShuffleNetV2 outputs matched in
`board-session-20261025-15/port-3-3`. At the default 852 MHz operating point,
inference took 2.91 ms from doorbell to interrupt and about 3.3 ms end to end.
The design and its budgets are in [designs/npu-board.md](designs/npu-board.md),
the routing, power order and adoption in
[designs/npu-sram-route.md](designs/npu-sram-route.md), and the software-model
counts in [designs/npu-board-measurements.md](designs/npu-board-measurements.md).
Both boards are speed bin 4, whose vendor maximum is 852 MHz at 800 mV.

## Construction before the scheduler

`a733_npu_startup_init` runs at EL1 before the scheduler starts. It copies the
configuration and the provider view, and registers one domain-4 row with the
power-domain owner. The configuration holds:

- the fabric record: the AHB, NSI and MBUS selector, divisor and enable words
  with their exact rates ([platform tables](npu-platform.md));
- the owner identity used for the clock, rail and row requests;
- the session deadline in counter microseconds;
- the image's PCK client number for the NPU.

Initialization refuses a fabric record outside the supported SYS24 and PERI0
profiles, a provider that is missing or uninitialized, and storage that
overlaps the configuration or the provider view.

The optional operations structure supplies register, time and lock callbacks
for host models. With `NULL`, the sequencer uses the native ones: the NPU
allowlist adapter ([`a733_npu_platform_native.c`](../drivers/npu/a733_npu_platform_native.c)),
the counter clock, `CNTPCT_EL0` for LOCK spacing and the shared register lock.
`a733_npu_startup_bind_task` then names the one task that may call the step,
stop and activation functions.

## One step function with counter deadlines

Each call to `a733_npu_startup_step` checks the calling task, EL1 and the
scheduler state, takes one counter sample, and runs stages until one has to
wait. A waiting stage returns PENDING; every wait has a counter deadline.

1. **Tokens.** One clock-registry token: exclusive PLL_NPU and NPU-local
   scopes, shared fabric, reference and oscillator, and shared PERI0 when a
   fabric clock selects it. One shared DCDC2 rail token from the ledger. One
   stable OSC and PCK lease from the GPU/PCK manager, with its parent lease
   when PERI0 is selected. A busy provider returns PENDING.
2. **Supply.** One acquire load of the power-authority word. DCDC2 verified
   ends the wait; the owner may still be reading DCDC4. Startup init marks
   the owner's supply settled, so the owner verifies at its first step
   ([power domains](power-domains.md#supplies-are-verified-once-per-boot)).
   The deadline is the later of the token step and the PMIC provider's
   startup deadline, plus 5,160 µs, the owner's 100 ms verification budget
   and one tick. A PMIC transfer or domain-4 fault bit, a done bit without
   DCDC2 verified, or an expired deadline refuses.
3. **Register pass and domain 4.** Under one register-lock acquisition: the
   owner's domain-4 check, which reads the PCK600 words once and returns the
   coherent domain state (OFF or ON), and the register pass below. Both only
   read. The lock is released, and the step requests domain 4 ON with
   `a733_power_domain_request(row, 1)`. A domain the pass read OFF must
   reach ON before anything else: the step returns PENDING. A domain the
   pass read ON is already powered, so the step goes on to the PLL while the
   owner adopts the row.
4. **Row.** The power service answers the row; the step returns PENDING
   while it is pending. The deadline is the request plus
   `A733_POWER_PCK_BOUND_US` (10 ms) plus two ticks of service latency. The
   open stage follows only after the ON answer.
5. **PLL enable.** Under a second acquisition, the PLL_NPU method-2 enable
   table ([PLL table](npu-pll.md)); for an adopted PLL one read that must show
   it unchanged; for a PLL locked at the other profile, the vendor's rate
   change. The lock is released, and LOCK is read without it: at most 100
   reads, each at least 3 µs after the previous one, until three in a row
   read locked, then 20 µs (`include/a733_pll_lock.h`). Times are whole
   microseconds, so each gap is 4 µs on the counter and the worst case is
   about 420 µs; the spin does not yield.
6. **Open.** Under a third acquisition, the step requires PLL_NPU locked with
   its gate controls set and opens its output gate. It then routes the shared
   SRAM to the NPU, clearing SYSCTRL `0x03000008` bit 1 with readback while
   the NPU resets are still asserted. The local open table follows, with a
   readback of each write.
7. **Settle.** A counter sample at the end of the open stage sets the settle
   end 2,000 µs later. The vendor platform waits 2 ms after its power and
   clock hook (`vip_drv_device_platform.c:1386-1387`). The step returns
   PENDING while more than one tick remains, and otherwise spins the rest on
   the raw counter, so the settle ends at its counter time.
8. **Final readback.** Under a fourth acquisition, domain-4 policy and
   status ON, the routing bit clear, the bus and reset word, the module word
   and the PLL_NPU control word. The step then opens the register window and
   returns READY.

Domain 4 is ON before any NPU clock or reset changes, the order that runs on
the board's vendor kernel: its NPU driver compiles out runtime power
management, and the platform bus attaches the device to its domain with
power-on before probe ([reference backsolve](reference-backsolve-npu.md#the-vendor-build-compiles-out-npu-power-management)). The sequencer writes no PCK600
register and issues no PMIC transaction; the power-domain owner does both.

### The bootstrap waits on notifications

The sequencer's power-domain row carries a wake, which the power service
calls after it answers the row and after it publishes a supply result. The
native wake notifies the bound bootstrap task. After a PENDING step, the
bootstrap blocks in `ulTaskNotifyTake` for the ticks that
`a733_npu_startup_wait_ticks` gives: up to the deadline for the supply and
row waits, which the notification ends early, and up to the last tick before
the settle ends. The NPU bootstrap loops of the convolution, finite thermal,
NPU board and OS images wait that way; a host model that passes no wake
callback polls once per tick and gets the same results.

In the host model with the PMIC at 400 kHz and domain 4 initially OFF, the
sequencer reaches READY 4,628 modeled microseconds after the scheduler
starts, in 4 calls, each advancing a stage. It makes 61 register reads besides
the LOCK samples (56 by the sequencer and 5 by the owner's domain check), 13
writes and 4 lock acquisitions. With domain 4 ON at entry, as both boards
read it at boot, READY follows at 4,615 µs with the same 13 writes and no
PCK600 write. [The measurements](designs/npu-board-measurements.md#startup-latency-change-by-change)
list each change's effect.

### Two operating points

By default, startup selects the vendor maximum supported at the inherited
800 mV for the SID speed bin. Both populated boards use 852 MHz. Bins that
require another voltage are refused before a write; nothing writes DCDC2.
The supported bin table is documented in the
[operating-point analysis](designs/npu-job-overhead.md#default-operating-point).
`a733_npu_startup_rate`, after init and before task binding, optionally selects
492 or 852 MHz. An explicit 852 MHz selection still requires a compatible bin.
The finite image exposes these options as `--rate-492` and `--rate-852`.

## The register pass refuses before any write

The pass reads each word and compares it under a mask with a constant:

- the oscillator and reference PLL, through the shared 24 MHz reference
  decoder ([clock-leases.md](clock-leases.md#detected-oscillator-and-normalized-reference));
- the AHB, NSI and MBUS words against the fabric record, with no update
  pending;
- when a fabric clock selects PERI0: PERI0 enabled, locked and unmodulated,
  and each selected output's No-Auto route, software enable and effective
  gate;
- the other PLL_NPU consumers: video encode and decode module gates closed and
  their selectors in range, their bus gates closed and resets asserted;
- the NPU module gate closed, its bus gate closed, core, AXI and AHB resets
  asserted and SRAM reset released;
- DRAM0 enabled from DDRPLL with no update pending and its PHY on the
  separate PHY PLL; video and NPU clock-management modes disabled;
- PLL_NPU disabled, preserving its initially observed reserved bit 24 under
  the [PLL rules](npu-pll.md), or adopted as below, with its pattern and bias
  words (`+0x2a4`–`+0x2b0`) at their reset values;
- the shared SRAM routed to the CPU: SYSCTRL `0x03000008` bit 1 set, as the
  boot ROM leaves it.

The domain-4 policy and status come from the owner's check, which precedes
the pass. The pass takes 21 reads, or 26 when PERI0 is selected, and two more
for an adopted PLL.

An inherited ON domain passes only with the same closed gates and asserted
engine resets, and a disabled or adopted PLL. The ON request after the pass
lets the existing power-domain owner adopt the domain and pin its rail
without a PCK600 write.
An OFF domain follows the owner's normal power-on sequence. A mismatched or
transitioning policy/status pair refuses before any NPU write. Neither
state establishes exclusive ownership: the supervised image still needs
the startup confinement, drain and ownership admissions.

The physical read session at
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/bb5-read/session.json`
records domain-4 policy and status as `0x8`. It establishes an observed ON
state, with no launch or transition; it does not establish the remaining
startup conditions. Requiring OFF would refuse that observed state even
if the remaining conditions passed.

### A second start adopts PLL_NPU and domain 4

Local stop leaves PLL_NPU enabled, locked at the profile and with its output
gate open, and domain 4 ON. An image started later in the same power
lifetime finds that state. The register pass adopts PLL_NPU only in exactly
that state: control word `0xf8305100` at 492 MHz or `0xf8104600` at 852 MHz, apart
from the inherited bit 24, with the gate controls at `+0x1920` and `+0x19a0` set. Adoption writes neither the
PLL nor its gate controls; the step still samples LOCK three times. The
vendor clock driver's PLL enable helper also returns without a write when the
PLL's enable, LDO and output gate bits are set (`ccu_gate.c:171-174`). A PLL at the other supported profile instead follows the vendor rate-change
sequence. Any other enabled PLL state refuses before a write. The owner adopts domain 4 ON
as before. In the host model a second start takes 12,001 modeled
microseconds and 6 writes: the routing bit and five local open writes.

## Failure keeps everything after the domain-4 request

A refusal before the domain-4 request releases the PCK lease, the rail
token and the clock token, and returns RETAINED with nothing written. Any
failure after the request returns RETAINED and keeps the tokens, the row with
its request, and the register lock if a table stopped midway. A domain that
does not reach ON keeps every NPU clock, reset and the routing untouched,
except that a domain the pass read ON has already started PLL_NPU: a refused
or failed row then leaves PLL_NPU enabled, possibly locked, with its output
gate closed. A
LOCK that never appears leaves PLL_NPU enabled with its output gate closed.
A routing word that already reads NPU refuses in the pass; one that does not
read back at the open stage retains before any reset is released. Further calls return RETAINED without
device access, and every failure closes the register window. Recovery is
supervised whole-board power removal.

## Local stop after CLOSE

`a733_npu_startup_stop` runs on the bound task after the controller's CLOSE
succeeds. It closes the register window, then under one acquisition checks
the state that startup left and applies the local stop table: module gate
off, bus gate off, then the AHB, AXI and core resets asserted, in the order of
`npu_clk_uninit` in the pinned VIPLite platform. It then routes the shared
SRAM back to the CPU. Each write is read back. PLL_NPU, the SRAM reset,
domain 4 and every token stay held.

Domain 4 rejects an OFF request before publishing it to the owner. Local
stop therefore preserves the admitted row, its rail pin and every lease.
After local stop, `a733_npu_startup_retune` moves PLL_NPU between 492 and
852 MHz in the vendor's rate change while the NPU's gates stay closed and its
resets asserted, as the vendor changes the NPU clock only while no core is
powered ([performance modes](designs/performance-modes.md)). No NPU voltage
transition exists; both points run at 800 mV. Faults retain resources until
whole-board power removal. Normal stopped startup can use the explicit
[restart handshake](#normal-restart-within-one-powered-lifetime) below; ordinary
bindings remain terminal. A later image in the same power lifetime starts again by
adoption. The board image supplies
[thermal admission and orderly stop](designs/npu-board.md#thermal-admission-and-orderly-stop),
and the optional [finite thermal image](../apps/npu-finite/ENTRY.md) supplies
polling admission and bounded cleanup through the same sensor policy.
The supervised OS profile also supplies an NPU client with thermal admission
and an orderly-stop gate in `apps/os/npu.c`. The ordinary numerical image has
no thermal integration. Stopped-PLL retuning has software coverage only;
voltage transitions, in-place recovery and thermal qualification remain open. Repeated startup across finite images,
including 492 and 852 MHz runs with matching outputs, passed in
`board-session-20261025-15/port-3-3`. That evidence does not establish restart
inside a faulted or stopped service instance.

### CLOSE and power-off establish different boundaries

The controller requires completed work, full idle `0x7fffffff`, no pending
interrupt, and readback of disabled interrupt and MMU controls. It checks idle
again after a barrier. The service requires result reap and, when IRQ-bound, route retirement before
publishing CLOSE. The polling finite consumer independently checks GIC IRQ97
quiet before normal return, after local stop gates the engine and asserts its
resets. These checks do not expose a separate A733 interconnect-drain or
power-isolation acknowledgement. PCK policy/status equality establishes the
selected domain state; its interpretation as complete fabric isolation depends
on the SoC's low-power-interface wiring. The generic PCK-600 description does
not identify that wiring for the NPU.

The pinned VIPLite PM state machine waits for all-module idle in its default
parallel-synchronous profile. Its optional asynchronous profile first excludes
the command frontend from the idle mask, then ends the wait/link loop and waits
for full idle. `Kbuild` permits a task-profile override; these source branches
are not evidence of a deployed Linux configuration. The native finite stream
already ends with END and has no wait/link loop. The Linux platform enables
its power wrapper only with `CONFIG_AW_PM_DOMAINS` and a `power-domains`
device-tree property.

The platform calls `pm_runtime_put()` before `npu_clk_uninit()`. In the
[pinned kernel helper](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/include/linux/pm_runtime.h#L454),
that call drops a usage reference and requests asynchronous idle processing
when the usage count reaches zero. An eventual generic-domain suspend may
attempt domain power-off. This establishes no synchronous order between
OFF completion and local clock/reset teardown, and proves neither order unsafe.

The manual's shared SRAM range is `0x00074000`–`0x000f3fff`, the CPU alias of
the NPU's 512 KiB internal SRAM. SYSCTRL `0x03000008` bit 1 routes it to the
CPU or to the NPU; the startup routes it to the NPU and local stop routes it
back ([routing design](designs/npu-sram-route.md)). The launcher and stack lie
below that range.
The FEL scratchpad does not: the boot ROM reports it at `0x000ee800`
([boot.md](boot.md)), inside the range and `0x7a800` bytes (501,760) above its
base. The approved numerical lifetime granted exclusive NPU SRAM/drain
ownership and excluded other CPU, DMA and auxiliary-firmware writers. Its
12×12 model uses the first 101,376 bytes, ending at `0x8cc00`, below the
scratchpad, so that lifetime has no known conflict with it. A future lifetime must carry its own ownership grant.

The ROM scratchpad inside the NPU SRAM alias constrains two later changes:

- **Domain-4 OFF before ROM return.** An image that powers the NPU domain off
  and then returns to the FEL ROM may remove power from the SRAM that holds the
  ROM's scratchpad. It must first establish that the shared SRAM stays powered
  or that the ROM does not depend on the scratchpad contents it left there.
- **NPU SRAM footprints above about `0x7a800` bytes.** A footprint measured from
  the start of the range that exceeds `0x7a800` bytes overwrites the scratchpad.
  The NN pool model needs 518,144 bytes, 16 KiB more than that. An image that
  uses it and returns to the FEL ROM must preserve or restore the scratchpad,
  or must show that the ROM tolerates its loss.

A normal OFF implementation still needs the A733-specific quiescence/isolation
contract and permitted ordering of local clocks/resets against PCK completion.
An observed full-idle value or PCK state alone cannot supply that contract.
A documented interface description or qualified trace exposing those handshake
boundaries would decide the sequence. Until then, keep the row, DCDC2 rail pin,
PLL/local/fabric/reference clock leases and SRAM state retained. Calling OPEN
again does not retire the stopped software owners. The optional normal-restart
consumer below explicitly rearms their per-run state while retaining those
platform-lifetime identities.
The local-stop source audit remains applicable; its identities are in
`npu-lifecycle-audit-20261024`. The service separately bounds pending interrupt
mask, rearm and retirement callbacks while retaining resources on failure
([service deadline](npu-service.md#pending-interrupt-callbacks-have-a-service-deadline)).

### Normal restart within one powered lifetime

The baseline interrupt consumer has an opt-in software composition for two
inference requests separated by CLOSE, local stop and restart. Build it with
`apps/npu-interrupt/build.py --restart --evidence PATH`. Its identity is
`npu-interrupt-restart-return-v1`; it accepts neither the large graph nor the
thermal or checkpoint profile. The [interrupt host](../apps/npu-interrupt/host.py)
prepares and captures this diagnostic using its own exact-image restart admission.
Same-instance physical restart remains unqualified.

The service must be deferred, IRQ-bound and bound to exactly one client. A
successful inference must reach DONE and be reaped before normal READY-to-CLOSE.
Canceled, unreaped and faulted work cannot authorize restart. The worker masks
and retires the interrupt, parks before publishing CLOSE, and makes no further
backend callbacks while parked. The bootstrap proceeds only after the client
collects that reply and all request, reply, deferred and held slots are empty.
Local stop then uses the existing clock/reset sequence and returns SRAM routing
to the CPU. Its ownership grant continues to exclude other SRAM users.

`a733_npu_startup_restart` authenticates the retained clock, rail and PCK tokens,
the registered ON row and stopped register state. It enters the existing
adopted-PLL ENABLE, ROW, OPEN, SETTLE and FINAL stages. The vendor's paired
`npu_clk_uninit`/`npu_clk_init` sequence supplies the local stop/open ordering
(`vip_drv_device_platform.c:265–359,822–886`, pinned revision above). The latter
pair is in the frequency-update branch with `CONFIG_AW_PM_DOMAINS` disabled;
it does not establish deployed Linux PM behavior. This path
never enters the initial PASS refusal path, acquires another token, requests a
domain transition or extends the original deadline. Every failure retains the
row and leases. The existing PLL lock sampling and 2 ms local settle still run.

After startup and metadata admission, the same worker validates that its IRQ
route is stopped, masked, disabled, inactive and pending-clear. It leaves the
route masked and does not clear pending. A single authorized OPEN consumes the
stopped marker inside the routed-core critical section, then performs device
work outside that section. Fresh bootstrap
publication cleans the existing immutable tables and command buffers. The native
memory backend is already IDLE, so its table arena and counter history survive.
The singleton, queues, worker, client ticket counters, row address, wake context,
tokens, deadline and buffer reservations also survive. Only the closed/opened
wrapper state and per-run restart eligibility are cleared.

An interrupt while parked, preparing or awaiting OPEN is a retained failure.
Atomic state transitions prevent bootstrap or worker publication from replacing
that failure. Both tasks check the original absolute deadline. The bootstrap's
failure exit withdraws a pending resume and closes software register admission
before suspension, so delayed worker scheduling cannot revive it. Uncertain
states have no restart path.

The AArch64 layout adds 24 bytes to the service and 32 bytes to the containing
convolution object, including alignment; startup storage and all data buffers
are unchanged. With the native image's compiler flags, the three driver objects
add 3,788 bytes of function text before linker garbage collection. No task,
queue or allocation is added. Ordinary submission adds no translation, job
digest or cache operation. The worker adds an opt-in branch per pass, and an enabled
ISR checks the activation state. Successful REAP records one eligibility bit.
These fixed software costs have not been timed on hardware. The startup model
counts 32 register reads and six local writes over 2,036 microseconds, including
the existing settle, with no PCK or PLL write. A restarted run also pays the
normal bootstrap publication and controller-open costs once.

Focused evidence is under `resume-npu-restart-1026`: sanitizer-backed execution
of the real startup and providers, native FreeRTOS service/controller/cache
composition, and linked IRQ callback/client instruction checks. The native
composition preserves ticket and object identities, copies and reaps both
outputs, and exercises retained interrupts, canceled work and delayed-worker
failures. Device arithmetic, interrupt delivery and register effects are modeled;
the existing numerical fixtures and physical captures remain separate evidence.

### Returning restart diagnostic requires a separate admission review

The returning restart profile runs one trusted catalogue convolution, copies and
reaps its output, completes CLOSE and local stop, reopens the same retained
providers and service, then runs the second inference. It closes and stops again
before the existing provider parking and ROM return. The original 20-second
application deadline bounds the entire sequence. A retained or uncertain state
cannot produce a normal-return receipt.

An immutable 32-byte image marker identifies two runs and four notified
completions. The host checks the marker and linked restart entry points; an
ordinary interrupt image cannot acquire restart authority by changing its
manifest. Built manifests continue to report `physical_admission=false`.

Use `apps/npu-interrupt/host.py session`, `prepare`, `validate` and `run` with
the existing [finite entry procedure](../apps/npu-finite/ENTRY.md). The session
remains 456 bytes with a 232-byte boot payload. The entry schema remains
`a733-npu-interrupt-finite-entry-v1`, with the distinct execution profile above.
In addition to the ordinary finite and interrupt evidence, its `restart` field
references a hashed `a733-npu-normal-restart-entry-v1` record. That record binds
the image, build, target, port, epoch, generation, lease, NPU owner, whole-platform
owner and supervisor. It requires `reviewed=true` and
`restart_qualification="under-test"`.

The record's hashed conditions cover the same providers, worker, service, queues,
buffers and tokens; SRAM exclusion throughout both routing windows; first-job
REAP, CLOSE and local stop before reopening; and the stopped, masked, disabled,
inactive and pending-clear IRQ route before rearming. They also cover the
original absolute deadline, retained domain/PLL/rail ownership and permanent
retention until manual whole-board power removal after uncertainty. The exact
condition names are `RESTART_CONDITIONS` in the interrupt host. These are
source-reviewed entry and ownership requirements. They do not assert prior
physical restart qualification.

The returned log must contain the ordered first CLOSE, restart OPEN and second
SUBMIT stages, four successful notified completions, and the final stopped
receipt. Existing monitor identity, two copied-output comparisons, cleanup
receipt, ROM return and full DRAM-restoration checks still apply. A numerical
mismatch remains a failed numerical result even when retirement succeeds.
The timing field `initial_entry_to_restart_ready_us` includes the first
inference and its stop/reopen sequence; `restart_ready_us` records the final
READY timestamp. It is not an isolated startup measurement.

Evidence is frozen under `~/.cache/sunstoneos/evidence/sol-npu-restart-host-1028/`.
One native returning image, four linked client cases, 54 restart host cases and
22 selected ordinary interrupt host cases pass. The host cases model admission,
transport and completion records; the linked client cases model service replies
and device results. The accepted FreeRTOS service/startup restart evidence above
remains applicable. Against its preserved restart image, executable text and
mutable storage are unchanged; immutable storage grows by 32 bytes. No hot-path
operation, allocation, task, queue, MMIO access or wake-up is added.

A physical proposal still needs current board/lifetime grants, DDR and survey
continuity, IRQ97 state, stable 800 mV supply, exact-image root review and the
restart ownership review. Fresh DDR alone supplies none of the other authority.
Run one supervised RAM-only attempt with manual power recovery available and
stop after the first transport failure. Normal restart, fault recovery, thermal
response and full domain OFF remain separate physical qualification gaps.

## Service activation

The convolution, session and runtime bindings construct the deferred service
with an activation callback that runs on the bootstrap task. The callback
compares the service's copy of the backend operations, MMU image and command
catalogue once, then calls `a733_npu_startup_prepared`. That function returns
0 before READY, 1 when READY with the counter, registry and register lock
healthy, and -1 after any failure or from another task. Activation therefore
publishes the service only after the sequencer reached READY. No startup
call hashes models or command streams.

Both backends also check the startup's register window,
`a733_npu_startup_window_open`, before every NPU register read and write. The
window opens at READY and closes at local stop and on any failure; a refused
access makes no bus access and faults the controller.

Integration follows this order:

1. Construct the counter, registry, locks, GPU/PCK manager, rail ledger, PMIC
   provider and power-domain owner before the scheduler.
2. Initialize the startup, create the bootstrap task at EL1 and bind it.
3. Bind the convolution, session or runtime to the startup and its clients.
4. After the scheduler starts, the bootstrap task calls the binding's startup
   poll once per tick until READY, then `npu_service_activate`.

## Checks

- [`checks/npu-startup-host.c`](../checks/npu-startup-host.c) runs the
  sequencer with the actual PMIC provider, rail ledger, clock registry,
  GPU/PCK manager and power-domain owner over a register model that includes
  the routing word. It checks boot to READY with its counts and its order:
  domain 4's PCK600 command before the first CCU write, and the routing write
  between the PLL output gate and the first reset release. It also checks the
  register window, local stop with the routing restore, four refusals before
  any write, five retentions and seven reference-clock cases. Its `--inherited-on` cases
  check owner adoption without PCK600 writes, six unsafe local-state
  refusals, two incoherent-state refusals, retention after a failed write,
  and retained ownership after local stop and a refused OFF request. Its
  `--second-start` cases run a second startup over the stopped state, with
  the reserved PLL bit 24 clear and set, and ten adoption refusals. Its
  `--retune` cases change the rate of a stopped NPU in both directions,
  with five writes in the vendor order, five refusals before any write and
  a lock failure that retains; `checks/perf-mode.py` runs them.
- [`checks/npu-startup.c`](../checks/npu-startup.c) runs it on two QEMU cores
  with real FreeRTOS tasks and the real counter, registry, ledger and register
  lock. The success mode reaches service activation; the readback mode shows a
  wrong readback retaining the register lock and every token.
- `checks/npu-application-clock-reference.py` drives the linked image's
  sequencer through its register pass for six reference-clock cases.
- `checks/npu-stop-native.py` exercises the linked allowlist adapter's stop
  writes, PLL rules, routing-word rule and PCK600 refusals.
- `apps/npu-finite/sram-route-check.py` runs the finite image's linked
  sequencer and adapter from the register pass through local stop and a
  second start ([routing design](designs/npu-sram-route.md#checks)).
