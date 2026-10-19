# Supervised eight-core observer

The separate `a733-smp-supervised` image runs the existing cached eight-core
FreeRTOS diagnostic and USB0 result service without arming a watchdog. It is a
RAM-only bring-up mode. Its native image links and the focused software cases pass;
physical startup, coherent memory and native USB operation remain unqualified.

The diagnostic checks 32 floating-point rounds on each core, per-core background
progress, software-generated interrupts, and at least 32 scheduler ticks on CPU0.
CPU0 freezes a copied snapshot and suspends the diagnostic worker. Its independent
USB task remains scheduled to send that snapshot. The host validates the session,
build identity, fresh challenge, digest, completion mask and every core's result
before recording success. A ROM descriptor or successful USB enumeration cannot
establish diagnostic success.

## Entry and retained state

The image has its own build-input identity, contract magic `0x4153373353554333`,
contract version three and result magic `0x4153373353555231`. It preserves the
224-byte contract layout. In this ABI, `armed_at` means session start. It does
not claim a timer was armed. Startup has a one-second deadline; observation has
a twenty-second deadline. The host stops capture nineteen seconds after entry
begins, allowing time before the software deadline.

Watchdog, FEL-flag and reset-redirect contract fields must be zero. The launcher
and boot monitor require a disabled watchdog and inactive redirects. Neither
writes those registers. The architectural counter-frequency declaration may be
zero, as observed in ROM, or 24 MHz. Other values refuse. Raw counter arithmetic
uses the source-established 24 MHz rate; native entry initializes the declaration.
The existing assembly launcher writes the boot CPU's
reset-vector address and requests its AArch64 transition through RMR. This local
architecture transition is distinct from a hardware-watchdog system reset.

The last six contract words contain a nonzero reservation identity, one zero
word and the complete SHA256 of the reviewed entry-admission record. The launcher
compares that digest with its request before publishing contract magic. The
record binds fresh DDR initialization, the live USB instance, image digest,
physical memory ownership and existing platform-entry evidence. Nonzero fields
and hashes cannot establish physical facts by themselves.

The platform record retains the existing observer's requirements: source-backed
CPU and shared-cache transitions, cold caches and exclusion of dirty aliases,
secure and nonsecure DRAM access, exclusive CPU/memory/DMA ownership, exclusion of
remote writers, USB clock/power/reset ownership, device mapping and physical
counter continuity. Physical coherence is the experiment's result and remains
under test. Independent watchdog expiry, BOOT-pad wiring and automated USB power
control are not entry requirements for this image.

Normal capture does not stop the other cores or return to ROM. At the software
deadline, or on a fatal fault, the monitor records the terminal result and retains
all CPU, register and memory reservations. A primary-core halt can remove USB
service. A recorded secondary-core failure before snapshot publication prevents success. A CPU or
interconnect stall can prevent software deadlines from running. These outcomes
require manual USB reconnection; no independent fault recovery is claimed.

## Host preparation and one attempt

`tools/check-a733-smp-supervised.py prepare` accepts the exact image, its manifest
and an externally reviewed admission file. It provides no usable default physical
admission. The admission schema is `a733-smp-supervised-entry-v1` and contains:

- `image_sha256`, a nonzero 64-bit `reservation`, live `identity` and physical `port`.
- `ddr` and `platform` artifact references with absolute `path` and `sha256`.
  They use the existing DDR-reservation and observer-platform schemas and bind
  the same image, instance and port. DDR initialization evidence is required.
- `recovery` with `mode` equal to `manual-usb-reconnect`, true
  `demonstrated_fel`, `explicit_reconnect_before_resumption`, `preserve_microsd`
  and `one_attempt`, plus hashed `evidence` references. This records the reviewed
  recovery plan without requiring synchronous physical attendance.

Preparation creates a distinct supervised bundle. Launch requires the existing
queue claim and flock, exact software evidence, reconnect permissions and fresh
A733 ROM identification. Queue prerequisites are `supervised-ram-recovery`
and `supervised-observer-software`. The latter records the tested image and
launcher bytes, current host implementations and focused check evidence.

The host reuses sealed uploads and readback verification. It issues one `exe`,
observes the expected USB re-enumeration, and captures a bounded sequence of
complete native replies. It stops at the first transport failure or malformed
reply. Successful capture saves and synchronizes the exact frame before recording
success. After the bounded no-native window, the tool permits one SRAM refusal-report
probe only if the original instance remained unchanged and no transport failure
occurred. A fresh epoch/generation/lease report must identify a clean pre-RMR
refusal. The existing helper then checks that the watchdog is disabled and
restores every backed-up SRAM region with readback verification. Only completed
restoration permits release of the flock and a distinct clean-refusal result;
the queue claim remains with the coordinator for review. The tool does not retry
execution automatically.

An invalid report, report 12 indicating requested architecture transition, failed
transfer or incomplete restoration quarantines the instance and retains the
flock and claim. A successful native capture also retains ownership because CPUs
continue running. These outcomes require actual manual reconnection and
coordinator recovery review before releasing ownership.

The image includes no storage grants, storage application, microSD controller
operation or UFS probe. Preserve the inserted microSD card during manual recovery.
Existing armed-watchdog images and their sealed preparation bundles remain
separate and retain their original admission contracts.

## Focused validation

The changed behavior is supervised admission and completion ownership. The
smallest batch builds this native image, executes its changed boot admission with
modeled architectural I/O, checks supervised SRAM-launcher instructions, and
checks host capture and retained ownership. It also checks rejection of armed
watchdogs, old contracts, missing reservations and changed admission digests.

Existing scheduler, coherence and USB component evidence remains applicable only
within its recorded inputs and conditions. These new cases do not repeat those
workloads or establish physical qualification. A software-only fixture bundle
uses an intentionally nondeployable schema; physical preparation still requires
the reviewed fresh-instance admission record.

The native link and focused checks are preserved under
`~/.cache/sunstoneos/evidence/smp-supervised-20261017/batch1/`. Ten ARM launcher
cases, six contract-admission cases, three linked boot-admission cases, host
capture/ownership cases and fresh-entry-record refusal cases pass. The linked
boot cases enter the changed C boot function with its real stack and zero modeled
RAM; they reuse the unchanged cold-entry assembly evidence. The separate
`checks/software.json` binds reusable software evidence to executable bytes and
host implementations. It supplies no physical admission.

The native image was built once. Successful focused cases were retained while
correcting check fixtures; no existing scheduler or USB workload was replayed.

The focused frequency/refusal follow-up preserves the failed physical image and
its original evidence. It rebuilds only the ARM SRAM launcher, adds zero and
incompatible-frequency instruction cases, and checks clean-refusal restoration,
fresh session identities and retained ownership after failures. The original
all-core image and unchanged component cases are reused. The first physical
attempt's cause remains unconfirmed because it produced neither a native frame
nor a refusal report; the frequency change addresses a separately recorded ROM
metadata mismatch.

The corrected launcher was used for a second physical attempt after confirmed
power removal, fresh DDR initialization and matching entry-control observations.
Uploads, readback and the execution request succeeded. No observer appeared
within nineteen seconds, and the single refusal-report read timed out. The host
stopped communication and retained the board lock and queue claim. Evidence is
under `supervised-reconnect-20261017/`. The failure location remains unknown;
offline comparison with the working single-core startup precedes another attempt.

## Returning A32 launch survey

`tools/run-launch-survey.py` prepares and runs a separate SRAM-only register
survey. It does not launch the native image or write a reset, cache, MMU, power
or watchdog control. The root coordinator alone executes it under the existing
per-port lock and a reviewed queue claim.

The survey uses `0x85000..0x85400` for code and its inline register save area,
and `0x86000..0x86080` for a 128-byte report. These intervals reuse the returning
DDR wrapper and mailbox workspace. They exclude the live FEL image at
`0x47000..0x4a000` and the loader scratch interval at `0x71000`. It never touches
the caller's stack. The host backs up both complete intervals, checks immutable
upload readback, executes once, copies the nonce-bound report, then restores and
verifies both SRAM intervals. A successful report read and restoration establish
ROM responsiveness; a queue claim still requires coordinator review.

The report captures CPSR, SCTLR, MPIDR, CNTFRQ, RMR, two physical-counter reads,
SP, LR and the caller's general registers. SCR is explicitly unavailable. Reading
SCR from an unproven exception/security context can trap; the survey omits that
access. RMR is read with the same instruction used by the previously exercised
pinned `reset64` helper, without writing it. The instruction model verifies the
return convention and preservation; it cannot establish physical register-access
permission or real counter progress.

Build with `run-launch-survey.py prepare --build DIR --clang CLANG --lld LLD
--objcopy OBJCOPY`, then run `checks/launch-survey.py --build DIR --output
SOFTWARE.json` with bytecode disabled. A physical `run` requires `--build`,
`--software`, `--fel`, `--database`, `--claim`, `--owner` and a fresh `--evidence`
directory. The queue prerequisites are `supervised-ram-recovery` and
`launch-survey-software`; submitted artifacts include the exact manifest and
binary. On the first transport error, instance change or uncertain SRAM state,
the runner stops all traffic and retains ownership for manual recovery review.
It never retries or automatically reconnects.

## SCR access before the local transition

