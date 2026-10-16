# A733 crypto-engine random output

The native crypto-engine controller implements sequential 32-byte hardware
random-number requests under one retained reservation. It uses the A733 TRNG and its SHA-256 conditioner through the shared
crypto-engine DMA interface. The driver requires an existing authenticated
hardware health profile, can provision it while tests are disabled, and retains
the engine and memory after completion. The [entropy service](entropy.md) composes
repeated tasks using one allocation from the actual linker memory pool.
Physical randomness, health-test efficacy and board operation are unqualified.

The implementation is in `drivers/crypto/a733_ce.c` and
`drivers/crypto/a733_ce_native.c`. The interfaces are `include/a733_ce.h` and
`include/a733_ce_native.h`. These files provide the common command controller. The [SHA-256 stream
interface](crypto-hash.md) shares its actual retained session with entropy.
Cipher, other hash and key APIs remain required.

## Hardware path and memory ownership

The A733 has nonsecure registers at `0x04603000` and secure registers at
`0x04603800`. Four task channels share the engine in each security world.
The implemented requests use channel 2. SHA and entropy use the hash/TRNG
execution unit; [AES block transforms](crypto-cipher.md) use the symmetric unit.
Both secure and nonsecure clients must be excluded throughout this reservation.
The platform must establish initial DMA drain independently before admission.
The task-status register identifies a running channel; zero does not prove idle.

A 208-byte hash descriptor occupies a private 256-byte allocation. SHA-capable
sessions also reserve separate cache lines for intermediate state and input.
AES uses a 220-byte descriptor in the same reservation and adds separate cipher
output and key lines. The complete 3520-byte object fits within one retained page. The 32-byte
result occupies a separate private 64-byte allocation. Sixty-four-byte alignment
protects cache-line ownership; the manual permits byte-addressed descriptors.
Both allocations require authenticated CPU mappings, physical RAM extents,
CE reachability, cache ownership and exclusion of aliases and remote accesses.
Descriptor address fields support 40 bits. That encoding does not establish that
all such addresses reach RAM on this board or pass its security controls.
The system IOMMU's camera mapping is not reused or assumed to cover CE.

The descriptor selects channel 2, the last package, task interrupt notification,
TRNG and SHA-256 conditioning. Its first scatter entry contains only the output
address and a 32-byte destination length. Every unused input, key, IV, scatter
entry and chain pointer is zero. The driver writes both task-address registers,
including the upper byte for addresses below 4 GiB, then writes hash doorbell
bit 10. No seed or deterministic fallback is available.

The native cache adapter verifies a 64-byte data-cache line. Before submission,
it cleans the descriptor and cleans and invalidates the output allocation.
After successful task completion, it invalidates the output allocation and
completes barriers before the CPU reads it. The driver copies bytes to the caller
only after its final deadline check. It returns the result once; subsequent
polls report that the result was consumed.

## Clock, security and health admission

The controller borrows the actual shared clock registry token. CE ownership is
exclusive; PERI0, REF, OSC, FABRIC and RC16M ownership must remain stable. Every
writer uses the common register mutex. The native path observes the adopted
400 MHz CE clock, PLL factors, disabled modulation, forced enabled parent gates
and their effective status. It checks both CE reset releases, local gates, the
AXI leaf gate and the admitted AHB, NSI and MBUS source/divider words. This first
profile does not change clock parents, dividers, gates or resets.

The TRNG sample clock derives from RC16M divided by eight. Native admission
checks RC16M enable at RTC `0x07090160` bit 0 and retains its separate resource
scope. That bit does not establish oscillator quality or frequency. Supply,
clock-path and remote-firmware authority remain physical binding requirements.

Hardware health configuration resides in the secure register bank:

| Offset | Meaning |
| --- | --- |
| `0x90` | Repetition-count cutoff |
| `0x94` | Adaptive-proportion cutoff |
| `0x98` | Delay between test windows |
| `0x9c` | Test enables and adaptive-proportion window size |

