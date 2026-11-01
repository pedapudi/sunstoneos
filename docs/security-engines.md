# A733 security-engine implementation and qualification

The crypto application erases private CPU buffers when its session expires or
the board authority is revoked. This includes copied request data, plaintext,
decrypted output, intermediate hashes and cipher state. Its mutex serializes
erasure with service operations. Controller metadata and DMA backing remain
retained after uncertainty; erasing CPU staging does not establish engine drain.
Unpublished request storage remains USB-owned. The consumer erases that storage
only after acquiring its input-publication flag.

`checks/crypto-staging.py --output DIR` compiles the actual orchestration and
request code with AddressSanitizer and UndefinedBehaviorSanitizer. It checks
complete CPU erasure, unchanged session state, normal orchestration, operation
failures and final publication refusal. The fixture does not instantiate DMA
memory. The scrub performs no controller access, cache operation or callback.
The corrected ownership check is under
`~/.cache/sunstoneos/build/security-1031/staging-ownership-check/`.
The linked AArch64 check in `staging-native/`, under the same build directory,
executes both published-request erasure and preservation of producer-owned
request bytes. Its RTOS and authority callbacks are modeled.
The producer also erases copied request data after copy refusal or post-copy
expiry. `checks/crypto-upload-erasure.py` executes those linked instructions
and checks successful publication separately. Results for SHA-256 and SM3
requests are in `upload-sha-native/` and `upload-erasure-native/`, under the
same build directory; board-liveness and counter observations are modeled.

The [engine](crypto-engine.md), [hash service](crypto-hash.md),
[cipher service](crypto-cipher.md), [entropy service](entropy.md) and
[application](crypto-application.md) records describe the existing interfaces
and their physical evidence limits. No CE cryptographic operation is physically
qualified by these host checks.

## MD5 and SM3 use the existing retained hash path

The hash controller and stream service accept A733 hash selectors 0 (MD5) and
6 (SM3), alongside the existing SHA methods. Include
`a733_ce_digest_methods.h` with `a733_ce_hash.h` to request them through
`a733_ce_hash_start`. Use `a733_ce_digest_size` for the final output size.
The older `a733_ce_hash_*` size helpers describe SHA methods only.

MD5 uses 64-byte compression blocks, a 16-byte state and a little-endian
64-bit message length in software padding. SM3 uses 64-byte blocks, a 32-byte
state and a big-endian length. The first task selects the engine's default IV;
later tasks reuse its opaque output as the next IV. Every compression runs on
CE. HMAC uses the existing inner/outer passes and key hashing on the same engine.
MD5 remains useful for hardware qualification and legacy interoperability;
applications requiring collision resistance must choose another hash.

The implementation adds no allocation, MMIO address, clock point or authority.
Each task still accepts at most 2048 input bytes and returns at most 64 state
bytes. A stream retains its existing absolute deadline and client slot. HMAC
keys remain bounded to 1024 bytes. Task completion permits reuse of these
reserved buffers; it does not authorize release or reset.

`checks/crypto-digests.py` authenticates cached source hashes, compiles the
actual controller, services and native request parser with sanitizers, and
compares results with host OpenSSL through `hashlib` and `hmac`. Its descriptor
model implements compression independently of the production code. The model
does not establish the physical engine's digest byte order.

The check covers 210 streams across MD5 and SM3: fifteen message lengths with
plain hashing and six HMAC key lengths. Lengths include both sides of padding
and 2048-byte submission boundaries. Another 27 application cases cover empty
and `abc` known answers, maximum application messages, HMAC and unchanged
SHA-256. Reserved selectors, wrong digest sizes and completion errors are
refused; completion errors retain memory and leave caller output unchanged.

The host request producer also needs selectors 0 and 6 and output sizes 16
and 32. That shared Python file is outside this stream's implementation
ownership. Its separate patch was checked through an isolated build copy with
the native parser. Until integration, the tracked host preparer refuses these
two methods. Existing SHA and AES requests retain their wire format.

## The silicon inventory includes unfinished hardware functions

The [source manifest](../apps/crypto/security-sources.json) pins URLs, revisions,
SHA-256 hashes and licenses. Cache paths are relative to
`~/.cache/sunstoneos/sources/`. The A733 datasheet section 3.11 establishes
applicability; user-manual section 19 specifies CE descriptors and controls.
Linux's `CONFIG_ARCH_SUN60IW2` selects CE v5. Linux executes none of the native
code. Adapted CE definitions retain their original license notices.

