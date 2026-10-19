# Native fixed GPU render application

The native application composes real firmware, reserved GPU memory, the native
power backend, the privileged GPU service and USB observation. It submits one
64 × 64 RGBA8 clear per boot. Physical rendering, interrupt delivery, inherited
PLL/rail ownership, the larger RAM reservations and watchdog recovery remain
unqualified. Physical entry requires qualified power, memory and recovery
conditions. Successful rendering and interrupt delivery are outcomes to establish
on the board.

One native USB provider owns the shared locks, GPU resource manager, rail ledger
and PMIC provider. The GPU platform borrows DCDC4 through that same provider.
The application uses the existing large memory profile: cached backing occupies
`0x48000000..0x78000000`; non-cacheable backing occupies
`0x78000000..0x79000000`. The boot path verifies the actual translations and
publishes both native pools before construction. Neither numeric reservation
identities nor software admission prove physical RAM ownership.

The application consumes the existing immutable board session at `0x41f00000`
with radio work disabled. Its version-two boot contract uses the existing large
memory handoff and the GPU contract magic `0x4153373347504331`. The session and
contract must match the image, epoch, generation, lease and absolute deadlines.
The application contract appends the board session's 32-byte SHA-256 digest
immediately after the standard version-two fields. Its byte count includes that
extension. EL3 copies those bytes with the base contract before admission,
then copies the complete session into private storage and verifies its digest.
EL1 consumes that sealed copy; the
version-two reserved words retain the existing RAM ownership metadata. Firmware, compute and graphics
owners are fixed at 9, 10 and 11, excluded from the session's other owners.
The GPU resource-manager client is 7. USB2 and radio tasks are absent.

The EL3 boot path configures GPU interrupt 95 as Group1NS, level-high and routed
to the primary CPU at an API-safe priority. It leaves delivery disabled. The
service owns subsequent masking and rearming. Interrupt dispatch forwards only
that interrupt to the actual service and preserves the existing timer and SGI
handlers. Physical GIC routing remains unqualified.

The client waits for the real PMIC provider, opens the service, waits for firmware
readiness and submits the fixed render. The existing render service owns both
command rings, fragment/geometry timelines, cache invalidation and complete
pixel/guard validation. The application collects all 64 copied output slices
before publishing success. CPU code never fills device output with expected
pixels. A queued submission or a firmware acknowledgement cannot publish output.

All GPU memory, firmware objects, command rings and power leases remain held for
the boot, including successful rendering. There is no implemented physical GPU
transaction isolation provider. The native backend accepts its absence for this
fixed lifetime. A CLOSE fails at the first stop mutation, before firmware shutdown, reset,
power-off or reclaim. Missing isolation cannot report successful shutdown or reusable memory.
No retry, resource release or automatic restart follows uncertainty.

## Build and observe

Configure `apps/gpu-render` using the repository AArch64 toolchain and the pinned
FreeRTOS and TinyUSB sources. Supply `SUNSTONE_LLD` and `SUNSTONE_GPU_FIRMWARE`.
The firmware must have SHA-256
`1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`.
Build target `sunstone-a733-gpu-render`. The image identity and linker map are in
the build directory's `gpu-application` subdirectory. Downloaded sources,
firmware and build artifacts belong outside Git.

`apps/gpu-render/observe.py` attaches only to an already-running image selected
by USB bus/address and epoch. It never launches, resets or configures a device.
It reads copied output chunks and verifies every RGBA byte before creating a
fresh 16 KiB output file. A finite observation timeout bounds the complete copy.
The command requires `--bus`, `--address`, `--epoch` and `--output`.

The USB request is one little-endian 32-bit chunk index, from zero through 63.
The response header contains magic, version, result, chunk, pixel count, width,
height, epoch and checked-pixel count. Success includes 64 copied pixels. Pending
responses contain no pixels. The native USB worker owns transport behavior. The image performs no Type-C
source activation; observation does not qualify electrical operation.

## Evidence limits

The existing [render evidence](gpu-render.md) covers real paired submission with
explicit modeled timelines and poisoned output. Every modeled case rejects the
poisoned output or retains a transport/deadline failure. It supplies no successful
rendering evidence. The application reuses those component results.

