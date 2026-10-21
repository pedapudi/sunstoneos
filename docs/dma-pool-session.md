# DMA service buffers remain reserved for the image lifetime

The native DMA pool constructor binds the existing DMA0 service to linker-owned
write-back memory, the common clock registry, the common register mutex and the
physical counter. It prepares fixed RAM-copy jobs for an admitted subset of the
controller's sixteen channels. Hardware operation remains unqualified.

The reusable constructor is in
[platform/a733/dma_pool_session.c](../platform/a733/dma_pool_session.c), with its
contract in [a733_dma_pool_session.h](../include/a733_dma_pool_session.h).
[The copy application](../apps/dma-copy/main.c) is a separate integration entry.
It selects every admitted channel, runs one copy per channel, checks the copied
bytes and untouched padding, reaps each result, closes each channel and suspends.
The service and its resources remain allocated afterward.

## Each job owns complete descriptor and data pages

Construction acquires one parent allocation from the actual write-back pool.
Each job receives three disjoint 4 KiB pages: hardware descriptors, source data
and destination data. The native policy contains separate banks for those roles.
The service adapter admits only a complete registered page with its canonical
CPU address, physical address and role. Adjacent accelerator allocations,
partial-page mappings and altered physical addresses are refused.

The allocation retains its actual owner, slot, generation and complete extent.
Its allocator state becomes `DEVICE` before native adapter construction.
Every device callback checks the permanent parent record and the single admitted
session identity. The adapter uses the existing native address-translation and
cache-maintenance instructions. Its MMIO callbacks require the actual privileged
service worker and completed startup.

The parent never returns to the allocator. Successful completion, result reaping
and channel closure govern a job's buffer access, while the parent remains
`DEVICE`. The controller must observe its stop and drain conditions and complete
destination cache invalidation before the service reports a result. The client
consumes a successful result before submitting `REAP`.

An uncertain drain keeps the controller transfer and service buffer hold live.
The client must not inspect that destination or reuse its pages. A constructor
failure after allocation marks the parent `RETAINED`. Later faults preserve its
permanent `DEVICE` reservation without mutating allocator state.

## Boot construction precedes concurrent use

Call the constructor once at EL1 during serialized primary boot, before the
scheduler releases application tasks. The actual image mapping must already be
established on every participating CPU. All pool allocation and state mutation
occur during this interval. Later readers inspect one permanent slot that has
no writers; they do not invoke allocator APIs concurrently.

The session, callback state, borrowed counter, clock registry and register locks
must retain platform lifetime. They remain private and immutable after
construction, except for their documented owner transitions. Session copies and
changed lease identities are refused. These checks detect accidental misuse;
the current shared EL0 mappings do not isolate arbitrary memory corruption.

The constructor checks identity translations for its inputs and private state,
and excludes the pool from all such storage. Service protection also excludes
the rest of the image and the secure monitor reservation. Other device pools
receive no DMA mapping authority. The complete native mapping, boot image
reservation and absence of aliases remain platform prerequisites.

The designated bootstrap task runs the actual DMA startup state machine. It
borrows the image's common providers, obtains the real DMA0 clock lease and
performs register operations under the shared mutex. Activation requires the
existing authenticated startup callback. A bounded startup deadline includes
activation contention. Only the bootstrap and service worker receive EL1t
through `a733_dma_pool_task_privileged`; application clients remain EL0.

The acquired clock token remains private and permanently held. After activation,
the worker reads the actual atomic counter, registry and register-lock fault
indicators before each device or cache operation. An observed fault publishes a
failed session and invokes the nonreturning fatal handler, preserving every
allocation and active transfer. It performs no token-status retries or device
operations after observing failure. Ordinary registry contention does not alter
the permanent token or interrupt this bounded check.

## The copy image requires actual platform inputs

The application entry requires an externally supplied immutable
`a733_dma_copy_handoff`. The record must describe independently established
remote-user exclusion, prior drain, IRQ masking and watchdog recovery, with a
future absolute deadline. No default handoff is linked.