| Hardware function | Native support and remaining evidence |
| --- | --- |
| MD5, SHA-1/224/256/384/512, SM3 | Implemented with software padding and engine compression; host-tested. All require physical known answers, visibility and completion evidence. The pinned Linux caller implements MD5 and SHA; SM3 has a selector but no registered caller in that source. |
| HMAC | The native service composes HMAC over each supported engine hash. Hardware HMAC selector support is documented for SHA-1 and SHA-256 but is not used by this service. |
| AES | ECB, CBC and CTR use native CE modes at 128/192/256 bits. GCM uses engine AES and CPU GHASH; native CE GCM remains missing. CTS, OFB, CFB, CBC-MAC, OCB, XTS and the other CTR widths remain missing. |
| DES, 3DES, SM4, RC4 | The datasheet lists these blocks. Native operations remain missing. Their presence is independent of an application's algorithm choices. |
| Public-key engine | RSA to 4096 bits, ECC to 521 bits and SM2 are applicable. User-manual sections 19.1.4.16–17 describe microcode operations and operand ordering. Native operations remain missing. |
| Conditioned TRNG | Implemented with assessed-profile admission and secure health-register checks. Raw-source assessment, startup completion, physical output and recovery remain unqualified. |
| PRNG and DRBG | The manual describes PRNG and deterministic-generator state operations. Native coverage remains missing; neither substitutes for entropy-source assessment. |
| Hardware keys and key ladders | HUK, SSK, RSSK and internal-key selectors exist; the secure bank has key-ladder controls. Native access and applicable operations remain missing. Existing tasks select caller-supplied keys only. |
| SID and one-time storage | The datasheet describes 4 Kbits of eFuse with protection and SRAM shadows. The board identity path reads four chip-ID words. It establishes no key access, provisioning or mutable secure-storage service. |

No populated eMMC, UFS or designated scratch NVMe exists. Secure storage over
such media cannot be qualified here. A microSD-backed service would require
the storage owner's partition confinement, backup and write authorization.
Reading identity does not authorize eFuse programming or changing protection.

## Raw entropy access remains a reference gap

The CE TRNG descriptor exports SHA-256-conditioned output. The documented
health registers expose cutoffs and enables, with no raw-sample export or
startup-completion counter. The inspected CE v5 path does not supply either
mechanism. These are limits of the inspected references, rather than proof
that no undocumented mechanism exists.

The cached Linux tree also contains a separate `sunxi-trng` driver with jitter
configuration and counter registers. It matches `allwinner,sunxi-trng`;
the selected A733 DTS node instead matches `allwinner,sunxi-ce` and supplies
the two CE apertures. Those layouts cannot be interchanged. No A733 route
from that separate driver to raw CE samples was established, so no register
probe or sampling image follows from it.

The vendor secure-key adapter calls trusted-firmware functions and supplies
weak stubs that fail when the service is absent. That source does not establish
an installed secure-key or entropy SMC service. Enabling an entropy consumer
still requires a documented raw path, source assessment and successful startup
tests for the current enable epoch. Conditioned-output statistics or discarded
request counts establish none of those requirements.

## Physical use waits for ownership and a returning-image contract

The reference CE node requests 400 MHz from PERI0-400 and routes nonsecure
and secure interrupts to INTIDs 200 and 201. The existing fixed clock profile
uses that rate. This change adds no operating point or voltage transition.

The current native application masks both routes and polls completion once
per RTOS tick. IRQ completion remains unfinished. The source handler reads
channel status, acknowledges it and wakes its waiter; the caller then disables
the channel interrupt and acquires DMA output. A native IRQ owner must carry
that completion identity through notification and output acquisition.

The reference stop helper is unsupported. Neither task success nor zero status
proves that predecessor DMA has drained. Before a launch, the coordinator must
establish prior CE writers, exclusion of future secure/nonsecure clients,
secure-bank access, reachable private memory and stable clock ownership.
The [remote-writer audit](remote-writers.md) retains this separate CE requirement.

The built crypto application is a supervised, single-core diagnostic that
parks after completion. Its admission contract retains resources until
whole-board power removal. It has no normal ROM return, no CE-specific RTC
progress sequence and no qualified watchdog recovery path. It is ineligible
for the returning-image queue. No board launch is requested for this artifact.
The coordinator needs a reviewed CE completion/drain and return contract,
matched watchdog/breadcrumb implementation and a qualified runner before
these images can enter a board schedule.

## Host evidence and integration inputs

All evidence below is under `~/.cache/sunstoneos/build/security-1031/`.
Independent review covered the erasure ownership rules, descriptor methods,
padding, test model and shared host-parser patch.

| Check | Result and scope |
| --- | --- |
| `checks/crypto-staging.py` | Passed sanitized orchestration, CPU erasure and unchanged session metadata; see `staging-ownership-check/`. |
| `checks/crypto-digests.py` | Passed 210 stream cases and 27 application cases, with reserved-selector and retained-error checks; see `digests-check/`. Uses the isolated, patched host producer. |
| `checks/crypto-modes-native.py` | Passed linked MD5/SM3 upload and publication checks in `digests-native/`, and complete CPU-erasure/producer-ownership checks in `staging-native/`. CE completion is modeled. |
| `checks/crypto-upload-erasure.py` | Passed linked producer refusal, post-copy expiry and successful publication; see the upload directories above. |
| OS-only, hello-compute and external mimus | All three baseline targets build with Clang 21 through the cached configuration wrapper. Independent OS builds use empty `MIMUS_DIR`; mimus uses its pinned external checkout. |
| Native crypto | `crypto/sunstone-a733-crypto-single-core.elf` builds with empty `MIMUS_DIR`. The built-in diagnostic remains SHA-256/AES; runtime hash requests select MD5/SM3. This is compilation and emulation evidence only. |

The stream supplies separate patches for the shared host request producer,
public-header descriptions and current hardware/handoff/reference records.
Apply the host-producer patch before preparing MD5/SM3 requests. The records
patch links this inventory and preserves every open physical requirement.
The image archive's README gives its SHA-256, expected diagnostic bytes,
failure stages and the reason no coordinator launch command is currently valid.