The failed physical attempt reached neither a native USB result nor a readable
refusal report. The old assembly launcher read SCR before initializing a report.
The returning DDR wrapper recorded CPSR mode `0x13` (SVC), rather than Monitor
mode `0x16`. SCR access depends on the exception and security context; a read
can trap before the launcher reaches its return path. The previously exercised
pinned `reset64` helper reads RMR and requests the local transition without
reading SCR. This difference identifies a concrete possible early failure,
but does not prove where either failed physical attempt stopped.

The corrected launcher omits only the SCR read and its associated NS-bit test.
It preserves the SCTLR, endian, SVC-mode and primary-MPIDR checks, the full
contract admission and the RVBAR/RMR sequence. The native entry still checks
`CurrentEL == 12`, primary topology and disabled MMU/cache state before using
its stack or accessing memory. EL3 execution establishes its secure exception
context; the boot monitor repeats admission before CPU/coherence startup.
Reviewed secure/nonsecure memory authority and every other external prerequisite
remain required. A returning survey with the remaining gates satisfied justifies
one reviewed corrected launch; a failed gate or transport stops that sequence.

The physical returning survey passed on the recovered board. It recorded all
remaining launcher predicates as true, copied the report and verified full SRAM
restoration. The subsequent SCR-free launch still produced no native observer;
its single refusal-report read timed out. The host stopped communication, and
the user then removed power and reconnected. Evidence is under
`supervised-reconnect-20261017-072/`. SCR omission does not establish a fix for
the observed startup failure. The next diagnostic must locate the failing stage
without repeating the full launch unchanged.

## Returning A64 entry capture

The SCR-free full image also failed to expose a native service. SCR access was
a plausible early trap, not a confirmed explanation. The next diagnostic is
`boot/a733/a64-entry-probe.S`, a separate stackless entry capture that stops before
coherence setup, GIC changes or secondary startup. It uses the pinned FEL
`reset64` implementation and the previously exercised A64 probe's exact return:
RVBAR `0x20`, high word zero, then `RMR_EL3 = 2`. It installs EL3 exception
vectors early and captures synchronous fault registers before the same return.
It does not introduce a watchdog or another reset destination.

The probe records initial `CurrentEL`, MIDR, MPIDR, CNTFRQ, CNTPCT, RVBAR,
SCTLR_EL3, SCTLR_EL1, SCR_EL3 and the unused reset SP. SCR is read only after
`CurrentEL == 12` is established. It reports the stack, monitor BSS, page-table
and EL1 BSS addresses checked against the unchanged native ELF. Those addresses
are layout metadata; reporting them does not test every referenced memory page.
The probe writes only its 256-byte result and the established return controls.

A completed returning A32 survey supplies the pre-transition counter sample.
Its exact session JSON is reviewed as an input and must match the current USB
identity and physical port. The post-transition sample includes host preparation
and upload time; their difference is not a measurement of pure RMR latency.
Inherited SCTLR_EL1 bits are reported separately from required EL3 invariants.
The working single-core monitor initializes that inactive bank explicitly; a
set bit alone does not establish an unsafe active EL1 regime.

`tools/run-a64-entry-probe.py prepare` requires the preserved observer ELF and
explicit build/tool paths. Run the single `checks/a64-entry-probe.py` instruction
case, then submit the exact manifest, binary, software result and prior A32
survey session for review. Physical execution requires the queue prerequisites
`supervised-ram-recovery` and `a64-entry-probe-software`, plus a recovery record
bound to the same live instance. The host waits only on passive enumeration
until a new FEL instance appears, verifies it, copies the report, and restores
the overwritten DRAM code/result bytes. Any transport error or missing return
stops requests and retains ownership; there is no full-image retry.

The known secure-timer recovery covers the demonstrated EL1 stall. It does not
protect EL3 code with exceptions masked, cache/coherence changes, a stopped
interconnect or all-core startup. Normal and captured-exception return follow
the prior physical mechanism; an arbitrary hardware stall still needs manual
recovery. Further staging must retain early observation and return checkpoints
before attempting those changes.

The physical A64 capture passed on 2026-10-17. It reached EL3 without an exception,
recorded both SCTLR banks as `0x00c50838`, and satisfied every captured entry
predicate. The counter advanced; CNTFRQ remained zero. ROM returned on USB instance
`003:074`, and both overwritten DRAM spans were restored and compared. Evidence
is under `a64-entry-probe-20261017/physical-073/`. The initial EL1-state hypothesis
is excluded for this run. The native initialization checkpoint below extends
this physical evidence through stack setup and admission.

The following returning A32 survey on the same warm-returned ROM instance found
SCTLR `0x00c51838`, with instruction caching enabled and MMU/data caching disabled.
Its caller SP was `0x000f2200`. SRAM restoration and ROM response passed. Evidence
is under `startup-stage-20261017-074/survey/`. The launcher requires instruction
caching disabled; the earlier cold-entry survey cannot authorize this warm state.
The unchanged returning cache-off helper subsequently ran at fresh SRAM address
`0x73000`. Same-instance restoration and a follow-up survey passed, establishing
every launcher predicate before the native initialization checkpoint.

## Returning startup diagnostics with failure observations

`tools/run-native-startup-probe.py` prepares a fixed diagnostic overlay for the
preserved native ELF with SHA-256
`b6b1f9115cf36644f070407002d5d8b3150e24fa522e6f01f4e04a9390afde50`.
The native code, data, BSS and stack addresses remain unchanged. Thirty-one verified
instruction replacements redirect C entry, walk blocking, the first GIC write,
mailbox admission, secondary wait, RELEASE, excluded GIC writes/readiness and the
next-core loop. They also redirect secondary hardware writes, requested control
writes, both planning failure parks, terminal stop, initial park and sixteen EL3
exception slots. The C entry
replacement records the initialized stack and replays its displaced instruction.
The original stack selection, BSS clearing, boot-state validation and admission
checks execute. Cache preparation then checks core identity and deadlines and
constructs five translation-table pages. Three publication calls clean the native
code, data/BSS and tables to the point of coherency with `DC CVAC` and `DSB SY`.
Native CPU/DSU identification and planning then execute. A guard at `0x41004440`
captures the first proposed implementation control or patch write before it
executes. Both native preparation failure parks return their diagnostic state.
The complete native `a733_cached_block_walks` sequence executes after its original
SCTLR_EL1 and TCR_EL1 values have been saved. It sets TCR_EL1.EPD0/EPD1 and
SCTLR_EL1.M while EL3 remains untranslated. Native GIC CTLR/TYPER reads and
security admission follow. Before the first GIC store at `0x410003d0`, the
diagnostic captures CTLR, TYPER and every SPI-enable word. It requires both
affinity-routing bits already set, DS clear and no pending distributor write.
The native proposed CTLR must preserve the saved non-group bits. Only a complete
snapshot permits the displaced store to execute. Native distributor setup then
runs through its final CTLR verification. The first secondary's power and entry
controllers initialize and validate their linked allocations. A pre-arm gate then
requires OFF power status, clear hotplug control/mode request bits, and another
OFF read within the startup deadline. This gate precedes every command write
and SEV. It requires fresh external evidence that all secondaries are OFF and
that no other firmware or power writer can change those resources; checking
the target alone does not establish those conditions.

After admission, the original callback publishes its ARM command and SEV. Native
power preflight reads OFF status, control, mode and reset architecture. The
guard at `0x4100a1e4` admits exactly three core-1 configuration writes in order:
reset-entry low `0x08002004 = 0x41002000`, reset-entry high `0x08002008 = 0`,
and architecture `0x08002000 = original | 1`. Each callback retains its native
privilege/address validation and store. Native readback checks and the publication
barrier execute before activation. The callback then admits the native mode-on,
control-on, control-clear and mode-clear writes, preserving all original non-request
bits. Every other callback remains intercepted. An activation marker is stored
before the first mode-on attempt, so an uncertain or faulting write retains ownership.
SEV itself does not request PPU power; the pre-arm OFF condition excludes an
executing secondary before the native power-on sequence.

Every returning path restores saved SCTLR_EL1, executes ISB, restores saved
TCR_EL1 and executes ISB. Exact readbacks must match before ROM return. Restore
state 1 means both originals are saved; state 2 covers every restoration or
verification read and write; state 3 means verified restoration. An exception
in state 2, unknown state or failed readback parks and retains ownership. A late
exception after state 3 re-verifies under state 2. This prevents a faulting
restoration read from repeatedly re-entering the same operation.

After restoring EL1 and verifying shutdown, cleanup restores the OFF core's reset configuration.
It marks restoration in progress before any MMIO read, requires OFF status and
clear control/mode request bits, then captures the terminal configuration. It
restores entry low, entry high and architecture, with barriers and exact readbacks,
and repeats the OFF/request checks. A changed or uncertain power state retains
the board without configuration restoration; cleanup never powers down a core.
Only verified configuration restoration permits distributor cleanup.

After activation, configuration cleanup requires a completed shutdown handshake
and independently observed PPU OFF. The primary restores the saved pre-activation
control/mode words while OFF and verifies exact readbacks plus another OFF read.
An interrupted restore cannot retry. Only this verified state permits the existing
configuration and distributor cleanup.

Every returning path then restores affected distributor state.
Cleanup drains pending writes, disables groups while retaining all original
non-group CTLR bits, and waits for RWP. It clears SPI enables, waits for RWP,
restores the saved enables and checks every word. It then restores the original
CTLR and verifies it after RWP clears. A fresh 100 ms counter deadline bounds
cleanup independently of the native startup lease. A fault, timeout or mismatch
retains ownership without retry or ROM return. A stalled MMIO access still needs
manual recovery because a counter check cannot interrupt it.