The cached platform must provide its actual primary preparation guard before
this entry runs. The guard checks that all cores reached the preparation barrier
and the kernel release flag remains unpublished. The dedicated copy image owns
one common counter, registry and lock set. A larger OS instead passes its existing
providers to the reusable constructor.

Central build integration selects this application entry, enables the linker
pool and applies the existing privileged-task patch to a private cached port.
It must rebuild all consumers against the same FreeRTOS and DMA service layouts.
The copy application does not replace the full hardware and mimus requirements.

The standalone target now links the complete cached eight-core boot, scheduler,
pool constructor, privileged bootstrap/worker and EL0 copy client. Configure it
with an explicit source file defining the immutable handoff:

```sh
cache="$HOME/.cache/sunstoneos"
export PATH="$cache/host/usr/lib/llvm-21/bin:$PATH"
cmake -S apps/dma-copy -B "$cache/build/a733-dma-copy" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/aarch64-clang.cmake" \
  -DFREERTOS_DIR="$cache/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2" \
  -DTINYUSB_DIR="$cache/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973" \
  -DA733_DMA_HANDOFF_SOURCE=/absolute/path/qualified-dma-handoff.c
cmake --build "$cache/build/a733-dma-copy" --target a733-dma-copy -j4
```

Configuration refuses a missing handoff source. The file must define
`const struct a733_dma_handoff a733_dma_copy_handoff` and
`const struct a733_dma_copy_usb_handoff a733_dma_copy_usb_handoff` in normal
immutable image storage. Compiling its assertions does not establish the physical facts they
represent. The target supplies no default authority. Physical launch requires the separate
host admission described below.

A private vector patch admits synchronous exceptions and interrupts from the
privileged driver's EL1t context through the existing kernel handlers. The
original cached diagnostic continues to reject that context. FIQ, SError and
unrecognized synchronous exceptions retain their fault paths. Application
tasks return to EL0; this does not add isolation against arbitrary application
memory corruption in the existing shared mappings.

After verifying and closing each copy, the client records its channel bit in
`a733_dma_copy_result`. It records start/end counters and release-publishes
completion after all selected channels finish. The USB service acquires
completion before reading the mask and counters. Partial progress produces a
pending frame; it cannot report a successful subset of the selected channels.

On 17 October 2026, the target linked once with no undefined symbols and a
2 MiB write-back pool at `0x4126b000..0x4146b000`. That link used an explicit
all-zero handoff fixture that grants no authority and must not be deployed.
Seven focused instruction cases on the same ELF passed for the changed
exception routes, stack/register preservation and EL0/EL1t return modes.
They did not execute the boot, complete application or physical DMA.
The ELF hash is
`220156f455bd2a59926f0801fa0b480c02ebd5659e8d502920965d89369f5151`.
Build inputs and results are in `~/.cache/sunstoneos/evidence/dma-copy-image-20261017/`;
instruction evidence is in `~/.cache/sunstoneos/evidence/dma-copy-vectors-20261017/`.

## USB observation binds the completed copy to one boot session

The copy image reuses the existing observer's USB0 programmed-I/O adapter,
TinyUSB configuration, bounded SVC handler and digest implementation. Private
patches copy the admitted boot identity before cache publication, route SVC 120
and select DMA-specific framing. Shared observer sources and its sealed bundle
remain separate artifacts.

The external USB handoff binds epoch, generation and watchdog lease to the
accepted boot contract. It also binds its evidence identifier to the DMA
handoff/evidence identifiers. All four assertion bits are required: exclusive
USB0/PHY/clock ownership, valid device mappings, exclusion of remote users and
independent watchdog/ROM recovery. USB ownership covers the complete CCU words
at `0x02003300` and `0x02003304`; other USB users must remain stopped until reset.
These declarations require independently established evidence. Reading them
cannot establish their physical truth.

