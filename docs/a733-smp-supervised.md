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