A source review accepted the executable composition and failure paths before
the focused batch. The native executable compiled and linked. Symbol inspection
confirmed the real GPU and USB providers, pinned firmware, render workload and
interrupt dispatch, together with the exact large pool extents. Storage workers,
USB2, radio and Type-C code are absent from the linked image.

The focused native instruction-model check passes refusal at the first stop
mutation when isolation is absent. It also passes revocation before a later
kick, power poll or stop mutation. Both cases preserve published memory and
power ownership without another device access. Observer header checks pass
valid-state decoding and refusal of mismatched identity, epoch and validated
extent. These results establish software composition and retention behavior.

Evidence is retained under
`~/.cache/sunstoneos/evidence/gpu-application-20261017/batch1/`. It includes the
final ELF/map, build logs, actual compiler dependency files, native executable
and logs, parser checks and source snapshots. The initial build lacked the
native power adapter's private header selection; the build resumed after that
composition correction. The symbol checker resumed with the existing Python
environment providing ELF support. Successful component cases were not replayed.
Physical watchdog work remains deferred. Physical isolation and resource disposal
remain required for shutdown/reuse. General draws, textures, formats and physical compute qualification remain
separate hardware requirements.

The central `sunstone-a733-gpu-render` target also compiles and links. Its 150
repository dependencies match the reviewed worker snapshot. Central artifacts,
generated sources, firmware bytes and actual compiler dependency files are under
`central-snapshot/`, with the image identity in `central-integration.json` beside
the original batch. No runtime case was repeated for central integration.

## Supervised RAM-only admission

`SUNSTONE_GPU_SUPERVISED=ON` selects a separate supervised contract and preserves
the default watchdog mode. The supervised image checks that the watchdog and
reset redirect are disabled. Its SRAM launcher stamps the actual session start,
a one-second startup deadline and a sixty-second observation deadline. Uploaded
watchdog settings and software timestamps are refused. These deadlines bound
cooperating software. They cannot stop GPU DMA, an interconnect stall or a
stalled CPU.

The supervised contract retains the exact large-memory layout. Its copied
288-byte record includes both the complete session digest and the digest of
reviewed GPU entry evidence. EL3 seals the admitted session before EL1 uses it.
The latter digest must match the session's qualification field. Native startup
still uses the actual shared PMIC/resource providers, power controller, mapping
checks, firmware, render rings, timelines and output validation.

`apps/gpu-render/prepare.py` prepares immutable input files offline. It requires
the final ELF manifest, a supervised session template and reviewed entry evidence
bound to the image and target. It reuses the existing supervised platform and
manual recovery requirements. Additional raw evidence must establish ownership
of the complete GPU RAM reservations, actual 400 MHz parent and settled 800 mV
supply, shared register/PMIC exclusion, remote-master exclusion, intended Device
mappings and initial GPU request exclusion with no outstanding traffic. The
initial-isolation evidence must describe complete power removal and the absence
of later GPU activity. Holding buffers after execution cannot establish those
initial conditions. Neither a record digest nor nonzero ownership metadata
proves the physical facts described by that record.

The bundle freezes every referenced evidence byte string, the final ELF,
launcher, session, contract and exact upload spans. Later validation uses the
frozen evidence and reconstructs the session and contract. Preparation never
opens a USB device. The coordinator owns physical target selection, exclusive
access, protected upload and one bounded attempt. No actual prerequisite record
is supplied by the repository's software fixtures.

The image enumerates as `1f3a:efe8` with product `sunstoneos native GPU` and
serial `sunstoneos-a733-gpu`. The observer checks that identity before copying
pixels. Other applications can use the same permitted PID; the PID alone does
not select a GPU image.

Success, failure, lost transport and elapsed observation time all retain GPU
backing, power leases and ownership until complete whole-board power removal.
A USB reconnection alone does not establish that the SoC or GPU supply lost
power. The coordinator must wait for explicit recovery and must not reuse these
resources, restore their backing or resume a failed session after a timeout.
There is no automatic retry, shutdown or resource release in supervised mode.
Physical watchdog work remains deferred; this mode supplies no claim of
independent fault recovery.