The DMA constructor still uses the image's common clock/lock providers. The
USB observer retains its separately admitted exclusive peripheral reservation;
its PIO buffers require no additional DMA mapping. USB initialization runs only
after both handoffs are admitted. Its 150 ms detach and 1 ms reset delays consume
the existing observation interval. No code feeds or extends the watchdog.

The USB task remains EL0, runs on core zero and invokes only the fixed SVC entry.
The service authenticates the task, core, return address, stack extent and saved
privilege state before USB access. DMA bootstrap and worker tasks retain their
existing EL1t route. The shared mappings still require cooperative tasks; these
checks do not provide isolation against arbitrary memory corruption.

The device uses revision `0102`, product `sunstoneos DMA copy observer` and serial
`sunstoneos-a733-dma-copy`, with bulk OUT `02` and IN `81`. Each request carries
command `0x32`, an increasing sequence, boot epoch/generation and a nonzero
32-byte challenge. The 512-byte response payload uses DMA magic
`0x41533733444d4131`. It contains the accepted boot identity, challenge, compiled
build identity, selected/completed channel masks, bytes per copy, controller,
evidence identifiers and counter bounds. SHA-256 covers its first 480 bytes.
The digest detects malformed data; it is not keyed device authentication.

Success requires the exact selected mask after every copied payload and padding
check, result reap and channel close. It makes no claim about eight-core
scheduler diagnostics. The host writes and synchronizes the complete response
before publishing `dma_result: observed-success`. ROM return is recorded
separately. Missing or malformed output cannot be replaced by reading RAM after
reset.

## The host command reuses the sealed compact launcher

[The DMA host command](../apps/dma-copy/observe.py) provides `prepare`, `validate`
and `launch`. Preparation requires an image manifest naming target
`a733-dma-copy`, protocol version 1 and its exact `final_elf` SHA-256. It extracts
immutable DMA/USB records from that ELF and uses their fixed boot identity.
The coordinator must allocate a fresh identity for each admitted boot.

Preparation copies the existing observer launch binary after validating its
bundle, exact launcher sources and unchanged contract declarations. It performs
no launcher build. The DMA image accepts that launcher's compact version-one
contract magic, `0x415337334f424331`; its monitor result and USB frame have distinct
DMA identities. The original sealed bundle is read only.

```sh
python3 apps/dma-copy/observe.py prepare \
  --image /absolute/path/a733-dma-copy.elf \
  --image-manifest /absolute/path/image-manifest.json \
  --launcher-bundle "$cache/evidence/smp-observe-host-20261016/prepared-observer" \
  --output /absolute/new/dma-bundle
python3 apps/dma-copy/observe.py validate --bundle /absolute/new/dma-bundle
```

`prepare --refusal` permits offline preparation with refused handoff flags.
`launch` rejects every such bundle before device discovery. The ordinary
preparation path requires complete DMA and USB declarations, but physical launch
also requires the claimed board-queue artifacts and external evidence.

The launch command requires `--bundle`, `--fel`, `--qualification`,
`--input-evidence`, `--ddr-contract`, `--platform-contract`, `--evidence`,
`--database`, `--claim`, `--owner` and `--confirm-bridge-stable`. Its queue
prerequisites are `independent-watchdog-rom-return`, `stable-protected-fel`,
`ddr-reservation`, `dma-copy-platform` and `dma-copy-software`. The platform record
binds the current USB instance, image, boot identity and DMA/USB evidence IDs.
It includes the inherited cached-entry/USB requirements plus actual DMA startup,
drain and ownership evidence.

The platform record also requires a `dma-nonsecure-access` condition referencing
a digest-bound JSON record with schema `a733-dma-nonsecure-access-v1`. The record
binds identity, port, image SHA256, epoch, generation, lease, controller,
`service_channels`, `dma_evidence` and `dma_handoff` to the prepared image/session.
Its 16-bit `nonsecure_channels` mask must cover every selected service channel.
`reviewed` and `physical_authority` must be true, and `modeled` must be false.
It requires nonempty digest-bound supporting `evidence` and these true assertions:

