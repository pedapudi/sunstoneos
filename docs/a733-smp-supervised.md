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

## Returning native table-construction checkpoint

`tools/run-native-startup-probe.py` prepares a fixed diagnostic overlay for the
preserved native ELF with SHA-256
`b6b1f9115cf36644f070407002d5d8b3150e24fa522e6f01f4e04a9390afde50`.
The native code, data, BSS and stack addresses remain unchanged. Twenty verified
instruction replacements redirect the C boot entry, the end of table construction,
the terminal stop, the initial park and sixteen EL3 exception slots. The C entry
replacement records the initialized stack and replays its displaced instruction.
The original stack selection, BSS clearing, boot-state validation and admission
checks execute. Cache preparation then checks core identity and deadlines and
constructs five translation-table pages. The normal checkpoint at `0x41004b84`
returns after successful table construction, before publication, cache maintenance,
coherence, GIC or secondary initialization. The first publication call at
`0x41004b94` would otherwise execute `DC CVAC`; it remains outside this stage.

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
result, admission and constructed-table records, then restores and verifies the
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
is under `startup-stage-20261017-074/native/`. Cache preparation and subsequent
startup stages remain unresolved; this result does not establish multicore operation.

One affected build and one focused table-construction instruction case pass.
The case checks table links, secure monitor isolation, table-page attributes and
the permitted write intervals. Architectural reads and admission MMIO are modeled.
No publication, cache-maintenance instruction or translation-control write
executes before return. Evidence is under
`~/.cache/sunstoneos/evidence/native-tables-probe-20261017/`. Table construction
and later cache/coherence startup still require physical evidence.