The reviewed supervised software batch passes the root CMake image build,
default admission/session/startup/watchdog-boot unit compilation, offline bundle
validation, SRAM handoff/refusal cases and native admission/refusal cases. The
native admission fixture initially placed PMIC pins in the Type-C fields and
was correctly refused. Only that fixture was corrected; the failed native stage
and later unexecuted frozen-input checks resumed. Earlier passing package and
launcher cases were preserved. No GPU job, controller case or physical device
was executed in this batch. Existing render evidence continues to cover poisoned
output and retained failures rather than successful rendering.

Raw evidence is under
`~/.cache/sunstoneos/evidence/gpu-supervised-20261017/batch1/`, including the
linked ELF and manifest, actual compiler dependency files, default objects,
modeled prerequisite fixtures, frozen bundle, launcher results and native case
results. Modeled prerequisites are explicitly labeled as software fixtures and
must never be used for physical admission.

The coordinator uses `tools/check-a733-gpu-supervised.py` to launch a reviewed
bundle and capture copied pixels. Required arguments are `--bundle`, `--fel`,
`--evidence`, `--database`, `--claim`, `--owner` and
`--supervised-manual-reconnect`. The tool binds queue approval to the initial
bundle hashes, checks the embedded firmware and preserves raw replies before
decoding. Successful output requires all 64 chunks and a durable complete image.
Every uncertain post-entry outcome, including successful capture, retains the
A733 port lock until confirmed full board power removal.

Nineteen focused host cases pass under `gpu-supervised-host-20261017/`. They use
modeled replies and do not establish successful GPU rendering. Existing native,
launcher and protocol evidence is reused without rebuilding or replaying it.

The refreshed supervised image includes the PMIC provider's supported APBS1
source4 route through PLL_REF. Its native adapter permits the required PLL_REF
read, and the provider retains the selected parent for later baseline checks.
The source4 checks and native adapter evidence in
[pmic-runtime.md](pmic-runtime.md) remain applicable.

The artifact refresh under
`~/.cache/sunstoneos/evidence/gpu-pmic-refresh-20261018/` passes one supervised
image link and nominal offline bundle preparation/validation using the final
ELF identity. The bundle contains software-only prerequisite records and cannot
supply physical admission. The recorded compiler input comparison identifies
seven changed files: three PMIC implementation/header inputs, two headers with
comment changes, and the runtime string implementation/header additions. GPU
application, boot patch, CMake fragment and packager bytes match the earlier
supervised batch. Prior PMIC, GPU, launcher and refusal evidence is reused.
The earlier image remains preserved. This refresh establishes linkage and
packaging; successful physical rendering and its entry prerequisites remain
unqualified.

## Repeated numerical compute mode

Configure the existing native GPU target with
`-DSUNSTONE_GPU_COMPUTE_APPLICATION=ON` to run two sequential instances of the
[pinned arithmetic workload](gpu-compute-job.md). The service API supports
further submissions until sequence exhaustion. The bounded application copies
both 32-word results and their advancing timeline identities before publishing
success. It uses the actual reserved-memory, native VM, shared PMIC, power,
backend, firmware, service worker and USB providers. The compute mode omits
render-target construction. Its build identity includes the mode selection.

The USB request is a little-endian 32-bit submission index, zero or one.
A compute response uses magic `0x43555047`, version one, a 40-byte header and
128 result bytes after success. Header fields identify state, index, word count,
submission count, timeline sequence, session epoch and total verified words.
Pending and failed responses contain no result bytes. The render protocol uses
a different magic. The host observer validates both sequences and every result:

The GPU client waits until one valid pending response has completed its USB IN
transfer before sending OPEN. Queuing a response or completing only part of it
does not open this gate. The existing pending-status request supplies this
boundary; no new command or fixed delay is required. Waiting remains bounded by
the admitted session deadline. This establishes working transport before GPU
power or firmware startup can fail, without claiming host journal durability.

