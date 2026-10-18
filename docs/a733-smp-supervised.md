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
writes those registers. The existing assembly launcher writes the boot CPU's
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
A733 ROM identification. Queue prerequisites are `supervised-entry-admission`
and `supervised-observer-software`. The latter records the tested image and
launcher bytes, current host implementations and focused check evidence.

The host reuses sealed uploads and readback verification. It issues one `exe`,
observes the expected USB re-enumeration, and captures a bounded sequence of
complete native replies. It stops at the first transport failure or malformed
reply. Successful capture saves and synchronizes the exact frame before recording
success. Every post-entry outcome quarantines the live instance and retains the
flock and coordinator claim, including a clean launcher refusal. The tool sends
no recovery ROM requests and performs no automatic retry. After actual manual
reconnection, the coordinator reviews recovery before releasing ownership.

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