The 512-byte GIC snapshot at `0x42002100..0x42002300` is inside the existing
result-arena backup. It holds original SPI enables and their restored readbacks,
original and native-terminal CTLR, TYPER, bank count, cleanup deadline and error.
State 1 permits native writes only after a complete snapshot; state 2 guards
every cleanup operation; state 3 records verified restoration. State 4 means
cleanup failed and the board remains retained. The host durably copies this
snapshot with the other reports before restoring DRAM.

The physical boot CPU is Cortex-A55 r2p0. The pinned TF-A reset code and native
planner request no CPU workaround writes for that revision. Conditional DSU
clock-gating workarounds can still require a control write; the diagnostic captures
its register and value. The pinned A733 vendor monitor uses the same DSU register
encodings at `0x48002814`, `0x48002840` and `0x4800284c`. Its speculative-walk
blocking sequence at `0x4800388c` also changes TCR_EL1 and SCTLR_EL1.M. Those
instructions explain why planning and later activation have separate return
boundaries. The reference is the retained `cpu-startup/monitor.disasm`, derived
from the vendor monitor with SHA-256
`8af16b86ae8e2eff634a3ebca76a8cdf3c10b3540387573d46c3d785545b2632`.

Checkpoint 13 records target CPU preparation or an eligible preparation refusal,
followed by verified target OFF. The observation origin distinguishes those
outcomes. This diagnostic stops before target walk blocking and any redistributor
access. Checkpoint 9
captures a prohibited secondary hardware-write address and
32-bit value, with verified EL1 and distributor restoration. Checkpoint 10
refuses pre-arm admission before command publication or SEV. Checkpoint
6 records a requested register and value before that write; checkpoint 7 records
a native failure park. Checkpoint 5 retains exception registers, including the
faulting instruction address. Checkpoint 8 refuses unsafe GIC snapshot admission
before any GIC write. The host copies the 184-byte native controller at
`0x4200e480` and 128-byte core record at `0x4200e080`. They contain identity,
revision plan, poll index, phase and native error state. Both intervals already
belong to the backed-up monitor BSS. Unknown DSU revisions and register-access
failures remain observed outcomes rather than inferred board properties.

The copied GIC snapshot's unused tail holds the 64-byte pre-arm record at
`0x42002240`: core, refusal reason, initial power status, control, mode, repeated
status, gate state and counter. State 1 admits the original ARM callback;
state 2 refuses it. Reasons distinguish invalid core, non-OFF status, active
control/mode request, changed status or expired/regressing time. The host also
copies the first power controller, entry controller and mailbox from monitor BSS.
Their poll phase and error identify native admission or preflight failure.
The 128-byte configuration record occupies `0x42002280..0x42002300`, immediately
after the pre-arm record. Sixteen 64-bit fields have byte offsets 0 through 120:
state, core, base, original architecture/low/high, terminal architecture/low/high,
restored architecture/low/high, error, admitted attempts, restore status and packed
restore requests. The packed word holds control in its low 32 bits and mode in
its high 32 bits. State 1 requires all originals saved; state 2 guards every
cleanup read/write; state 3 means verified restoration; state 4 records failure.
An admitted attempt is counted before native execution so a partial-write fault
still triggers cleanup. Native `writes_attempted` also counts intercepted requests;
four attempts can describe three completed configuration writes and a blocked
mode request.