Shared provider, power, clock, or transport faults still revoke the application
and stop USB. A final failed-status frame is therefore not guaranteed after a
fatal fault. Missing native USB alone cannot identify whether startup, PMIC/USB
preparation, or a concurrent GPU OPEN failed in images preceding this gate.

```sh
python3 apps/gpu-render/observe.py --compute --bus BUS --address ADDRESS \
  --epoch EPOCH --output /absolute/new/results.bin
```

The observer reads an already-running admitted image. Supervised preparation,
actual memory/power admission and coordinator board ownership remain required.
A bounded physical attempt must establish two advancing timelines
and correct copied numerical outputs while retaining all GPU resources afterward.
No successful cancellation or resource-release claim follows from this image.

The affected compute image builds under
`~/.cache/sunstoneos/evidence/gpu-completion-20261018/native/`.
Its manifest identifies the final ELF and build inputs. The focused host check
`python3 checks/gpu-compute-application.py` accepts both numerical frames and
refuses malformed state, identity, sequence, arithmetic and render frames.
[Service composition checks](gpu-compute-service.md) cover repeated native memory
publication and acquired result copying with modeled GPU writes. Physical
numerical execution, arbitrary programs, general graphics and mimus acceleration
remain unqualified.


## Supervised capture of the first numerical workload

The coordinator launcher selects the output protocol from the frozen image
manifest's `workload` field. `fixed-arithmetic-repeat` captures two numerical
submissions into `compute.bin`; `fixed-clear` captures the render into
`rgba8.bin`. Earlier render manifests without that field retain the render
protocol. Other workloads are refused before device access. Protocol selection
has no command-line override. Each reply is saved before decoding. Success
requires both advancing compute sequences, all 64 correct words, and durable
output before the capture deadline.

The focused host evidence is under
`~/.cache/sunstoneos/evidence/gpu-physical-proposal-20261018/host/`.
Its 33 cases cover render regression, compute success and refusal, transfer
failures, queue ownership, pinned firmware and frozen manifest selection.
Replies and device operations are modeled. The checks reuse the compute ELF
under `gpu-completion-20261018/native/build/`, with SHA-256
`55fcba76201f52e54ab4e1dbd24f04e9c3dbfe9857843c5d463204663621f72e`.
Its manifest is `gpu-application/manifest.json` beside that ELF. No native image
was rebuilt or physically executed for the host change.

The all-core GPU target starts cores 1 through 7 in
`platform/a733-smp-cached/boot.c` before entering its runtime. That target depends
on working all-core startup, interrupts and coherent scheduling. The separate
single-core compute target below uses the boot CPU and requires the other seven
CPUs to remain OFF. A CPU startup correction requires an affected build and new
image-bound admission; preserved images cannot establish the correction.

Before preparing physical inputs, the coordinator must establish these facts
for the selected target and final image:

| Required fact | Available observation and remaining limit |
| --- | --- |
| Target and recovery | Identify the stable USB port and current instance, verify reconnect access, and document complete board power removal with every power source removed. A ROM descriptor or USB reconnection alone cannot prove GPU power removal. |
| RAM exclusion | Establish exclusive ownership of `0x41000000..0x42200000` and `0x48000000..0x79000000`, disabled inherited CPU caches, and exclusion of remote writers. DDR capacity and two successful page transfers do not cover those reservations. |
| Inherited parent | Read oscillator selection at `0x07090160`, PLL registers at `0x02002000`, `0x020020a0`, `0x020020a8`, `0x020020ac`, and branch configuration/status at `0x02003908`, `0x02003988`. Apply the exact checks in [GPU resources](gpu-resources.md). These establish programmed 400 MHz settings, not measured frequency. |
| Inherited supply | Through the reviewed PMIC owner, verify AXP8191 page/identity/revision and enabled DCDC4 selector 30, then establish settling. Register settings alone do not measure an 800 mV rail. The GPU path cannot repair an unsupported supply or PLL baseline. |
| Initial GPU exclusion | Verify disabled local clock `0x02002b20[31]` and asserted external reset with bus gate disabled at `0x02002b24[16,0]`. Establish complete earlier power removal and no later GPU activity. These register reads cannot prove an inherited transaction drain. |
| Platform ownership | Establish shared CCU/PCK/PMIC exclusion, exclusion of other GPU masters, and Device mappings for GPU/GIC. Resource tokens and session digests bind records but cannot establish those physical facts. |

