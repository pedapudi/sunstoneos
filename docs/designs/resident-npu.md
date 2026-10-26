# Resident OS NPU application and card assets

The resident recorded-audio OS can run its existing finite NPU catalogue
application alongside CPU recognition. Enable `SUNSTONE_A733_OS_NPU` and
`SUNSTONE_A733_OS_RESIDENT`; the resident profile also accepts
`SUNSTONE_A733_OS_CPU_CLOCK`. The NPU application runs one selected catalogue
entry for one to eight jobs, publishes its existing result, completes local stop
and suspends. Mimus recognition continues on the CPU. This application does not
accelerate Mimus operations.

The implementation uses the existing NPU startup, interrupt, service and runtime
providers in [npu.c](../../apps/os/npu.c). It adds no resident restart loop or
second asset loader. [The startup contract](../npu-startup.md) describes normal
local stop, retained power ownership and the separate restart consumer.

## Lifetime and thermal ownership

The NPU work deadline is the earlier of the board deadline minus 10 ms and
60 seconds after runtime preparation. The resident board lifetime can continue
after that finite application completes. The NPU service has a five-second
health window, which covers the existing request's two-second send and
two-second collection limits. The application reports health before requests
and during OPEN and local-stop waits. Each report checks the absolute work
deadline independently of bootstrap progress. Expiry calls `board_fail`,
retains resources and stops watchdog renewal.

The application publishes completion once. The supervisor exempts its suspended
client only when the published report has `finished=1`, `stopped=1` and
`retained=0`. A retained failure or an unpublished stopped flag cannot provide
that exemption. Existing atomic fault, pending-IRQ and IRQ-stop state must
also continue to prove the NPU stopped. A late IRQ revokes the exemption, and
the unchanged five-second health window then stops watchdog renewal. The
published result remains immutable. After normal CLOSE, the runtime worker can continue its
counter reads and CPU bookkeeping; those operations do not access NPU MMIO
through the closed startup window.

The existing OS thermal owner grants work only while all five channels have
fresh NORMAL readings. Hot or stale readings cause the existing terminal STOP
with at most five seconds to drain; CRITICAL fails the board lifetime. There is
no independent NPU thermal policy or voltage owner. Startup selects the vendor
852 MHz point at 800 mV for supported SID codes `0x12`, `0x04` and `0x05`.
Unsupported bins cannot use this fixed-voltage top-point policy.

Each request keeps its existing deadline sample and adds one resident heartbeat
counter read and atomic store. Each 25 ms OPEN or stopped wait adds one
deadline sample and one heartbeat, at most 40 reports per second while waiting.
The health table adds one eight-byte timestamp and one 16-byte service
descriptor; each supervisor pass checks the published result and existing
atomic stopped, fault and IRQ state, with up to six acquire loads in the
stopped predicate.
Startup adds one counter read to cap the work deadline. This integration adds
no per-inference model hashes, address translations or allocations.

## Card layout and admission

[card.py](../../apps/os/card.py) uses the existing four-extent card format and
[ordinary-file preparation procedure](autonomous-card-os.md). The extents are
the linked runtime at `0x41000000`, session at `0x41f00000`, combined Mimus
models at `0x44000000`, and recorded WAV at `0x45000000`. The model extent
preserves the existing overlay offset of 8 MiB. The NPU pool
`0x47400000..0x47c00000` receives no card payload: it is reserved for runtime
model, command and tensor allocations. The loader's `0x47000000` load limit
excludes both that pool and the stage-2 monitor reservation.

The NPU session adds 272 bytes to the 808-byte recorded profile. Its size is
1,080 bytes without a CPU grant and 1,176 bytes with the trailing 96-byte grant.
The slot producer binds the generation; stage 2 checks zero persisted epochs,
binds the board SID identity and stamps a fresh epoch into the board record
and optional CPU grant. The linked OS then validates the complete session.
The monitor retains its fixed 32 KiB initialized-image limit.

The selected ShuffleNet entry contains a 964,224-byte model, a 150,528-byte
input and a two-byte expected output. OS generation places those const,
64-byte-aligned arrays in `.data.os_npu_assets`, which the existing linker
collects into initialized RAM starting at `0x41200000`. Metadata and executable
code remain in the fixed 2 MiB CODE block. The flat runtime extent includes the
asset bytes, and startup's existing publication covers that extent. The payload
precedes BSS, heap and device allocations and ends below the session page.
The CPU RAM mapping is writable and non-executable; asset immutability relies
on trusted ownership. The mapping supplies no hardware read-only protection.

The card producer follows the actual linked model and vector pointers and
checks their sizes and hashes against the selected catalogue entry. Runtime
preparation authenticates the model again before copying it into the private
NPU pool. No additional model extent or loader ABI is introduced.

The cold policy binds the selected entry, catalogue digest, job count, owner,
command admission, supervisor, clock client, pool, work deadline and operating
point. Its five NPU records bind admission, clock, power, supervision and DDR
reservation to the board identity, SID authority and image digest. Every
condition reference is hashed and copied into the bundle. The existing
supervised NPU schema remains separate from this cold authority.

Cold records must establish cross-PLL disturbance qualification, watchdog-reset
retirement of NPU activity and memory access, and exclusion of remote writers
at the following cold entry. A normal local stop retains clock, rail, power
domain and memory ownership; it does not prove those reset and entry facts.
A finite period without observed writes is insufficient evidence of retirement.
The `--model-fixture` path only exercises the producer and admission checks.
It cannot qualify a physical installation, even when every software check
passes.

## Software evidence

`~/.cache/sunstoneos/evidence/resident-npu-1026` contains the selected ShuffleNet
resident build and focused checks. The component build uses CPU clock grants,
A76 admission and the default two recognition workers. Its stage-2 binary is
32,608 bytes, leaving 160 bytes under the fixed limit. The failed CODE-only
placement and its symbol sizes are preserved alongside the corrected layout.

The actual card producer and sanitizer-backed native admission pass 32 cases.
That record inherits the case label `actual-eight-worker-bundle`; its image
manifest and linked capability establish two workers. The checker now uses
`actual-linked-worker-bundle`, and seven focused named refusals confirm the
NPU summary binding and actual model-byte authentication.
The linked stage-2 entry passes 20 cases, including both NPU session sizes,
fresh epoch publication and rejected grant mutations. The linked NPU code and
cold-policy validator pass 18 focused cases covering the finite work cap,
OPEN and stopped-wait expiry, terminal health and mismatched cold authority.
RTOS replies, counters, SID and watchdog registers are modeled. These checks
execute no NPU inference, reset or physical card operation. Earlier successful
ShuffleNet inference and normal local-stop evidence remain unchanged; they do
not qualify this resident cold-boot path.

The combined resident build in
`~/.cache/sunstoneos/evidence/resident-npu-cpu8-1026` selects eight recognition
workers, CPU clock grants, A76 admission and the same ShuffleNet entry. Its
monitor includes the reviewed CPU static-OFF owner and is 32,688 bytes, leaving
80 bytes. The final image passes card production and 20 linked entry cases.
Three focused linked cases verify late-IRQ revocation, persistent atomic
runtime fault and the pre-activation stopped exemption. The initial late-IRQ
fixture targeted an unlinked helper; the corrected check invokes the actual
`board_resident_renew` path with only watchdog reload modeled. CPU-owner checks
use this same final artifact. No inference or physical test is repeated.