- `controller_access_nonsecure_el1`: the controller is accessible to the native
  nonsecure EL1 provider.
- `descriptor_data_access_nonsecure`: every descriptor, source and destination
  memory extent admitted by the image is accessible through its DMA/firewall path.
- `valid_after_reset_release`: these permissions cover the selected cold/adopt
  lifecycle, including reset deassertion, operation and recovery.
- `security_writers_excluded_until_recovery`: no external writer can invalidate
  the admitted security configuration before recovery.

The host verifies declarations and identity; it does not establish their physical
truth. Secure-firmware attestation or separately authorized observations must
supply that authority. Missing or modeled security evidence refuses launch before
device transactions. The verified record bytes are preserved with launch
evidence. No nonsecure security-register probe or native rebuild is introduced.

The shared host validator is `tools/dma-nonsecure-access.py`. DMA-copy software
admission also binds that file through its `dma_nonsecure_helper` reference.
The UART DMA, I2S and finite HDMI audio admissions use the same security schema,
referenced by a top-level `dma_nonsecure_access` object with an absolute `path`
and `sha256`. These consumers additionally require `configuration_sha256` over
their complete immutable DMA configuration: UART's 96 bytes at offset 1824,
I2S's 104-byte `dma` field, or HDMI audio's 104 bytes at offset 552.
The admission and every supporting security record are frozen and rechecked;
frozen validation cannot reopen missing evidence from its original path.

UART DMA also requires `session_template_sha256` over the complete 1952-byte
input template. Its independently reviewed security record supplies nonzero
integer epoch, generation and lease values. Preparation validates that record
before adopting those values, and bundle/capture validation checks them against
the actual runtime session. Existing UART records continue to bind the original
template, image, circuit, request and DMA configuration. Only runtime IDs and the
linked image identity may replace template bytes 24 through 79. FIFO UART keeps
its existing random IDs and requires no DMA security record.
Externally assigned IDs do not establish freshness; the existing one-attempt
admission and retained recovery requirements still apply.

I2S and HDMI audio already carry immutable session IDs. Their security records
bind those IDs, the image and exact DMA configuration; their admission digests
then bind the security reference into the session. A full-session digest inside
the security record would create a digest cycle and is not required.
HDMI audio admission also names the reviewed board `identity` and `port`.
Its result decoder remains an offline observation entry and supplies no physical
execution authority. No wire layout, register access or native image changes.
Earlier frozen images and evidence retain their original admission scope.

The focused consumer check passed 30 host cases using the preserved UART DMA,
I2S duplex and HDMI audio images. It covers added authority refusals, UART's
actual ID-adoption producer, and HDMI's evidence loader and frozen bundle.
Model positives replace only explicit modeled/nonphysical decisions; production
refuses modeled security records. Evidence is under
`~/.cache/sunstoneos/evidence/dma-consumer-security-20261020/`.
Declared input digests and imported source bytes were checked before use and
verified unchanged afterward. No native instructions, launcher build, device
access or earlier component matrix ran for this host-only change.

The focused `checks/dma-copy-security.py` check passes eight host cases against
the immutable profile in the preserved `49397dc7…` image. Missing authority,
modeled production evidence, absent channel coverage, differing controller or
session, a boolean mask and missing post-reset validity are refused. Positive
validation substitutes only the physical-authority decision in the checker;
the production path accepts no modeled override. Records are preserved under
`dma0-security-admission-20261020/`. No native, capture, launcher or device case
was replayed.

The command reuses sealed uploads, SRAM preservation, qualification checks,
refusal recovery and quarantine helpers. It holds the existing board flock,
performs one launch, accepts only the protected port's expected USB identity and
stops transport after the first failure. It retains ownership when a live launch
has an unresolved outcome. Successful pending replies allow bounded observation;
they do not authorize retries after a failed transfer.