The absent [runtime drain guarantee](gpu-isolation.md) does not require a
shutdown experiment for this fixed lifetime. Entry still requires actual
initial isolation. Every outcome retains all GPU backing and leases until
complete whole-board power removal.

Once those conditions and the selected image's CPU startup dependency are established,
the bounded coordinator procedure is:

1. Freeze the final supervised compute ELF and its matching manifest. Use the
   existing pinned firmware hash above. Review target-specific admission and
   session-template records against the raw observations; software fixture
   records cannot supply physical authority.
2. Run `apps/gpu-render/prepare.py` offline with `--image`, `--manifest`,
   `--admission`, `--session-template`, a fresh `--output`, and the previously
   configured `--clang` and `--lld`. This freezes all referenced evidence and
   builds the bounded SRAM launcher. Validate the resulting bundle before any
   device operation.
3. Submit the frozen `prepared.json`, `image.elf` and `launch.bin` to the existing
   per-board queue. Review prerequisites `supervised-ram-recovery` and
   `gpu-supervised-software` against that exact image, launcher and current host
   source bytes. Reference the existing admission/launcher evidence and affected
   host cases instead of replaying unchanged checks.
4. With `SUNSTONE_USB_PORT` selecting the qualified target, invoke
   `tools/check-a733-gpu-supervised.py --bundle BUNDLE --fel FEL --evidence NEW_DIR
   --database QUEUE --claim CLAIM --owner OWNER --supervised-manual-reconnect`.
   The shared runner takes the port lock, checks the claimed frozen inputs and
   launches once. The native session allows sixty seconds; host capture allows
   at most fifty-nine seconds and 304 queries.
5. Accept numerical operation only when `gpu_result` is `observed-success` and
   the durable `compute.bin` contains both verified 128-byte results. Inspect
   the saved raw replies for advancing sequence identities. On pending-budget
   expiry, malformed output or transport failure, stop communication. Success
   also retains resources and the physical lock. Confirm full power removal
   through the existing recovery workflow before permitting any resource reuse.

No actual GPU entry record is supplied by the host checks. Initial memory and
master exclusion, supported settled power, CPU startup and physical numerical
execution remain requirements for the first board attempt.


## Boot-CPU-only supervised computation

With `SUNSTONE_GPU_SUPERVISED=ON`, `SUNSTONE_GPU_COMPUTE_APPLICATION=ON` and
`SUNSTONE_GPU_SINGLE_CORE_APPLICATION=ON`, build target
`sunstone-a733-gpu-compute-single-core`. Its manifest uses execution profile
`gpu-single-core-supervised-v2`. An immutable one-core geometry record and the
actual entry symbol bind that profile to the ELF. Preparation rejects attempts
to label this image as the all-core target, or to label an all-core image as
single-core. The existing all-core target remains available.

This target reuses the storage application's single-core entry, retained EL3
monitor, FreeRTOS SRE port and terminal handling. Its explicit GPU branch seals
the complete 288-byte GPU contract and 592-byte session. Before EL1 entry, it
checks the seven secondary CPUs' OFF status and control settings. It starts no
secondary CPU and changes no secondary power control.

The shared large-memory constructor builds the GPU tables while caches remain
disabled. Both MMIO roots select outer-shareable Device memory. EL1 adopts the
exact existing coherence TCR/MAIR values and table root, then runs the unchanged
full translation scan before publishing either memory pool. The secure monitor
block remains inaccessible to EL1. The session, memory binding and bounded EL1
runtime contract/secondary-state copies occupy a separately cleared retained
RAM section outside EL1's BSS range. The complete authority contract stays in
EL3 monitor storage. The diagnostic platform log has separate image RAM; GPU
completion remains the copied and verified USB output.

