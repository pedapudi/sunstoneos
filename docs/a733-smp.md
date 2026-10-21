# Eight-core diagnostic image awaits physical recovery qualification

> **Superseded.** Use the cached base in [supervised startup](a733-smp-supervised.md)
> and [cached SMP](a733-smp-cached.md). This uncached image predates two physical
> corrections that exist only in `platform/a733-smp-cached/continue.S`: Secure
> `ICC_PMR_EL1` initialization at EL3 and `SCTLR_EL1.nTWE`. Its continuation in
> `platform/a733/cpu_continue.S` still lacks both, so it must not be launched
> unchanged. The text below describes the image as it was built.

The dedicated `a733-smp-diagnostic` target links the A733 CPU power controller,
secondary entry, EL3-to-EL1 continuation and eight-core FreeRTOS scheduler.
Its intended check covers six Cortex-A55 cores and two Cortex-A76 cores.
The image builds, its admission checks pass under host sanitizers, and instruction
emulation verifies all eight scheduler interrupt destinations and bounded boot
exclusion checks. The complete image has not executed on a board or in a machine
emulator. Physical CPU startup, shared atomic operations and scheduling remain
unqualified.

This is a functional diagnostic with instruction and data caches disabled.
All CPUs use the [continuation's canonical Normal-noncacheable mapping](cpu-continue.md).
Physical support for exclusive accesses to that mapping still requires testing.
Cache-enabled coherence, CPU-specific errata and production memory isolation
remain required work.

## Boot owns resources until an independent watchdog resets the board

The image requires a reserved, published ELF image and private RAM allocations.
It enters at EL3 on core 0 with the MMU, instruction cache, data cache and
big-endian mode disabled. Other firmware and bus masters must relinquish the CPU,
clock, rail, interconnect, GIC and image allocations before entry.
A nonzero lease in a host-provided contract names that external reservation.
The image cannot establish ownership or remove cacheable aliases.

The [native boot adapter](../platform/a733-smp/boot.c) restricts each power
callback to its selected CPU's architecture, entry, hotplug and status words.
It verifies core 0, EL3 and cache-disabled access before each callback.
The existing [power controller](cpu-start.md) supplies the register sequence;
the adapter does not duplicate it. The published board-firmware comparison in
that document remains the sequence evidence.

The boot owner prepares the shared translation tables, disables GIC distributor
groups and shared interrupt sources, then enables affinity routing and the
required groups. Each of seven secondary CPUs must complete the existing
arrival, power-cleanup and release protocol before core 0 enters EL1.
All components use the same 24 MHz physical counter and absolute startup deadline.
A failure retains every allocation and register reservation, without a retry.

Each CPU first completes the continuation's bounded private exclusive-access
probe. All eight then enter a shared exclusion check before any FreeRTOS spinlock.
Each CPU increments a shared counter 32 times under a bounded exclusive lock.
Core 0 requires a total of 256 increments and releases the boot barrier.
This small check can expose immediate failures; success would not qualify every
inter-core memory-ordering case.

## Scheduler interrupts address A733 affinity level 1

The image generates its own FreeRTOS port using the existing SMP fixes and an
[A733-specific patch](../patches/freertos-a733-smp.patch). CPU index `n` maps to
Aff1=`n`, Aff0=0. SGI0 therefore uses `ICC_SGI1R_EL1 = (n << 16) | 1`.
The generated primary readiness loop uses acquire/release publication and checks
the startup deadline. Redistributor discovery supplies the primary priority
register address; the image does not assume frame order.

The continuation installs separate EL3 and EL1 vector tables and reserved stacks
for every CPU. Core 0 alone enables physical-timer PPI30 and generates kernel
ticks. Each core receives an explicit SGI0 and runs two pinned tasks: a busy
background task and a higher-priority task that preserves a distinct `d8` value
across 32 blocking switches. Core 0 requires every worker's completion, background
progress, SGI receipt and clear failure record before reporting success.
All C code uses general registers only; the FP assembly fixture supplies the
floating-point activity.

## Recovery qualification must precede contract publication

The [contract ABI](../platform/a733-smp/boot.h) occupies a page outside result
clearing. A coordinator must provide a matching compiled build-input digest,
nonzero session epoch, generation and inverse generation, resource lease,
absolute deadlines and the recorded watchdog configuration. Reusing the generation
already present in the result record is refused. The digest identifies selected
build inputs; the coordinator separately records and verifies the exact ELF hash.

The current adapter admits only a four-second watchdog interval with RTC 32 kHz
clock selection, whole-system reset and the documented output setting.
It checks watchdog MODE, CFG and OUTPUT readback, FEL flag, ROM redirect flag and
entry, clear alternate redirect, and the low physical FEL input before GIC or
CPU-power writes. It copies the contract before those checks and bounds startup
by one second from admission. The diagnostic deadline leaves at least one
second before the declared watchdog expiry.

These readbacks cannot prove oscillator progress, watchdog independence or
successful ROM return. The coordinator must first qualify that watchdog interval,
clock source and protected FEL recovery procedure. The pending
[SRAM watchdog trial](watchdog-expiry.md) has not established those properties.
The host adapter described below closes the preparation and publication gap;
physical execution remains blocked by independent recovery qualification.

The diagnostic never feeds, disarms or resets the watchdog. Success and failure
park with resources retained until the independently armed reset occurs.
Watchdog reset can invalidate DRAM contents. A returned ROM instance therefore
cannot establish diagnostic success from the previous result address.
A qualified observation channel remains necessary to report a physical scheduler
result. A launch-protocol trial may report only that a distinct ROM instance
became accessible.

## Preload and verify before issuing one execution request

The [host adapter](../tools/check-a733-smp.py) separates host preparation from a
coordinator's physical launch. Preparation parses the ELF, checks its reviewed
hash and linker reservations, builds the SRAM launcher, and copies every input
into a new evidence directory. It creates random nonzero session, generation and
lease identifiers. The contract initially has a clear magic value and zero times.
Preparation makes no board requests:

```sh
python3 tools/check-a733-smp.py prepare \
  --image /absolute/path/a733-smp-diagnostic.elf \
  --image-manifest /absolute/path/image-manifest.json \
  --output /absolute/new/prepared-directory
```

The image manifest's `final_elf` field is the reviewed ELF SHA-256.
Preparation uses Clang 21, the cached LLVM 21 linker and LLVM 21 objcopy.
It records source copies, commands, build output and payload hashes.
Prepared schema 2 and request version 2 bind the expanded SRAM layout; the host
rejects earlier bundles before board access.

A physical launch requires a claimed queue item and four reviewed prerequisites:

- `independent-watchdog-rom-return`: qualified reset, clock and protected ROM
  return evidence for the same physical port, SoC and captured ROM.
- `stable-protected-fel`: successful physical input inspection for the current
  USB instance, with SRAM restoration recorded. The coordinator must confirm
  that the BOOT-to-GND connection remains stable through reset.
- `ddr-reservation`: current-instance initialization and exclusive ownership of
  `0x41000000..0x42200000`, cache-disabled publication and excluded remote writers.
- `smp-launcher-software`: the exact prepared bundle, image, launcher and host
  source, with successful host protocol and instruction-model evidence.

The independent recovery record uses schema `a733-independent-reset-v1`.
It names the physical port and sysfs path, SoC `00001903`, captured ROM hash,
watchdog MODE `0x41`, CFG `1`, OUTPUT `2`, and the independent 32,768 Hz RTC source.
Its hashed evidence covers clock source, masked-CPU expiry, whole-system reset,
protected ROM return and interconnect-stall recovery. It also requires a
24 MHz physical counter that remains continuous across the CPU warm reset.
The launcher checks CNTFRQ, but that software register cannot prove either clock
progress or continuity. These prerequisites concern independently qualified
hardware properties; they do not require this launcher's earlier physical success.
No qualifying recovery record currently exists.

The DDR record uses schema `a733-ddr-reservation-v1`. Its `identity`, `port`,
`image_sha256` and `ranges` bind the current instance and exact reservation.
The `exclusive`, `published_cache_disabled` and `remote_writers_excluded` fields
must be true, with hashed initialization and reservation evidence.
The software record uses schema `a733-smp-launcher-software-v1` and requires
`host_protocol` and `instruction_model` true, `physical_access` false,
artifact references for `prepared`, `image`, `launcher` and `host_tool`, and a
nonempty `evidence` list. Artifact references contain absolute `path` and
`sha256` fields. The coordinator reviews these records before queue admission.
A record describes evidence; its booleans do not establish hardware qualification.

The launch command requires explicit paths for the prepared bundle, pinned FEL
tool, qualification record, input evidence, DDR contract, queue database and new
evidence directory. It also requires the queue claim, coordinator owner and
`--confirm-bridge-stable`. The adapter checks host health, acquires
`/tmp/sunstoneos-a733-<USB-port>.lock`, checks the target identity, and requires the port's
reconnect access rule and an unquarantined ROM instance.
It reads the queue without changing its claim or completion state.

The adapter saves and flushes SRAM backups, then uploads every file-backed ELF
segment, a zeroed result reservation, the unpublished contract, request, report
and launcher. The adapter verifies each source against its admitted length and SHA-256, then
copies those bytes into a Linux memory file sealed against writes and resizing.
FEL inherits that descriptor and reads its `/proc/self/fd` path. Ordinary source
replacement or growth cannot change the transferred bytes or extent.
Device readback must match the admitted identity. SRAM restoration uses the same
sealed transfer mechanism and the original backup hashes. Watchdog
MODE must remain disabled before each upload and before execution.
After four low-input samples and a second queue review, it flushes the execution
intent and issues one `exe 0x52000` request. It never invokes the FEL tool's
`reset64` operation, which would upload another helper after watchdog arming.

## The SRAM launcher arms and publishes the timed contract

| SRAM range | Purpose |
| --- | --- |
| `[0x52000,0x52c00)` | Launcher code; the linker rejects overflow |
| `[0x52c00,0x52c80)` | Immutable 128-byte request |
| `[0x52d00,0x52d80)` | Transient launcher report |
| `[0x53000,0x54000)` | Private launcher stack |
| `[0x71000,0x71080)` | Saved FEL helper scratch |

The request contains its magic, version and size; session epoch; generation and
inverse generation; resource lease; build-identity address; 32-byte build identity;
and four reserved zero words. It has no selectable branch destination.
The launcher requires secure AArch32 SVC execution on core 0, little-endian access,
disabled MMU and caches, and CNTFRQ equal to 24 MHz.
It validates the request, corresponding copied contract and image build identity
before any peripheral writes. It also requires low BOOT input, disabled watchdog,
RTC clock selection and clear ROM redirects.

The [launcher](../platform/a733-smp/launch.c) sets and reads back the protected ROM
redirect, FEL flag, whole-system reset and watchdog output configuration.
It samples the physical counter immediately before enabling the four-second
watchdog, feeds once, and verifies MODE, CFG and OUTPUT.
That pre-enable sample is a conservative lower bound for the actual arm time;
the transient report also records a post-readback sample.
The contract's startup, diagnostic and expiry times are respectively one, two
and four seconds after the lower-bound sample. The launcher publishes magic last.

Counter checks after arming, after publication and before the transition refuse
elapsed intervals of 24,000 ticks or more, equivalent to one millisecond at the
required counter rate. A backward counter or arithmetic overflow also refuses
entry. The final check precedes a short instruction tail that requests warm reset.
It does not bound a stalled CPU, bus transaction or hardware reset completion.
Independent watchdog recovery remains necessary for those failures.
The diagnostic independently rejects entry after its one-second startup deadline.

The assembly writes fixed RVBAR `0x41000000`, verifies both RVBAR words, executes
DSB/ISB and requests AArch64 warm reset through RMR's RR and AA64 bits.
This follows `aw_rmr_request` in the pinned sunxi-tools source revision
`b92f4ed3115a520e3ec406697d268239e3f29488`.
The A733 RVBAR location is `0x08001004`; the high word is cleared.
The watchdog register sequence and protected-ROM prerequisites are documented in
[watchdog expiry](watchdog-expiry.md) and the A733 manual's watchdog section 8.3.6.
Request, contract, image-identity, BOOT-input, clock-source and counter-frequency
refusals return to FEL when the architectural context is valid and the watchdog
is disabled. The entry wrapper saves `r0`–`r12`, LR, CPSR and the incoming stack
pointer before masking interrupts. The clean-return epilogue restores that state
and uses `bx lr`, including the caller's ARM or Thumb instruction state.
It follows the returning wrapper in `checks/a733-watchdog-expiry.S`.

Before returning, the launcher publishes a refusal reason and the request's
epoch, generation and lease in the report, followed by its refusal marker.
The first device write ends the clean-return phase. Post-mutation failures,
an unexpectedly active watchdog and unsafe architectural contexts still park.
They do not retry, feed, disarm or branch to an alternate image.

## ROM observation does not establish a scheduler result

After execution, the host passively observes the physical port for up to
20 seconds. A distinct instance must regain permissions and satisfy ROM access
checks before receiving a version request. A successful response records
`scheduler_result: unobserved` and `complete: false`.
The reset cause, launcher entry and scheduler result remain unobserved.
Neither the transient SRAM report nor the previous DRAM result is read after
reset. DDR must be initialized again before reuse.

If no departure was observed and the original identity remains accessible, the
host permits one bounded refusal-report probe after that window.
This probe is permitted only if the execution request completed without a
transport failure. Unchanged USB metadata does not prove that the CPU returned
to ROM; the probe can fail while the CPU is still stalled.
The captured ROM ends before the FEL execution dispatcher, so the execution
acknowledgement is not treated as proof of callback completion.

A successful report transfer must contain the fresh epoch, generation, lease,
pre-mutation reason and refusal marker, with zero arm times and reserved fields.
The host then confirms the disabled watchdog and restores each saved SRAM region
with a readback comparison. It records a clean refusal and `sram_restored: true`.
The DDR reservation stays with the coordinator. Both a clean refusal and a
new-instance observation exit with status 2; neither completes the queue item or
reports a scheduler success. A failed or mismatched probe retains ownership and
receives no retry.

The first transport failure ends device communication. There is no automatic
retry, host USB reset or second execution request. If an attempted launch has
an unresolved outcome, including an observation timeout or interruption, the
process holds the board flock and leaves the queue claim in place.
It waits for independent coordinator intervention; Ctrl-C does not release that
ownership. Before an execution attempt, a failure retains the queue claim and
quarantines the instance. Downloaded backups remain available for review.

## Linker reservations separate code, task memory and monitor state

| Range | Purpose |
| --- | --- |
| `0x41000000..0x41200000` | Read-only executable code at EL0 and EL1 |
| `0x41200000..0x42000000` | Writable non-executable scheduler data and eight EL1 stacks |
| `0x42000000..0x42004000` | Diagnostic result reservation |
| `0x42004000..0x42005000` | Immutable host contract page |
| `0x42005000..0x42200000` | EL1-only, non-executable mailboxes, continuation records, tables and EL3 stacks |

The linker names both result and contract reservations and rejects overflowing
allocations. Startup clears ordinary and monitor BSS independently, preserving
the contract and previous result until admission checks finish.
The result header records the epoch, generation, stage, status, detail and counter
timestamps. Stage 5 is terminal; status 0 and completion mask `0xff` report the
implemented diagnostic's success. Status 1 means admission refusal, 2 means boot
failure, 3 means deadline expiry, and 4 means a scheduler diagnostic failure.
Secondary faults also retain per-core continuation or diagnostic records.
Their ELF symbol addresses appear in the image-check evidence.

## Focused evidence preserves the executed inputs

Run the separate checker after integrating the continuation dependency:

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-smp-image.py \
  --freertos ~/.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2 \
  --evidence /absolute/new/evidence-directory
```

The checker snapshots the repository dependencies and pinned FreeRTOS inputs,
builds outside the repository, and refuses an existing evidence directory.
It checks contract tampering, deadline refusal, watchdog-source refusal and
per-core MMIO boundaries under AddressSanitizer and UndefinedBehaviorSanitizer.
It inspects ELF segments and reserved symbols, then executes the linked scheduler
interrupt calls for all eight destinations. Linked shared-lock checks cover primary
and secondary completion, a missing peer and a held lock with deadline expiry.
The instruction model does not reproduce physical inter-core contention.

Reusable evidence is in
`~/.cache/sunstoneos/evidence/a733-smp-image-20261016`.
The initial configure needed the available Make generator and explicit linker
propagation into compiler checks. Linking then required the runtime `memmove`,
task-delete configuration and vector-table alias. Those failures and corrected
commands remain in the evidence directory. The successful host admission result
was reused throughout. The continuation's corrected EL3 reset constants required
only its assembly object, build identity and final link to change.
The image manifest records executed and final artifact identities separately.

## Launcher and host evidence cover the new preparation protocol

The launcher instruction checker executes the linked AArch32 bytes with modeled
registers and counter reads. Its cases cover admission, contract mismatch,
ignored register writes, counter rollback, overflow and elapsed-time refusal.
The host checker uses a fake transport and a private SQLite queue to cover every
request failure point, corrupt readback, changed claims and missing prerequisites.
It verifies one execution request and an unobserved scheduler result after ROM
observation. Neither checker discovers or opens a board.

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-smp-launch.py \
  --bundle /absolute/prepared-directory --output /absolute/launcher-results.json
python3 checks/a733-smp-host.py \
  --bundle /absolute/prepared-directory --output /absolute/host-results.json
```

Reusable evidence is in
`~/.cache/sunstoneos/evidence/a733-smp-launch-20261016`.
The earlier 2,036-byte launcher passed 28 instruction cases; its host adapter passed
41 protocol cases. Focused host checks also cover ownership retention when
evidence flushing or quarantine fails. Host checks were repeated for durable
backup flushes, source-identical evidence after integration and failure retention.
The successful launcher cases were reused. These checks do not model oscillators,
interconnect stalls, warm reset completion, AArch64 entry or physical scheduling.
The earlier image, SGI and CPU-entry evidence remains valid and was not rerun.

The current 2,220-byte launcher and refusal adapter have separate evidence in
`~/.cache/sunstoneos/evidence/a733-smp-refusal-20261016`.
Sixteen focused instruction cases verify caller-state restoration, ARM and Thumb
returns, pre-mutation refusals, unsafe-context parking and the first-write boundary.
Seventeen focused host cases verify fresh refusal reports, failure stopping,
SRAM restoration, obsolete-layout refusal and upload changes after admission.
Use `--focus-refusal` with either checker to select those cases.
The unchanged reset, deadline and scheduler evidence was reused; the current
launcher received a native compile and link. No physical request occurred.

Six additional host cases cover source growth and replacement before and after
sealing, replacement of restoration backups, and descriptor inheritance by an
actual child process. Select them with `--focus-freeze`.
Their evidence is `freeze-results.json` in the refusal evidence directory.
The passing native and refusal-protocol cases were reused for this host-only change.