Both tests are disabled at reset. The manual's threshold calculations require
an entropy-source bound, and the inspected A733 kernel and U-Boot paths supply
no characterized threshold profile. The API therefore requires an authenticated
profile for this silicon, supplies, RC16M and double-entropy-source mode. It reads the actual registers and rejects mismatches. It accepts
continuous testing with the manual's 512- or 1024-sample window encodings. It
provides no threshold defaults. Optional cold provisioning requires both tests
initially disabled, writes supplied cutoffs and the window, then enables tests
last. Native writes refuse live reconfiguration and preserve reserved bits.
These operations establish neither startup-test completion nor entropy quality.
Output remains unqualified for production security use; counting conditioned
requests cannot prove successful startup testing for the current enable epoch.
The controller enables the source-defined second entropy source at nonsecure
offset `0xd0` bit 1 while the engine is excluded and quiescent.

Native secure-register access requires actual security permissions. A platform
that cannot provide those permissions must supply a trusted secure service
before using this driver. No existing SMC entropy service is assumed. The MMIO
adapter exposes only the needed queue/status and health registers; it does not
read keys, eFuses, secure-debug configuration or key-ladder state.

## Completion and faults

Channel-2 status must equal successful completion, and both banks' error words
must remain zero. Any failure status or error prevents output publication.
The secure error definitions include `0x43` for repetition-count failure,
`0x4c` for adaptive-proportion failure and `0x4f` for both. The driver also rejects
unrecognized errors because the nonsecure manual table omits those health codes.

Inherited queue activity, enabled IRQ ownership, stale errors or disabled health
tests cause refusal. GIC INTIDs 200 and 201 must already be owned and masked;
the controller polls status without routing an interrupt. It acknowledges only
its channel's completion and restores its interrupt-enable bit while preserving
other register fields.

Timeout, reversed time, uncertain MMIO and cache failures retain all authority.
The common register mutex remains held after an uncertain operation under it.
If registry retention is busy, later polls retry only that metadata operation.
Caller output remains unchanged on every failure. No timeout reset, task retry,
memory release or automatic clock removal occurs.

Task completion permits acquisition and copying of this task's result. It does
not prove that all engine transactions have drained or authorize resource
reclamation. Rearm observes clear status, errors and launch bits before reusing
the same retained buffers. There is no resource-release operation. Platform
authority must remain immutable across the complete retained lifetime, including
all later requests. Per-request deadlines bound execution only.

## Evidence and remaining coverage

The initial command checks are `checks/a733-ce.c` and `checks/a733-ce.py`. The model
uses deterministic DMA fixtures to check descriptor decoding, complete address
writes, error rejection, clock/health refusal, cache handoff and retained faults.
It also covers a final deadline failure while registry retention is busy.
For the initial one-request implementation, the sanitizer model, pinned-source
checks and native AArch64 compilation passed. Its two native objects contained 8,336 bytes of code and no static data or BSS.
Its largest direct compiler-reported stack frame was 336 bytes; this is not a
whole-call-chain stack bound. These fixtures provide no evidence about physical
randomness. Exact inputs and artifacts are preserved in
`~/.cache/sunstoneos/evidence/ce-20261016/batch1/`.

Primary references are A733 manual sections 19.1.4.14, 19.1.7, 19.1.8.34–37
and 4.1.6.117–118; the board kernel's A733 CE v5 selection, register driver,
CCU and RTC sources; and U-Boot's CE3 descriptor and TRNG request. The board
kernel revision is `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`; U-Boot is
`b791be842935b27268ae3d00e943a9075495f30a`. Source hashes and validation artifacts
are preserved outside the repository. Linux v6.12 mainline's CE driver targets
older variants and uses a different TRNG descriptor, so it is not the source
for the A733 command format.

Cold clock provisioning, production platform authority, startup and entropy
qualification, interrupt service and physical qualification remain required.
Provisioning and repeated-request evidence is recorded in [entropy.md](entropy.md).
Full engine coverage still requires cipher modes beyond AES-128 ECB, other
hashes and HMAC, asymmetric algorithms, key ladders and secure-key operations.
The first TRNG command does not satisfy those remaining hardware requirements.