USB reference mode 2 selects the existing ROM PIO takeover path only for this
single-core GPU profile. Native admission and offline preparation accept modes
1 and 2 there; the all-core target accepts mode 1. The common native USB provider
and controller already support the takeover. No controller admission has been
relaxed. Actual absence of earlier undrained USB DMA, exclusion of concurrent
ROM/firmware access, ownership of USB0 and its clocks, and full power-removal
recovery remain required by [the USB contract](usb-platform.md).

Frozen evidence is under `~/.cache/sunstoneos/evidence/gpu-single-core-20261018/`:

- `native/build/sunstone-a733-gpu-compute-single-core.elf` has SHA-256
  `5bd80e10e2bf7f13dfeede7cbe1bb7a915937d3842c2195c5fe09cdbf8d19938`.
  Its map and manifest are in `native/build/gpu-single-core/`. It includes the
  integrated normalized REF24 clock decoding.
- `startup-pool-fixture/result.json` records actual linked startup, sealing,
  BSS survival, protected-monitor refusal, complete WB/NC mapping verification,
  and shared-provider, static-task and GPU service construction. CPU registers,
  counter progression, cache publication and descriptor-based hardware walks
  are modeled. No GPU job or arithmetic output is injected in these cases.
- `package-retained-view/result.json` records offline packaging and native
  contract acceptance/refusal. `profiles-resume/result.json` covers native and
  host acceptance for all four USB mode values in both execution profiles.
- `storage-compatibility/result.json` confirms unchanged preprocessed storage
  behavior in the three shared adapter files. Only whitespace and diagnostic
  file/line locations were normalized. Existing storage runtime evidence is
  reused. GPU arithmetic, SRAM launcher and host-capture evidence is also reused.

Initial checks exposed invalid generic PMIC pin/fabric fixtures and an incomplete
emulator RAM map; the final baseline uses the accepted provider settings and
complete GPU reservations. Review also found inaccessible EL1 pointers into
monitor storage; the retained runtime copies and an ordinary-load trap cover
that correction. Earlier failed results and image identities are preserved.

For one physical attempt, use the existing prepare/claim/launch procedure with
this ELF and its own manifest. This target needs primary-CPU startup, local
interrupts and the stated seven-CPU OFF baseline. It does not depend on secondary
scheduling or all-core coherence. Actual DDR exclusion, supported settled GPU
power, initial GPU isolation, ROM USB takeover authority, and successful numerical
execution remain unqualified. Every outcome retains the complete lifetime until
confirmed whole-board power removal.

## First physical compute attempt produced no native result

The single-core compute image was launched once on the second board from ROM
instance `003:116`. Current launcher checks passed after the returning cache-off
helper. Root reviewed the completed predecessor history, reset-held GPU/DMA
baseline, ROM PIO takeover, full memory ownership and source-qualified DCDC4
settling. Historical power removal and global transaction drain remained unknown.

No native USB service appeared within the capture window. The one permitted
refusal-report read timed out, and communication stopped. No computed output
was received. This result does not establish whether execution reached GPU
initialization; it also does not identify a shared cause with the multicore
failure. The first attempt was released after confirmed power removal. Do not repeat
a failed image without a concrete correction or a
narrower diagnostic supported by the failure evidence.

The frozen bundle, admission and result are preserved under
`gpu-single-core-20261018/physical-candidate-116/`. The executable remains the
reviewed image above; no build or accepted software check was repeated.


## Corrected session and earlier USB status still produce no physical result

The corrected image waits for a completed USB pending-status transfer before
opening the GPU. The corrected session masks the NSI configuration and uses
its observed 400 MHz MBUS rate. Eleven affected application cases and four
frozen-provider admission cases passed; unchanged arithmetic and startup
checks were reused.

A supervised attempt with fresh DDR, PMIC, clock, CPU-OFF and DMA observations
still produced no native USB response. The sole refusal-report read timed out,
and communication stopped. The runner retains all resources pending confirmed
power removal. This result does not establish whether provider or GPU
initialization began. Evidence:
`gpu-usb-ordering-20261018/physical-candidate-125/runtime/session.json`.
A narrower shared-startup observation or concrete correction must precede another
full launch. Performance measurement remains deferred.
