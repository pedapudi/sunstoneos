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

## Returning OFF-secondary configuration checkpoint

`tools/run-native-startup-probe.py` prepares a fixed diagnostic overlay for the
preserved native ELF with SHA-256
`b6b1f9115cf36644f070407002d5d8b3150e24fa522e6f01f4e04a9390afde50`.
The native code, data, BSS and stack addresses remain unchanged. Twenty-six verified
instruction replacements redirect the C boot entry, walk blocking, the first GIC write,
mailbox admission and the first secondary hardware write,
the first requested control write, both planning failure parks, the terminal stop,
the initial park and sixteen EL3 exception slots. The C entry
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
barrier execute before the next callback is intercepted. The first proposed
hotplug mode request is captured without executing it. No power-request or
reset-release write occurs. SEV does not request PPU power; the admitted OFF
state prevents an executing secondary from observing the command. Actual CPU
activation requires a separate return and quiescence argument.

Every returning path restores saved SCTLR_EL1, executes ISB, restores saved
TCR_EL1 and executes ISB. Exact readbacks must match before ROM return. Restore
state 1 means both originals are saved; state 2 covers every restoration or
verification read and write; state 3 means verified restoration. An exception
in state 2, unknown state or failed readback parks and retains ownership. A late
exception after state 3 re-verifies under state 2. This prevents a faulting
restoration read from repeatedly re-entering the same operation.

After restoring EL1, cleanup first restores the OFF core's reset configuration.
It marks restoration in progress before any MMIO read, requires OFF status and
clear control/mode request bits, then captures the terminal configuration. It
restores entry low, entry high and architecture, with barriers and exact readbacks,
and repeats the OFF/request checks. A changed or uncertain power state retains
the board without configuration restoration; cleanup never powers down a core.
Only verified configuration restoration permits distributor cleanup.

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

Checkpoint 9 captures the first prohibited secondary hardware-write address and
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

The overlay occupies `0x41020000..0x41021000`. Its nonce-bound 256-byte report at
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
Physical configuration writes and actual secondary startup remain unqualified.
