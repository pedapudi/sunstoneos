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
`include/a733_ce_native.h`. These files provide the common command controller. The [SHA and HMAC stream
interface](crypto-hash.md) and the [AES mode interface](crypto-cipher.md)
share its actual retained session with entropy. Hardware key selection and
asymmetric APIs remain required.

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

The native cache adapter uses the shared AArch64 device-cache helper. That helper
refuses an unknown or larger-than-64-byte writeback granule, a caller outside
EL1 and a disabled MMU or data cache. Before submission, the adapter cleans the
descriptor and cleans and invalidates the output allocation. After successful
task completion, it invalidates the output allocation and completes barriers
before the CPU reads it. Once the random block is copied and the task stopped,
the controller zeros the output line and cleans it, so the block does not
remain in the retained page. A TRNG task neither reads nor writes the IV and
input lines, so its scrub does not zero or clean them; the SHA-256 scrub described in
[crypto-hash.md](crypto-hash.md) already zeros them after each digest. When a
TRNG request replaces a SHA-256 request that was configured but not yet
admitted, the controller zeros the IV and input lines before the TRNG task is
prepared. The driver copies bytes to the caller
only after its final deadline check. It returns the result once; subsequent
polls report that the result was consumed.

## Clock, security and health admission

The controller borrows the actual shared clock registry token. CE ownership is
exclusive; PERI0, REF, OSC, FABRIC and RC16M ownership must remain stable. Every
writer uses the common register mutex. The native path observes the adopted
400 MHz CE clock, PLL factors, disabled modulation, forced enabled parent gates
and their effective status. It checks both CE reset releases, local gates, the
AXI leaf gate and the admitted AHB, NSI and MBUS source/divider words. The PLL
factor check is the shared `a733_clock_peri0_valid` rule in
`include/a733_clock_reference.h`, also used by the thermal and ADC drivers.
Reserved PERI0 bits 24:23 do not select a source, so a set bit 24, as
observed on the board, is admitted. This first
profile does not change clock parents, dividers, gates or resets.

The TRNG sample clock derives from RC16M divided by eight. Native admission
checks RC16M enable at RTC `0x07090160` bit 0 and retains its separate resource
scope. That bit does not establish oscillator quality or frequency. Supply,
clock-path and remote-firmware authority remain physical binding requirements.

## The driver follows the vendor's non-secure sequence

The driver does what the vendor's non-secure Linux driver for this SoC does (`sunxi-ce`, SUN60IW2, in the pinned
BSP sources under `rng/`), and nothing more. It uses the non-secure bank only. It enables the TRNG's second entropy
source in `CE_REG_LPC` (nonsecure `0xd0`) bit 1 (`v5/sunxi_ce_reg.c:40-49`, `v5/sunxi_ce_reg.h:38-61`,
`sunxi_ce.c:1218-1221`), and programs no health-test register: the device tree lists a secure bank at `0x04603800`
(`sun60iw2p1.dtsi:2401-2411`) but the vendor driver's TRNG path never touches it, and the secure monitor calls in
`smc_process/` serve key blobs only. The TRNG health tests therefore run at the hardware's reset defaults, and this
driver neither programs nor reads them. The health fields of `struct a733_ce_config` are reserved and must be zero. If
a later stage needs the secure bank (key storage), it comes back with a board result.

The vendor bring-up order is: deassert the reset, enable the bus gate, enable the module clock with its parent
`PLL_PERI0_400M` at 400 MHz, enable the MBUS gate and the system gate, then set the second entropy source
(`sunxi_ce.c:1150-1225`). `a733_ce_clock.c` follows it from a CE held in reset and gated. The board's CE is not: at FEL
entry it reads `CE_CLK_REG` `0x81000000` and `CE_BGR_REG` `0x00030003` (queue entry 1793756349340594212-e9227edb), so an
enabled CE is first taken down by the exact reverse of the bring-up, each step read back. That reset-and-gate cycle
(`CE_BGR_REG` bits 17 and 16 for the resets, 1 and 0 for the gates) is the exclusion of whatever DMA the boot ROM left.

## Completion and faults

Channel-2 status must equal successful completion, and the non-secure error word
must remain zero. Any failure status or error prevents output publication. The driver
rejects every unrecognized error because the nonsecure manual table omits the health codes.

Inherited queue activity, enabled IRQ ownership, stale errors or a disabled second
entropy source cause refusal. GIC INTIDs 200 and 201 must already be owned and masked;
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

No host check covers the command model (descriptor decoding, complete address
writes, error rejection, clock/health refusal, cache handoff, retained faults
and a final deadline failure while registry retention is busy). The one-request
implementation's two native objects hold 8,336 bytes of code and no static data
or BSS; its largest direct compiler-reported stack frame is 336 bytes, which is
not a whole-call-chain bound. The recorded fixtures provide no evidence about
physical randomness; see [crypto-engine.jsonl](evidence/crypto-engine.jsonl).
Exact inputs and artifacts are preserved in
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
Full engine coverage still requires the engine's remaining symmetric modes
(CTS, OFB, CFB, CBC-MAC, OCB, XTS and its native GCM), DES, 3DES, SM4 and RC4,
asymmetric algorithms, key ladders and secure-key operations.
MD5 and SM3 are implemented and host-tested through the retained hash path;
[the security inventory](security-engines.md) records their evidence and limits.
The first TRNG command does not satisfy those remaining hardware requirements.