[The focused check](../checks/dma-copy-observe.py) links the complete compact
image with handoff assertion masks set to zero. Actual client instructions
process modeled completed service replies and verify modeled copies before
publishing their result. The native encoder produces pending and completed
frames for the host capture path. A corrupted payload must reach the existing
fatal path without publishing completion. The fixture also checks new SVC
routing, preserved launcher reuse and offline launch refusal. It does not
execute physical USB, DMA, cache coherence, boot or watchdog recovery.

On 17 October 2026, this observer target linked once and the focused composition
passed on ELF SHA-256
`49397dc736e9b8f47c3a56632b7b0d1f5230404eef006aaa272524585ded4b96`.
The image retains refused handoffs. A fixture stop-hook correction preceded
completion of the instruction cases on that same ELF; no rebuild or completed
case replay followed. Inputs include generated assembly and its generation
sources. The exact original and corrected runners, refusal bundle, captured
frames and tool identities are preserved under
`~/.cache/sunstoneos/evidence/dma-copy-observe-20261017/`.

## Capacity and remaining hardware work

The maximum sixteen-job layout consumes 48 pages, or 192 KiB. Existing prepared
NPU and GPU sessions consume 399 write-back pages, leaving 113 pages in the
initial 512-page pool. Adding all sixteen copy jobs leaves 65 pages, or 260 KiB.
A real CE session consumes another page. These figures describe allocation
capacity; they do not establish safe concurrent device execution.

One 640-by-480 NV16 camera frame requires 600 KiB. Full concurrent camera, video,
DMA and accelerator workloads therefore need additional qualified DRAM
reservations. This constructor does not expand memory capacity or infer the
board's usable DRAM from its advertised size. It explicitly refuses a pool
outside the initial image reservation until that profile's protected-image
ranges are integrated.

The RAM-copy profile uses finite RAM transfers on DMA0. The I2S constructors,
`a733_dma_pool_session_stream_init` and its supervised variant, register I2S0
period rings with the same service owner: for each direction one descriptor page
and two to eight period pages, each admitted only for its own memory role and
the matching FIFO route. A duplex profile registers one such bank per direction
on two channels. The same catalogue serves finite and hardware-linked cyclic
stream profiles, and the controller implements cyclic transmit and receive. The
I2S stream driver disables its DMA requests before any DMA drain. DMA1,
interrupts, IOMMU operation and wider peripheral modes remain part of the
[DMA scope](dma.md). Physical cache coherence, descriptor
fetches, bus completion, external-user exclusion and recovery need board evidence.

## Selected composition check

[The focused runner](../checks/dma-pool-session.py) builds one two-core FreeRTOS
image using the actual linker pool, native address translation, cache operations,
counter, clock registry, shared locks, startup, controller and copied-job service.
Only A733 registers and DMA bus movement are modeled.

One channel completes a copy and returns its buffer through the result/reap
protocol. A second channel retains a busy condition after disable and must
preserve its transfer and destination ownership. The same run checks adjacent
allocation refusal, full-page role checks, lease/copy refusal, the startup
authority requirement and absence of constructor MMIO. It does not rerun the
earlier DMA protocol or scheduler suites.

The final step retains the actual clock registry after activation. The next
worker operation must fail before further DMA or cache work and preserve the
uncertain channel's transfer and buffer hold.

The runner also compiles the production application entry and constructor with
the cached eight-core configuration. Those objects retain the unresolved real
handoff and platform guard requirements. The emulator execution does not qualify
the complete native image, A733 cache coherence or physical DMA completion.

The composed execution and production compilation passed on 16 October 2026.
The selected target built once and executed once, with no compile or runtime
correction. The executed ELF has SHA-256
`c2994c7a9f1de4825051742c0f4160f024dc53db256c53a3e87c76b15fd2feff`.
Input snapshots, build commands, production objects, linker layout and execution
logs are under
`~/.cache/sunstoneos/evidence/dma-pool-session-20261016/batch1/`.
The parent directory records the frozen stream dependency import and its
unused-helper removal before this build. No board or performance operation ran.
