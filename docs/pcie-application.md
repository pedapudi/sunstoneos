# Supervised native PCIe direct-endpoint observation

The application connects the existing native PCIe controller, COMB1 PHY and
read-only configuration API to the board providers and copied USB output. Its
initial mode starts an x1 Gen1 link to one authenticated cold direct endpoint,
reads four conventional configuration-header dwords and checks endpoint/link
identity before publishing a private report. No endpoint is presumed connected.
Physical operation remains unqualified.

The complete 792-byte session binds the board prefix, owner, authority, startup
bound, PCK client, inherited PD20–23 configuration, actual endpoint vendor/device
identity and four entry-evidence digests. The common board seal and a local full
seal authenticate and publish retained copies before provider construction. Both
application hooks reject a differing board prefix. Only the matching admitted
session can extend pin authority to PD20–23.

One task owns the existing controller and native adapter. It uses the same clock
registry, rail ledger, PCK manager, pin manager, PMIC provider and register mutex
as USB. The controller reserves PCIE0/COMB1 exclusively, holds the shared parent
and domain scopes, obtains DCDC2/BLDO2 snapshots and checks inherited PERST low
before startup. Shared SERDES 100 MHz and PCK 7 must already be ready. The existing
PHY/controller sequence changes only its admitted local state. It does not
reset or retune shared SERDES, drive PD23 or alter PMIC voltages. COMB1 USB3 users
are excluded; COMB0 and unrelated shared fields are preserved.

App-local callbacks delegate to the actual native adapter, preserving its
privilege, designated-task, mutex-holder and address/width checks. Atomic
application liveness gates callbacks before and after I/O. Absolute deadlines
bound cooperating software, not a stalled bus. Cancellation or uncertainty
revokes copied-output eligibility immediately. The owner retains acquired clock
and rail metadata without another healthy controller operation. Pin, PCK and
other held leases remain owned until confirmed whole-board power removal.
There is no automatic reset, rollback, release or reuse.

Initial endpoint checks use the existing controller's capability walk and BME,
MSI and MSI-X refusal checks. The application then reads function 0 offsets 0,
4, 8 and 12 through the public configuration API. A final walk follows at most 48
aligned capability headers with a visited bitmap, rejects malformed or cyclic
lists, checks retained PCIe/MSI pointers and verifies both MSI and MSI-X remain
disabled. Final endpoint link status must report x1 Gen1, followed by the core's
READY resource/link/BME check before immutable publication. These sequential
reads are an observation interval, not an atomic hardware snapshot.

The 64-byte report contains its completion observation time, direct-function/read
selection, copied endpoint identity and four offset/value pairs. The 128-byte USB
header binds the entire session and payload by SHA-256. Pending/failure frames
carry no payload. Output eligibility is checked before snapshot, after copying
and before queued transmission. Success keeps every resource live and owned;
it does not qualify continued endpoint health or authorize more transactions.

## Build, prepare and capture

Configure the normal pinned board/toolchain dependencies with
`SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_PCIE=ON`, then build
`sunstone-a733-pcie`. Its final manifest is
`pcie-application/manifest.json`. `apps/pcie/prepare.py` requires `--image`,
`--manifest`, `--admission`, `--template`, `--output`, `--clang` and `--lld`.
It authenticates the complete session, board entry and PCIe-specific physical
records before producing an immutable SRAM-launch/input bundle.

`apps/pcie/capture.py` uses the existing supervised queue, claim, port lock,
access/discovery and retained-failure transport. Required arguments are
`--bundle`, `--fel`, `--evidence`, `--database`, `--claim`, `--owner` and
`--supervised-manual-reconnect`. PID `0xefe8` uses a distinct PCIe product/serial.
Raw frames and the complete checked report become durable before success.
No persistent storage or endpoint configuration write is requested.

## Physical request and remaining requirements

The coordinator may prepare one bounded RAM-only attempt after board quarantine
and recovery are resolved, with an actual endpoint/carrier present. Reviewed
image/port-bound records must establish the V1.2 FPC circuit, external supply and
signal voltages, COMB1 analog supplies, PD22 active-low PERST already asserted,
PD20/21/23 inherited input/disabled state and no inherited endpoint DMA. The
specific endpoint must obey PERST and disabled bus mastering. Its actual
vendor/device tuple is admitted data, not a fixed application assumption.

Entry also requires stable shared SERDES/parent clocks, effective PCK 7 ON with
autonomous PCIe power disabled, exclusion of COMB1 USB3 and remote/secure writers,
ownership of interrupt IDs 168–175/184/185, Device/security mappings and the
existing board CPU/memory/coherence/USB/PMIC/counter conditions. The initial mode
keeps those interrupts masked. It supplies no INTx/MSI delivery service.

Expected output is one consistent direct-endpoint report or a retained failure.
A cable reconnection alone permits no reuse; keep the claim, leases and backing
until confirmed full board power removal and the reviewed endpoint power/reset
sequence. Independently powered carriers need their own qualified recovery;
removing the board cable alone does not establish endpoint reset. A software deadline is not an
independent reset or a DMA-drain guarantee. Disabled inbound windows do not
establish hardware isolation from an untrusted endpoint.

BAR allocation, bridges/enumeration, endpoint drivers, DMA, INTx/MSI service,
Gen2/Gen3, power transitions and shutdown remain required PCIe work. This initial
application path does not claim complete PCIe support.

## Software evidence

The frozen software-only batch in
`~/.cache/sunstoneos/evidence/pcie-application-20261018/` contains one supervised
native link, sanitizer-checked production admission/full sealing, fourteen linked
application cases, four host groups and a nondeployable offline launch bundle.
The nominal setup executes the actual core configuration validator, pure core
constructor, native admission and native task checks before behavioral cases.
The linked cases cover retained-session refusal, actual native callback delegation,
endpoint/capability refusal, cancellation retention and publication/copy revocation.
Host cases cover durable copied output, session/frame integrity and actual shared
selector/access/discovery routing.

Controller return values, initial shared-manager state, translations, time,
RTOS scheduling and USB transport are modeled. Existing controller, PHY and shared
resource evidence is reused. These results establish application composition and
copied-output behavior; they establish no physical link, endpoint safety,
electrical qualification or stalled-MMIO recovery. The frozen fixture names no
physical port and cannot authorize a device attempt.