Full distributor setup sets GICD_CTLR.ARE_S/ARE_NS. Clearing either enabled bit
is architecturally unpredictable, so restoring an arbitrary saved CTLR is not a
valid general rollback. The [GIC specification](https://documentation-service.arm.com/static/601412d54ccc190e5e681269),
section 2.3.3, establishes this restriction. The existing
[single-core monitor cleanup](../platform/a733/monitor.c) writes its saved CTLR
after enabling affinity routing. Its cleanup is a pending correctness issue
when that write would clear either ARE bit. This diagnostic admits distributor
writes only with both ARE bits already set and preserves them during cleanup.

Publication covers `0x41000000..0x41200000`, `0x41200000..0x41290d10` and
`0x421f0000..0x421f5000`. It visits every 64-byte line in order; the last data
line ends at `0x41290d40`. The helper reads architectural state, line size and
the counter. It checks deadlines around maintenance and after each final barrier.
It never writes cache-enable or translation controls or invalidates data.
Publication retains the existing admission requirement for owned memory and no
stale dirty cache aliases. Cleaning does not independently establish that condition.
A stalled maintenance instruction or barrier still needs retained ownership and
manual recovery; a counter check cannot interrupt it.

The overlay occupies `0x41020000..0x41022000`. Its nonce-bound 256-byte report at
`0x42002000` is outside the native result structure. It records C entry, stack
addresses, terminal checkpoint, architectural state and counter, native refusal
arguments, or exception slot and fault registers. Initial-state differences and
native refusals remain report data. A wrong exception level records its checkpoint
and parks because the demonstrated EL3 return cannot run there. Each returning
checkpoint uses RVBAR `0x20`, high word zero and `RMR_EL3 = 2`.

Physical execution requires the unchanged corrected A32 launcher with SHA-256
`2b0641c2509963ad08d03e8498ee865b5d858c4536b9ea26fbd26ca576e698ea`, a
fresh admission bundle, a completed survey for the current ROM instance and a
reviewed queue claim. Every actual launcher gate must pass. Warm ROM may enable
the instruction cache; a survey with that bit set does not authorize execution.
The existing returning cache-off helper is a separate reviewed operation. It
requires disabled MMU and data cache before clearing the instruction-cache bit.
A subsequent survey must establish the resulting state.

The actual A32 launcher saves sixty bytes on its ROM caller stack before using
its private stack. The host validates the surveyed SP and rejects overlap with
its explicit SRAM reservations. It durably preserves those sixty bytes as
evidence, together with every SRAM upload interval and every DRAM interval the
diagnostic writes, including `0x421f0000..0x421f5000` for the five table pages.
After warm return, the new ROM owns SRAM; the host never
restores the obsolete caller stack or other old SRAM bytes into that instance.
It verifies the returned ROM service, durably copies the diagnostic, native
result, admission, table, controller and core records, then restores and verifies the
affected DRAM.
Any transport failure or absent return retains ownership and stops requests.

Prepare the overlay with `run-native-startup-probe.py prepare`, using the same
explicit tool and observer arguments as the stackless entry probe. Run
`checks/native-startup-probe.py` with `--build`, the preserved corrected
`--bundle`, `--output` and `--objdump`. The physical runner additionally requires
a fresh `--bundle`, `--survey`, `--software`, `--database`, `--claim`, `--owner`,
`--fel` and `--evidence`. Queue prerequisites are `supervised-ram-recovery` and
`native-startup-software`; submitted artifacts bind the exact overlay manifest,
software record, survey session and fresh bundle manifest.

One overlay build and six focused instruction cases pass: normal native
initialization, invalid contract, changed watchdog state, an injected BSS-write
exception, initial cache rejection and a wrong exception level. These cases use
the patched native instructions with modeled architectural registers and MMIO.
They establish the observation boundaries and write restrictions, not physical
memory operation or recovery. Evidence is under
`~/.cache/sunstoneos/evidence/native-startup-probe-20261017/`. The physical result
below applies to the reviewed initialization stage; later startup remains unresolved.

The native initialization checkpoint passed physically on 2026-10-17. The actual
launcher, stack setup, BSS clearing and admission reached checkpoint 2 before
cache preparation. No refusal or exception was recorded. The report captured
stack top `0x42014000`, terminal SP `0x42013f50`, CNTFRQ 24 MHz and both SCTLR banks
as `0x00c50838`. The board returned automatically as USB instance `003:075`.
Copied reports and all reviewed DRAM restoration comparisons passed. Evidence
is under `startup-stage-20261017-074/native/`. Subsequent physical checkpoints
extend that evidence below; multicore operation remains unqualified.

The table-construction checkpoint also physically passed once. All 41 host
transactions succeeded, the diagnostic returned to ROM, and affected DRAM was
restored. The five captured pages match the instruction model byte for byte;
their SHA-256 is
`771a13a8250b68bb2b76ccf2cdac5696adb2f703f5b2bbc767b26b8b7c8b29bf`.
Evidence is under `~/.cache/sunstoneos/evidence/startup-stage-20261017-075/native/runtime/`;
the table instruction evidence remains under `native-tables-probe-20261017/`.

The complete publication phase physically passed once. All 41 host transactions
succeeded, the diagnostic returned as USB instance `003:077`, and affected DRAM
was restored. Both SCTLR banks remained `0xc50838`; the table pages retained
their modeled hash. Evidence is under
`~/.cache/sunstoneos/evidence/startup-stage-20261017-076/native/runtime/`.
Its focused instruction evidence remains under `native-publication-probe-20261017/`.

Native CPU/DSU planning physically passed once. The controller reached PREPARED
without a requested write or refusal; the core record reached state 1. The DSU
revision byte was `0x41` and every workaround mask was zero. All 43 transactions
and DRAM restoration passed, returning as USB instance `003:078`. Evidence is
under `~/.cache/sunstoneos/evidence/startup-stage-20261017-077/native/runtime/`;
its four focused instruction cases remain under `native-planning-probe-20261017/`.

The walk-blocking and GIC-read checkpoint physically passed once. Saved and
restored SCTLR_EL1 were `0xc50838`; TCR_EL1 was zero. Native blocking produced
SCTLR_EL1 `0xc50839` and TCR_EL1 `0x800080`. CTLR read `0x37`, with both ARE
bits already set and DS clear; TYPER was `0x7b0408`, identifying eight SPI banks.
The diagnostic made no GIC write, verified EL1 restoration, restored DRAM and
returned as USB instance `003:079`. Evidence is under
`~/.cache/sunstoneos/evidence/startup-stage-20261017-078/native/runtime/`.
Its completed instruction cases remain under
`native-walks-gic-read-probe-20261017/restore-fix/` and its linked earlier evidence.

The complete distributor stage physically passed once. Original CTLR was
`0x37`, native terminal CTLR was `0x33`, and all eight saved SPI-enable words
matched their restored readbacks. Only bank 5 bit 28 was enabled on entry.
EL1 and GIC restoration both verified; all 44 transactions and DRAM restoration
passed, returning as USB instance `003:080`. Evidence is under
`~/.cache/sunstoneos/evidence/startup-stage-20261017-079/native/runtime/`;
its five instruction cases remain under `native-distributor-probe-20261017/`.

One affected build and three first-secondary instruction cases pass: admitted
ARM and native preflight before the first hardware write, non-OFF refusal before
command publication/SEV, and a native power-status read exception after admitted
ARM. The cases check raw gate fields, mailbox contents, proposed address/value,
controller phases and zero secondary hardware writes. Register reads, event
execution, GIC completion and cache maintenance are modeled; external ownership
and physical OFF stability require the current board evidence. Artifacts are
under `~/.cache/sunstoneos/evidence/native-secondary-preflight-probe-20261017/`.
The first-secondary preflight physically passed once: stable OFF admission,
ARM publication, native reads and intercepted reset-entry write all completed.
The proposed address/value were `0x08002004 = 0x41002000`; power phase 4 reported
one attempted callback with zero physical CPU writes. EL1/GIC restoration, all
47 transactions and DRAM restoration passed, returning as USB `003:081`.
Evidence is under `startup-stage-20261017-080/native/runtime/` in the same cache.
The fresh 21-word all-secondary-OFF record is under that stage's `cpu-state/`;
continuous exclusive ownership and the zero-CPU-write return preserve its relevance.

One affected build and five instruction cases pass through the three configuration
writes and stop before the mode request. The cases cover successful restoration, native entry
readback refusal, an exception after a partial configuration write, changed power
before cleanup, and a cleanup-read exception. The latter two retain ownership
without retry or ROM return. Artifacts are under
`~/.cache/sunstoneos/evidence/native-secondary-configuration-probe-20261017/`.
The configuration stage physically passed once. All three admitted writes completed:
terminal architecture was 1, entry low was `0x41002000`, and entry high was zero.
Their original and restored values were all zero; status remained OFF `0x100`
with clear requests. Checkpoint 9 intercepted `0x07050224 = 1` before mode-on.
All 47 transactions, EL1/GIC restoration and DRAM restoration passed, returning
as USB `003:082`. Evidence is under
`~/.cache/sunstoneos/evidence/startup-stage-20261017-081/native/runtime/`.

The earlier activation diagnostic stopped at native `release`, before any RELEASE command
or secondary continuation. Native power-on must finish, native ARRIVED validation
must pass, all seven permitted callbacks must have run, and live PPU status
must be ON with clear requests. The primary then publishes a separate shutdown
request bound to the current epoch and waits under a fresh 100 ms monotonic deadline.
The secondary's original wait entry redirects to a stackless diagnostic loop.
It validates the native command, core 1, Cortex-A55 r2p0, EL3 and disabled caches
before accepting the matching shutdown request.

The target follows the pinned A55 power-down hook: set
`CPUPWRCTLR_EL1` (`S3_0_C15_C2_7`) bit 0 and execute ISB. It clears its
power-mode bit 0, sets control bit 1 to disable GIC wakeup, publishes its
final acknowledgment, and enters a terminal DSB/WFI loop without further memory
loads. The primary requires both matching acknowledgment and actual PPU OFF.
Acknowledgment records intent; it does not establish quiescence. ARRIVED, FAULT
and WFE alone never authorize ROM return or DRAM restoration.

The platform sequence is selected for A733 by pinned TF-A revision
`5bc9605cbfc175276920c3204658c025da3ae279`,
[common native power management](https://raw.githubusercontent.com/InfernoEmbedded/arm-trusted-firmware/5bc9605cbfc175276920c3204658c025da3ae279/plat/allwinner/common/sunxi_native_pm_ncat2.c).
This BSD-3-Clause source remains a host-side reference. Its CPU-off callback clears
power-mode bit 0 and sets control bit 1.
The pinned Cortex-A55 assembly power-down hook sets only CPUPWRCTLR bit 0
and executes ISB. The retained vendor monitor independently uses the same
operations at `0x480084bc`, `0x48002aa0` and terminal WFI at `0x48003790`.
The unreleased native secondary changes DAIF, TPIDR_EL3 and VBAR_EL3 and writes
its RAM report; it has not enabled caches, changed SMPEN, or configured GICR/ICC.
No additional coherence or interrupt-controller rollback is inferred.

A separate 256-byte record at `0x42002400` fits the existing result-arena backup.
Its 64-bit fields at offsets 0 through 160 are state, epoch, request, acknowledgment,
core, initial control/mode, last PPU status, deadline, target original/set PWRCTLR,
target mode/control, restored control/mode, error, target state, EL, MIDR, SCTLR
and MPIDR. Remaining bytes are reserved. State 1 means activation attempted;
state 2 means shutdown requested; state 3 means acknowledgment and OFF observed;
state 4 guards restoration; state 5 means request-word restoration verified.
Every post-activation fault or uncertain state retains ownership. No power-down
rollback is attempted by the primary. The host durably copies this record before
restoring DRAM after ROM return.

Five focused instruction cases pass with interleaved primary and secondary code:
completed shutdown/restoration, missing ARRIVED, acknowledgment without OFF, a native
post-activation exception, and a control-restoration read fault. Retained outcomes
must contain no configuration/distributor restoration or ROM return. Artifacts are
under `~/.cache/sunstoneos/evidence/native-secondary-off-probe-20261017/`.
Secondary activation, validated ARRIVED and target-owned OFF physically passed once.
Checkpoint 11 recorded activation state 5 with matching epoch/request/acknowledgment.
The target reported EL3, MIDR `0x412fd050`, MPIDR `0x81000100` and SCTLR_EL3
`0xc50838`. PWRCTLR changed from 0 to 1; observed PPU status was OFF `0x100`.
Native power reached POWERED, step 18, with seven attempts and prior ON `0x108`;
the entry owner remained at phase 4 with ARRIVED and an ARMED command.
Original zero control/mode/configuration values, GIC and EL1 state all restored.
All 48 transactions, DRAM restoration and ROM service passed, returning as
USB `003:083`. Evidence is under
`~/.cache/sunstoneos/evidence/startup-stage-20261017-082/native/runtime/`.

The RELEASE extension snapshots the redistributor before publishing RELEASE.
A complete eight-frame TYPER affinity scan resolves core 1; frame order is
observed rather than calculated from its core number. Admission requires
EnableLPIs and RWP clear, WAKER.ProcessorSleep and ChildrenAsleep set, and zero
pending/active SGI/PPI bits. All private interrupt sources must remain quiescent
under exclusive ownership; readback alone cannot establish that condition.
Native continuation disables its three architectural timers. No external private
source or remote SGI writer is admitted. An ineligible snapshot records its
reason and uses the physically proven unreleased-target OFF sequence without
RELEASE or a redistributor write.

On admission, the original RELEASE callback executes. The target selects its
native stack, runs prepare_core and block_walks, and completes native GIC_READY.
A guard admits only the ten exact native continuation writes and rejects a
nonzero pending/active word before its corresponding clear. The shared planning,
block-walk and exception hooks dispatch by actual core; target bank snapshots and
failures cannot overwrite the primary report or execute its cleanup/ROM return.
The target checkpoint at `0x41004eb8` follows its real cached-record state 2
publication. The primary stops after core 1 is DISPATCHED and waits for that
record; it never initializes core 2 or changes the real all-eight-core barrier.
Native cache enable, SCR changes and later ICC setup remain beyond the checkpoint.

Before target-owned OFF, the target restores its redistributor while still ON.
Shutdown enables only target-local ICC_SRE_EL3.SRE/DIB/DFB as needed and disables
Group 0 and both Group 1 enables, with readbacks, ISB and DSB. This shutdown-only
step does not change SCR, EL2 or lower-level SRE. It disables SGI/PPI enables,
waits for RWP, and requires pending/active zero. While disabled, it restores and
verifies the original group, modifier, configuration and priority words, then
restores and verifies the original enable word. It again requires zero events,
restores the originally asleep WAKER state and waits for ChildrenAsleep.
Only verified asleep restoration permits the established target-owned OFF sequence.
The primary requires that evidence, the matching final acknowledgment and actual
PPU OFF before restoring hotplug/configuration/shared distributor state.

This sequence follows pinned TF-A
[gicv3_cpuif_disable](https://raw.githubusercontent.com/ARM-software/arm-trusted-firmware/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gicv3_main.c)
and its redistributor sleep helper. Restoration before OFF avoids assuming that
redistributor registers remain accessible after CPU power-down. Pending or active
interrupts are never reconstructed after being discarded. A fault, unknown state,
regressing counter or timeout retains the target and primary memory reservations.

A 512-byte redistributor record at `0x42002500` holds its admission state, reason,
resolved frame, original and restored affected words, target stack/bank snapshots,
shutdown CPU-interface values and faults. State 1 admits RELEASE; state 2 admits
native writes; state 3 records GIC_READY; state 4 guards cleanup; state 5 means
restored/asleep; state 6 records target failure. A separate 136-byte copy of the
native continuation state occupies offsets 256 through 391, followed by eight
TYPER values and fault evidence. The host copies this record before DRAM restoration.
The two-page overlay reservation remains disjoint from original native memory;
the existing result-arena backup contains the new record.

One affected build and five focused instruction cases pass. The preflight mapping
audits every consumed native field and patched instruction
against the frozen ELF's DWARF and instruction bytes. Five focused cases cover
native RELEASE/GIC_READY/restoration/OFF, awake-redistributor refusal, pending-event
refusal, target restoration fault and ChildrenAsleep timeout. Artifacts and the
compact mapping are under
`~/.cache/sunstoneos/evidence/native-secondary-gic-ready-probe-20261018/`.
Physical RELEASE, GIC_READY and redistributor restoration remain unqualified.


The physical `003:083` GIC_READY attempt did not return. All 27 host commands
exited zero, including `exe`, but no new FEL instance appeared within ten seconds.
The ROM sends the execute acknowledgment before branching to the image, so that
exit status does not prove native entry. No report was available; the runner
retained ownership without another board request. The failure remains unlocalized.
Evidence is preserved under
`~/.cache/sunstoneos/evidence/startup-stage-20261017-083/native/runtime/`.

The newly reached native coherence poll rejects SCTLR_EL1 bits in mask
`0x02001005` at identification index 2, before collecting the target identity.
The successful ARRIVED/OFF stage captured SCTLR_EL3, not the target's reset
SCTLR_EL1. Its instruction model had initialized both banks from the primary.
A different target reset bank or unavailable target DSU read could therefore
retain before the first redistributor write. Independently, the primary's first
TYPER scan and redistributor snapshot had not been physically observed to complete.
These are source-backed observation gaps, not established causes of the failed attempt.

The earlier overlay selected execution profile `core1-release-pre-gic-v1`.
It removes the primary redistributor scan and snapshot. After the existing
core-1 arrival, power and epoch guards, it permits actual native RELEASE.
The secondary selects its reserved stack, publishes RELEASED, installs the
cached exception vectors and disables local timer outputs. Native CPU/DSU
identification and preparation then run under the existing first-write guard.

The target stops at the entry to `a733_cached_block_walks`, before its displaced
instruction or any target EL1 control write. The observation requires a PREPARED
coherence controller, a matching epoch/core record in state 1, and disabled
target caches and translation. It records raw SCTLR_EL1 and TCR_EL1. It reloads
core, epoch and mailbox pointers independently of C register allocation and
validates the actual RELEASE command and RELEASED report. The primary stops
before initializing core 2.

A preparation failure before that boundary is eligible for observation only
before any admitted CPU/DSU or redistributor write. Actual core 1, A55 r2p0,
EL3, disabled EL3 caches, matching command/report epoch and clear power requests
remain required. A capture fault marks its state and parks without repeating
register reads or copies. Unknown states retain ownership.

Both successful preparation and an eligible refusal use the proven private
shutdown handshake. The target requests its own power-off. The primary requires
the matching acknowledgment and independently observed PPU OFF before restoring
control/mode, reset configuration, EL1 controls and distributor state. No
redistributor or CPU-interface cleanup is needed because neither is accessed.
Target observation waits retain on a regressing or expired startup counter;
the OFF wait retains its separate 100 ms bound.

The 512-byte observation at `0x42002700` remains inside the result-arena backup.
Its first sixteen 64-bit fields are state, origin, epoch, core, CurrentEL, MPIDR,
SCTLR_EL3, SCTLR_EL1, TCR_EL1, ESR, ELR, FAR, control, mode, release-admission
state and MIDR. Origin 1 means target preparation refusal; origin 3 means the
prepared target reached the boundary. State 1 guards capture, state 2 publishes
the target observation, state 4 requests shutdown and state 5 records
acknowledgment plus OFF. The raw 184-byte target coherence controller begins at
offset 128; its 128-byte cached record begins at offset 312. The host durably
copies all 512 bytes before DRAM restoration. The old redistributor record is
retained as a zero-access witness: its marker is 1, write count and TYPER slots
remain zero. The runner distinguishes preparation from a returning refusal.

One affected overlay build and three focused instruction cases passed: successful
target preparation with poisoned C callee-saved registers, target SCTLR_EL1
preparation refusal, and a capture read exception that retains ownership. Every redistributor read or write fails
the fixture. These cases model architectural registers, exceptions, distributor
and PPU behavior. Physical release and CPU preparation also pass as described below.
The image, software result and frozen inputs are under
`~/.cache/sunstoneos/evidence/native-secondary-release-pre-gic-20261018/`.
The earlier GIC_READY and pre-write observer images remain immutable under
`native-secondary-gic-ready-probe-20261018/` and
`native-secondary-prewrite-observer-20261018/` in the evidence cache.

The pre-write observer was attempted once after confirmed power reconnection on
USB instance `003:084`. Fresh DDR initialization, returning platform survey and
all seven secondary OFF/request-clear observations passed. Admission used only
that cold-session history; prior RAM state and uninterrupted ownership were discarded.

All 27 host transactions completed, including the launch acknowledgment. No new
ROM instance appeared within ten seconds, and no diagnostic report was recovered.
The failure location remains unknown. The runner retained its lock and claim until
user-confirmed power reconnection; backups remain preserved. Evidence is under
`reconnect-20261018-084/native/runtime/`. Another attempt requires a narrower
observation boundary or a concrete correction, followed by fresh physical recovery.


## Core 1 completes native preparation and returns to ROM

The diagnostic that stops before redistributor access passed on port `3-1` after
fresh DDR initialization, platform survey and all-seven-OFF observations.
Core 1 accepted RELEASE, published RELEASED and reached PREPARED with no
requested CPU workaround writes. SCTLR_EL1 was `0x00c50838`; TCR_EL1 was zero.
The observation recorded a matching core/epoch and no redistributor access.

Matching shutdown acknowledgment and independently observed PPU OFF preceded
shared-state restoration. All 50 host transactions and saved-DRAM comparisons
passed. The runner exited successfully and the queue claim closed. USB ROM
service returned from instance `003:086` as `003:089`. Raw evidence is under
`~/.cache/sunstoneos/evidence/reconnect-20261018-086/native/runtime/`.
This establishes release and preparation on core 1. It does not establish local
interrupt initialization, cache coherence or multicore scheduling.

The [continuation driver](cpu-continue.md) now includes the GIC-600 redistributor
power-up sequence omitted from previous images. Its software checks pass; the
returning diagnostic still needs observation and cleanup for that power change
before physical execution. The earlier failures remain unlocalized.


## ROM reads identify all interrupt frames without changing their state

One read-only batch on port `3-1`, instance `003:089`, returned 25 words:
GICD_IIDR and both halves of GICR_TYPER plus GICR_PWRR for each of eight frames.
The distributor reports `0x0201643b`. Frame `n` reports affinity `n << 8`, and
only the eighth frame sets the last-frame bit. GICR_PWRR reads `n << 8` for each
frame; all power-down permission and group-transition bits are clear.

The batch used the existing port lock and reviewed queue claim. It made no
register, RAM or media writes, exited successfully and closed its claim. Evidence
is under `~/.cache/sunstoneos/evidence/gic600-registers-20261018-089/`.
This establishes accessible identification and power registers in the returned
ROM state. It does not establish SGI/PPI contents or the state after another CPU
power transition. The missing driver handshake remains a required correction,
but these observations do not establish it as the cause of earlier failures.

## Target walk controls and frame reads have a returning diagnostic

The current overlay extends core 1's successful preparation boundary through
its native EL1 walk-blocking helper. It saves the original SCTLR_EL1 and TCR_EL1,
executes the frozen helper, and captures their resulting values. It then reads
only GICR_TYPER and GICR_PWRR at the core-1 frame identified by the ROM survey.
It restores both controls and requires exact readback before publishing the
observation or accepting a shutdown request. Interrupt configuration remains
untouched. The original native image and launcher are reused.

Three focused instruction cases pass under `native-target-walks-frame-20261018/`:
normal observation/restoration, a frame-read exception, and failed control
restoration. The latter two retain ownership without shutdown acknowledgment or
ROM return. Register values, exception delivery and power transitions are modeled;
these checks cannot recover a stalled MMIO transaction. The primary agent reviewed
the helper's disassembly and cleanup.


The physical target walk/frame diagnostic passed on port `3-1`. SCTLR_EL1 changed
from `0x00c50838` to `0x00c50839`, and TCR_EL1 from zero to `0x00800080`.
The target read GICR_TYPER `0x0000010000000121` and GICR_PWRR `0x100`.
Both controls restored exactly. Matching shutdown acknowledgment and observed
PPU OFF preceded shared-state and DRAM restoration. All 50 transactions passed;
ROM returned from `003:089` as `003:090`, the process exited and its claim closed.
Evidence is under `native-target-walks-frame-20261018/native/runtime/`.

This qualifies that bounded observation and return path. It does not qualify
local interrupt initialization or explain the earlier absent reports. Current
ROM survey gates passed before entry, so this attempt reused DDR continuity
without retraining or an additional cache-disable operation.

## Local interrupt state is captured before configuration

The current overlay reads core 1's redistributor control, wake, enable, pending,
active, group, group-modifier, interrupt-configuration and first priority word.
It requires clear power-down permission and equal power-group transition bits
before these reads. Unsupported power state produces a refusal observation;
it does not trigger another power request. A final power-register read accompanies
the snapshot. No interrupt register is written.

The 96-byte snapshot occupies `0x42002900` inside the existing saved result span.
Walk-control restoration, shutdown acknowledgment, observed OFF and shared cleanup
retain their previous requirements. Three focused cases pass for the full snapshot,
a powered-down frame and a power transition. Previous fault/cleanup evidence is
reused. The physical snapshot passed on port `3-1`: control was zero, WAKER
was `6`, and enable, pending, active, group, modifier, configuration and priority
were zero. PWRR remained `0x100`. Both translation controls restored, shutdown
acknowledgment matched, and PPU OFF preceded shared cleanup. All 51 transactions
passed; ROM returned from `003:090` as `003:091`. Evidence is under
`native-local-interrupt-read-20261018/native/runtime/`.

## The returning diagnostic executes the corrected interrupt controller

The current overlay compiles the production portable continuation controller with
the cached image's existing source patch. The frozen native image still supplies
entry, preparation and return code. A source change alone would not update that
frozen image, so the overlay calls the newly linked controller explicitly.
The linker bounds code and constants to a saved 16 KiB reservation and rejects
implicit writable sections. The resulting overlay occupies 8,868 bytes.

Core 1 first captures its local baseline and requires an asleep frame with no
enabled, pending or active interrupts. It enables EL3 interrupt system-register
access with legacy bypass disabled, then verifies both CPU interrupt groups are
disabled. The production controller performs frame discovery, core wake control,
GIC-600 power requests and local interrupt initialization. A rejected power request
rechecks group stability within the original deadline.

On success, cleanup disables local sources, waits for register writes, checks for
pending or active interrupts, restores group, configuration and priority words,
and waits for the frame to sleep. The initially powered frame remains powered.
Core wake control and both translation controls restore before the existing
shutdown acknowledgment and independently observed OFF. EL3 interrupt access stays
enabled until core power-off; interrupt delivery is never enabled. A failed
controller or cleanup retains ownership without acknowledging shutdown.

This diagnostic does not toggle CPU interrupt-group enables or use the priority
mask register. SPI enables remain disabled and LPIs remain off. Those conditions
exclude the SPI/LPI packet transitions described in Arm's
[erratum 2384374](https://documentation-service.arm.com/static/6287e27d41e28926d04306ec?token=),
pages 12–14. General interrupt delivery and shutdown require a separate errata
review when those conditions change.

Five focused instruction cases pass: initialization and restoration, one rejected
power request, refusal of a pending interrupt, retained restoration mismatch,
and retained wake timeout. The actual linked controller executes in these cases;
GIC, system-register and power responses are modeled. Evidence is under
`native-local-interrupt-init-20261018/`.

One physical attempt did not return to ROM. Uploads and the execution request
completed, but no new FEL instance appeared and no result buffer was available.
The host stopped device communication and retained its process, queue claim and
board lock. This result does not identify whether CPU-interface access, controller
initialization or cleanup failed. The model pass does not qualify those operations.
The user subsequently recovered the original board; the second board was not
involved. Returning diagnostics below identified a verification refusal without
repeating the unchanged failed launch.


## A narrower diagnostic stops before the first redistributor write

The optional `prepare --stop-before-power` build runs the same production
controller through CPU-interface access, frame discovery and core wake-control
verification. Its write callback refuses the first GICR_PWRR request before
issuing MMIO. The controller records a retained I/O result from that deliberate
refusal; this is not GIC_READY or a power-up success.

The wrapper verifies that the entire captured local bank and power register
remain unchanged, restores core wake control, and permits the existing control
restoration and shutdown handshake. The record distinguishes the deliberate
stop from an initial-state refusal. Any changed local state retains ownership
without shutdown. This profile performs no redistributor writes and leaves
interrupt delivery disabled.

Two focused instruction cases pass: normal observation with unchanged local
state, and a mismatched local-state read that retains ownership. The overlay
occupies 9,376 bytes in the existing 16 KiB reservation. Evidence is under
`native-before-power-20261018/`. Its unchanged payload passes physically under
`native-before-power-current-20261018/`. Subsequent power and wake/sleep probes
also pass; the [reference audit](a733-multicore-reference-audit.md) records them.


## Core 1 completes local interrupt initialization and restoration

A returning configuration-refusal path preserves the last controller access and
requires complete cleanup before publishing its result. It exposed phase 23,
error 4 and IGROUPR0 readback `0xfff0ffff` under `native-local-config-report-20261018/`.
The earlier full-width equality incorrectly rejected unimplemented PPI bits.

The production controller now verifies required SGI/timer bits while retaining
the all-ones initialization write. Source basis and focused validation are in
[the reference audit](a733-multicore-reference-audit.md). Physical execution under
`native-local-group-mask-20261018/native/runtime/` reaches GIC_READY, phase 26,
with eleven initialization writes and no error. Seven cleanup writes restore
the local bank and wake control. Matching shutdown acknowledgment, observed OFF,
translation/shared-state restoration and saved DRAM comparison pass. ROM returns
as `003:099`; the runner exits and its queue claim closes.

This establishes the core-1 local register sequence with interrupt delivery
disabled. Cached EL1 continuation, delivery, remaining cores, coherence and
FreeRTOS scheduling remain required. Frozen native images that contain the older
controller require an affected rebuild before using the correction; the returning
overlay compiles the current controller explicitly.

## Core 1 adopts and restores the production EL3 cache configuration

The returning overlay can call the frozen production SIF and EL3 adoption
helpers after successful local interrupt initialization. It records original,
enabled and restored SCTLR, TCR, TTBR0, MAIR and SCR values in Device memory.
The bounded cached interval executes immutable code and writes only its Device
report. It does not enter EL1 or access mutable cacheable runtime data.

Cleanup completes Device stores before disabling translation and caches,
invalidates local TLB and instruction-cache state, restores all five controls
and checks each readback. This procedure depends on the absence of dirty
cacheable stores; it is not a general cache-shutdown implementation. A helper
or restoration failure retains the target without shutdown or shared return.
The [A55 manual](https://documentation-service.arm.com/static/649ac6d4df6cd61d528c2bf1),
sections 2.4.8 and 2.6.2.3, supplies the cache and powerdown requirements.

Four focused instruction cases pass: normal adoption/restoration, inherited
SIF clearing/restoration, rejected adoption readback and rejected restoration
readback. The checker validates actual table descriptors for executed accesses
and refuses cacheable stores during the new interval. Register, cache, TLB and
power-controller effects remain modeled. Manifest validation is shared with the
physical runner so its flag checks execute before physical preparation.

Physical execution passes under
`native-el3-cache-20261018/native-attempt/runtime/`. SCTLR_EL3 changes from
`0x00c50838` to `0x00c5183d`; the observed root is `0x421f0000`, TCR is
`0x80823019` and MAIR is `0x44ff00`. All five original controls restore exactly.
Local interrupt cleanup, matching shutdown acknowledgment, observed OFF,
shared-state/DRAM restoration and ROM return pass. The runner exits and its
claim closes; the original board is now instance `003:100`.

One earlier host invocation stopped during manifest parsing before any hardware
access. Its terminal claim was closed, the typo was corrected, and the shared
validator passed nominal and invalid-type checks. The accepted image and four
instruction cases were reused for the single physical attempt.

## Core 1 enters cached EL1 and returns after Nonsecure publication

`tools/run-native-startup-probe.py prepare --el1-cache` invokes the frozen
production EL3-to-EL1 entry and EL1 cache helper. A checkpoint replaces the first
EL1 interrupt-interface configuration, copies system registers into general
registers, and returns through SMC. EL1 makes no ordinary data accesses. The EL3
handler blocks lower-level translation walks, validates the return, restores five
EL1 translation controls and resumes the existing EL3/local shutdown cleanup.
This is a cold-core diagnostic, not a resumable context switch.

AArch64 EL1 translation controls are not separate secure/nonsecure banks.
The handler restores their saved values explicitly. This follows Arm's
[TrustZone register model](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/TrustZone%20for%20Armv8-A.pdf)
and the existing TF-A-based walk-blocking order. Interrupt delivery, EL1 stack
access, mutable cached data, other cores and scheduling remain outside this
checkpoint.

The first physical attempt returned ROM as `003:102` with matching shutdown/OFF
and restored DRAM, but its EL1 report was entirely zero. The copied upload matched
the new binary. Evidence under `native-el1-cache-20261018/` retains that failure.

The corrected diagnostic first creates temporary EL3 mappings at
`0x421f5000..0x421f7000`. Executing code stays Secure Normal-noncacheable; stack
and reports stay Secure Device. An additional nonexecutable Nonsecure alias
permits `DC IVAC` over the immutable code and EL1 table pages. Caches remain
disabled during maintenance. Barriers and instruction-cache invalidation precede
comparison of the changed call instruction through both physical address spaces.
The original EL3 controls restore before production cached entry.

Four affected instruction cases pass, including retention when one Nonsecure
code line remains stale. Physical execution also passes under
`native-ns-publication-20261018/native/runtime/`: 32,768 code lines and 128 table
lines were maintained, the instruction comparisons match, and EL1 returns through
the expected SMC vector. SCTLR_EL1 reads `0x30d01805`, TCR_EL1 `0x200803019`,
and TTBR0_EL1 `0x421f2000`. All five saved EL1 controls restore exactly.
Temporary and production EL3 controls, local interrupts and shared state restore;
matching shutdown/OFF and DRAM restoration complete. ROM returns as `003:103`.

This maintenance admits only known clean immutable aliases from the recorded
startup history. It does not discard arbitrary dirty runtime data or establish
general warm OS replacement. The model checks security-selected maintenance
coverage and actual descriptors; it does not simulate the complete DSU cache.
Interrupt delivery, mutable cached memory, other cores and scheduling remain
required physical work.

## The complete supervised image now publishes Nonsecure aliases

The complete eight-core image includes the corrected required-interrupt mask
and a boot-time Nonsecure publication routine in
`platform/a733-smp-supervised/publication.S`. After Secure publication and the
primary CPU's revision setup and EL1 walk blocking, it temporarily maps Secure
noncacheable code and a nonexecutable Nonsecure maintenance alias. It invalidates
known-clean aliases for code, initialized data, BSS, stacks and both EL1 table
pages, completes instruction maintenance, and restores the original EL3 controls
before any secondary release. The existing exclusion of dirty aliases and other
writers remains required; this does not implement arbitrary warm OS replacement.

The routine adds two temporary table pages within the existing monitor reservation.
All table references use linked symbols. This image has a new layout and must not
use the old returning overlay's fixed patch addresses.

Evidence under `smp-ns-publication-20261018/` records ELF SHA-256
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`.
Four instruction cases cover complete publication, expiry before entry, expiry
during maintenance with restoration, and retained restoration failure. A subsequent
caller-ordering change has one new actual-boot case: all 42,165 required lines
are maintained before the first secondary power-owner construction, with original
EL3 controls restored and EL1 walks blocked. The four routine cases were reused
after matching their executed code, table constructor and linked addresses.
The snapshot preserves 89 compiler inputs and 34 linked-object dependency records.

These checks model architectural registers and cache effects. One additional
complete-image model case reaches cached EL1 on all eight cores with unimplemented
PPI bits modeled as read-as-zero/write-ignored. The ELF was reused unchanged.

One physical attempt subsequently ran the launcher on original-board instance
`003:103`, after a successful returning survey/cache-disable/survey sequence.
The launch command returned successfully and USB disappeared, but no native
observer appeared within the nineteen-second capture window. No failure-location
or scheduler result was recovered. The host issued no further device commands
and retained its process, lock and claim until the subsequent user-confirmed
reconnect. Recovery and the returning diagnostic are recorded below. Evidence is in
`smp-ns-publication-20261018/full-image/`. This does not identify a failed core or
instruction. Do not repeat the full launch unchanged: first distinguish the new
complete-image primary publication path from the later secondary/runtime path
with a source-bound returning diagnostic. All-core physical entry, interrupts,
coherence, scheduling and USB result capture remain outstanding.

## A returning probe isolates complete-image primary preparation

The offline builder `tools/prepare-primary-publication-probe.py` binds one overlay
to the complete ELF identified above. It locates the single direct call from
native boot to `a733_cached_prepare`, verifies the image hash, and replaces that
call with an observer. The observer executes the unchanged production callee.
It returns before interrupt-distributor configuration or secondary-core release.

The observer admits the physically recorded A55 r2p0 and DSU identity, for which
the production revision plan requests no workaround writes. It records original
EL1 and EL3 controls, observes successful preparation, verifies EL3 restoration,
and restores the original EL1 SCTLR and TCR with readbacks. A restoration mismatch
parks without requesting reset. The success path uses the previously exercised
RVBAR/RMR return sequence. Physical return of this overlay now passes on the original board.

The overlay occupies `0x41020000..0x41024000`; its 256-byte report starts at
`0x42018000`, outside native allocated sections and BSS clearing. The builder
checks those reservations against ELF sections and records the patch, payloads,
linked addresses and source hashes. A physical runner must save and restore
these intervals and every native writable/uploaded interval, zero the report,
and supply a fresh nonce. Preparation does not grant board access.

Evidence under `primary-publication-probe-20261018/` records two passing instruction
cases. The first executes actual boot, all 42,165 publication lines, control
restoration and the return request. It permits only the two return-vector MMIO
writes. The second resumes the captured restoration boundary and injects a
readback mismatch; execution retains control without reset, and the host decoder
rejects the report. The shared startup prefix runs once. The instruction cases do not establish physical cache visibility or USB return.

The existing `tools/run-native-startup-probe.py` now accepts this profile through
`tools/primary-publication-host.py`. It verifies the patched bytes against the
original ELF, derives saved intervals from allocated sections, and reuses common
queue, locking, transfer, return and restoration handling. Five focused host cases
cover success, transport failure, absent return, an invalid report with safe DRAM
restoration, and changed-input refusal. A fixture repair preserves the logged
command before parsing its arguments; the earlier successful case was reused.

One physical attempt on fresh instance `003:108` passes. Primary preparation
completes, EL1 controls show the expected blocked walks, all four EL3 controls
match their originals, and both EL1 controls restore with matching readbacks.
The host receives the nonce-bound report, restores every saved DRAM interval,
and confirms responsive ROM as `003:109`. All 43 transactions succeed; the runner
exits and its claim closes as passed. Evidence is under
`primary-publication-probe-20261018/physical-108/`. Later GIC configuration,
secondary startup, interrupt delivery and scheduling remain unobserved in the
full-image attempt. This result establishes primary preparation on the recovered
board; it does not identify the cause of the earlier missing observer.

## Sequential secondary identity survey

The returning survey in `boot/a733/secondary-identity-probe.*` observes cores 2–7
before their interrupt or cache initialization. It extends the demonstrated
primary-preparation overlay on the same frozen complete image. The extension
calls the production power and mailbox controllers, then redirects the fixed
secondary continuation after its RELEASED acknowledgment. Core 1 is excluded
because its entry and shutdown already have physical evidence.

Each target records MIDR, MPIDR, translation controls and cluster identification.
The primary requests shutdown only after matching the production entry report.
The target enables CPU power-down, completes the barrier required by A76 erratum
2743102, requests shutdown and publishes its epoch acknowledgment before WFI.
The primary independently observes OFF before restoring RVBAR, architecture,
mode and control registers. A failed check parks with ownership retained.
The power-down ordering follows the pinned TF-A `cortex_a76_core_pwr_dwn`
implementation referenced in [CPU coherence](cpu-coherence.md).

The combined report occupies `0x42018000..0x42018a00`; the extension fits inside
the existing overlay reservation. The existing host runner saves and restores
both report allocations and validates the completion mask, per-core identity,
matching acknowledgments, OFF and restoration records. It binds the report to
the uploaded epoch and primary nonce. The A32 launcher fills absolute deadlines
at execution, so the uploaded contract template is not a deadline observation.

Evidence is under `secondary-identity-probe-20261018/`. Three instruction cases
cover sequential execution and restoration of all six targets, acknowledgment
without OFF, and restoration-readback failure. Five host cases cover the expanded
report and backup ranges, retained transport failure and rejection of incomplete
results or changed reservations. CPU registers, power states and host transport
are modeled. The subsequent physical attempt passed all six cores on the original
board, returning from `003:109` to responsive ROM `003:110`. All 45 host
transactions passed; saved DRAM restored, the runner exited and its claim closed.

Cores 2–5 report MIDR `0x412fd050` (A55 r2p0), and cores 6–7 report
`0x414fd0b1` (A76 r4p1). The A76 targets have SCTLR_EL3 `0x30c50838` and
SCTLR_EL1 `0x30d50838`. Their TCR_EL1 values differ: core 6 reports
`0x600e12242aa`, core 7 `0x8250e02a402a`. These observations apply to this reset
lifetime. Future launches must read each target's controls again. Every target
reports matching epoch acknowledgment, PPU status `0x100` with OFF state bits,
and configuration restoration. Physical evidence is in
`secondary-identity-probe-20261018/physical-109/runtime/`.

Together with the earlier core-1 result, this establishes production entry and
shutdown for every secondary. It does not establish local interrupt initialization
on the remaining cores, interrupt delivery, mutable cache coherence or scheduling.
The full-image attempt still has no failure-location observation.

## Sequential local interrupt initialization

The returning local diagnostic calls the frozen production CPU preparation and
interrupt controller on cores 1–7, one core at a time. It saves the distributor
control and SPI enables before changing them. Each target saves its actual
translation controls and local interrupt state, initializes its local controller,
then restores that state before acknowledging shutdown. The primary requires
independent OFF observation before restoring the core configuration. Distributor
restoration follows the final shutdown.

The diagnostic keeps CPU interrupt delivery disabled. Initially powered,
asleep local interrupt frames are required. A bounded controller refusal can
return only after complete local restoration; its report remains distinct from
successful initialization. Cleanup failure retains ownership. This procedure
does not qualify powered-off redistributor transitions, interrupt delivery,
cache coherence or scheduling.

One compiled extension and three instruction cases pass under
`secondary-local-probe-20261018/`: all seven targets complete, an injected
configuration refusal returns after restoration, and a cleanup mismatch prevents
shutdown and return. The instruction cases reuse the saved primary startup
checkpoint and exercise A76 r4p1 preparation with the observed per-target reset
controls. Six host cases cover the larger report and backup, restored refusal,
invalid results and transport failure. Hardware responses remain modeled;
physical results follow below.

Physical execution passes on all seven secondaries under
`secondary-local-probe-20261018/physical-110/runtime/`. Every controller reaches
GIC_READY at phase 26 without error, completing eleven initialization writes
and seven cleanup writes. Each target restores its local state and actual
translation controls, acknowledges shutdown and reaches independently observed
OFF before its core configuration restores. The shared distributor and saved
DRAM restore. All 45 host transactions succeed; ROM returns as `003:111`, and
the runner exits with its claim closed. This establishes CPU preparation and
local interrupt initialization for all observed A55 r2p0 and A76 r4p1 targets.
CPU interrupt delivery remains disabled; concurrent execution is not exercised.

## Sequential cached execution on every secondary

All seven secondary cores pass production cached EL3/EL1 entry and return on
physical hardware. The diagnostic runs each target separately after production
CPU preparation and local interrupt initialization. It observes EL1 controls
without data stores, then returns through SMC before enabling interrupt delivery.
The monitor restores each target's original translation controls and local
interrupt state. Matching shutdown acknowledgment and independently observed OFF
precede shared distributor restoration and ROM return.

The extension reuses the demonstrated core-1 helpers with per-core reports and
ELF-derived production table and stack addresses. Four checked branch patches
bind it to the frozen executable. The combined 12 KiB report lies in the Secure
Device monitor window; the host includes it in saved DRAM restoration. Three
instruction cases cover successful execution, incorrect EL1 core identity and
failed EL3 restoration. Six host cases cover capture, expanded backup, transport
failure, incomplete reports, changed inputs and restored early refusal.

Physical evidence is under `secondary-cache-probe-20261018/physical-111/runtime/`.
Every A55 and A76 secondary completes cached entry and SMC return with exact
EL3/EL1 restoration. Local interrupt and shared distributor restoration pass,
as do all 45 host transactions and saved DRAM restoration. ROM returns as
`003:113`, identity `[7,2749,1097072,0,0]`; the process exits and its queue claim
closes as passed. Implementation: local commit `c59967b`.

This result establishes cached execution on each secondary with immutable code
and Device-memory monitor writes. Concurrent shared-memory coherence, interrupt
delivery and FreeRTOS scheduling remain outstanding. Runtime cache shutdown with
dirty Normal memory requires its own maintenance protocol; the diagnostic does
not exercise that case.

## Concurrent cached exchange across all seven secondaries

The returning diagnostic releases all seven secondary cores before waiting for
their completed workload. An early identity record permits the primary to start
the next target; final readiness still requires complete cached execution,
control restoration and local interrupt cleanup. The boot CPU remains at EL3.
Its unused first EL1 stack page provides the shared data area inside native BSS,
so existing initialization, Nonsecure publication and host backup apply.

Every secondary performs 32 exclusive atomic increments, followed by 32 ordered
payload transfers through release/acquire synchronization. The final-store
barrier requires every participant to finish writing before any starts cache
cleaning. Each target executes Nonsecure `DC CIVAC` over all 64 lines and
`DSB SY`, then returns through SMC and the demonstrated restoration path.
After matching acknowledgment and independently observed OFF on every target,
the primary copies the 4 KiB physical page into the Device-memory report.
The host validates every byte, including unused space, against the live epoch.

Three instruction cases cover success, corrupted payload and failed EL3
restoration. Six host cases cover capture, expanded backup, transport failure,
incomplete cleaning evidence, changed inputs and an incorrect physical-memory
copy. The successful instruction case was reused after correcting a fault-case
assertion to allow waiting peers to reach the deadline. No native rebuild or
successful case replay was needed. These models do not simulate physical caches.

Physical execution passes under
`secondary-coherence-probe-20261018/physical-113/runtime/`. All seven targets
complete the exchange, 224 total atomic increments and 224 total payload
transfers. Every target reports 64 cleaned lines; the copied physical page
matches all 4,096 expected bytes. Exact target control/local-state restoration,
shared distributor restoration and saved DRAM restoration pass. All 45 host
transactions succeed. ROM returns as `003:114`, identity
`[7,2753,1097073,0,0]`; the runner exits and its queue claim closes as passed.
Implementation: local commit `464c96c`.

The result covers cached coherent exchange across five A55 secondaries and both
A76 cores with a known single-page write set. The subsequent interrupt result
is recorded below. Boot-CPU participation, general runtime cache teardown and
eight-core FreeRTOS scheduling remain outstanding.

## Seven secondaries handle timer and peer interrupts before restored return

`--interrupts` extends the concurrent cached diagnostic with one timer PPI30 and
one peer SGI0 per secondary. The [reference audit](a733-multicore-reference-audit.md)
records the Secure priority-mask correction, Nonsecure priority scaling and
applicable GIC-600 errata. The stackless handler uses reserved registers and
writes only the existing shared page. The host requires matching delivery counts,
epochs, disabled groups and empty pending/active state in the copied physical page.

Every participant finishes receiving before any disables its timer and interrupt
group. It then verifies quiescence and restores the timer priority and vector
base. The existing final-store barrier and Nonsecure cache cleaning precede
return to EL3. The diagnostic restores the original Secure priority mask before
local cleanup and independently observed OFF. The production source has the same
EL3 mask initialization; the frozen ELF receives it through the diagnostic helper.

Three instruction cases cover normal completion, an unexpected interrupt and
an interrupt left pending during shutdown. The model represents Secure PMR access
restrictions, Nonsecure priority scaling and priority-bit rounding. Six host groups
cover successful capture, retained transport failure, restored invalid reports,
changed inputs, incomplete delivery and nonquiescent interrupt state. These checks
support control flow and report validation; the physical result establishes delivery.

Physical execution passes under
`secondary-interrupt-probe-20261018/physical-114/runtime/`. Every secondary handles
one timer interrupt and one peer interrupt. Interrupt quiescence, shared-page
cleaning and comparison, target restoration/OFF, shared restoration and saved DRAM
restoration pass. All 45 host transactions succeed. ROM returns as `003:115`,
identity `[7,2770,1097074,0,0]`; the runner exits and its claim closes as passed.
No reconnect is needed. General context saving, boot-CPU workload participation,
SPI/LPI shutdown, broader runtime memory cleanup and eight-core FreeRTOS scheduling
remain outstanding.

## Complete scheduler model with secure priority handling

The full supervised image initializes the Secure priority mask before entering
Nonsecure EL1. Its instruction model implements the access restriction,
priority scaling and implemented priority bits described in the reference audit.
The observer shares priority one with background tasks, allowing the
higher-priority diagnostic workers to finish while polling continues.

Both the rejected-mask case and the complete eight-core scheduler case pass
under `smp-secure-pmr-20261018/scheduler-observer-priority/`. Every core completes
32 floating-point rounds, makes background progress and handles inter-core
interrupts. CPU0 handles 333 timer ticks, and the copied snapshot validates.
The model executes actual FreeRTOS tasks and context handlers. It substitutes
USB controller calls and models cache, power and timing behavior; physical
scheduling and USB capture remain unqualified. The exact image, 89 compiler
inputs and 34 linked objects are preserved under the same evidence directory's
`final-snapshot/`. Unchanged admission, launcher and host checks are reused.

The subsequent physical attempt produced no USB observer result. After its
observation window, the sole refusal-report read timed out. Device communication
stopped and the runner retained ownership pending manual power removal.
Evidence: `smp-secure-pmr-20261018/full-image/runtime/`. Full scheduling remains
unobserved; no fault location can be inferred from the missing report. The
Secure priority correction and modeled scheduling success do not establish the
cause of this physical failure.


## Boot-CPU cache adoption and restoration pass physically

The new diagnostic extends the accepted primary-preparation path with the actual
production EL3 cache-enable helpers on CPU0. It stops before GIC initialization
or secondary release, restores the changed controls and returns to ROM. Its stack
and report use Secure Device memory; the observed instruction path makes no dirty
Normal-memory stores while caching is enabled. This bounded restoration does not
implement general OS cache shutdown.

The instruction check executes the original preparation once and reuses that
state for a restoration-readback failure. Normal adoption and exact restoration
pass; the injected failure retains execution without reset. Six host cases cover
the expanded report, RAM restoration, wrong startup epoch, changed reservation,
transport failure and missing ROM return.

Physical execution on the first board passed with the production EL3 cache tables.
The report confirms exact SCTLR, TCR, TTBR, MAIR and SCR restoration. ROM returned
as `003:121`, and saved DRAM restoration passed. Evidence:
`primary-cache-probe-20261018/physical-120/runtime/session.json`.
This result does not establish CPU0 cached EL1 execution, combined startup
barriers or eight-core FreeRTOS scheduling.

Sources are `boot/a733/primary-cache-probe.S`,
`tools/prepare-primary-cache-probe.py`, `tools/primary-cache-host.py` and the existing
`tools/run-native-startup-probe.py`. Checks are `checks/primary-cache-probe.py`
and `checks/primary-cache-host.py`. Evidence resides under
`~/.cache/sunstoneos/evidence/primary-cache-probe-20261018/`; the previously accepted
primary preparation and frozen production ELF are referenced by hash. No native
runtime image was rebuilt for this diagnostic.
