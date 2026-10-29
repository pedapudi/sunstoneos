# Retained HDMI audio preparation and observation

The HDMI audio image sends a finite stereo PCM source through DMA0 and I2S3 to
the HDMI controller while the existing TV0 hardware pattern supplies video.
Its format is signed 16-bit little-endian stereo at 48 kHz, with 32-bit I2S
slots. The source contains one to 48000 frames, or four to 192000 bytes.
The fixed upload address is `0x41f10000`.

`apps/hdmi-audio/prepare.py` extends the existing HDMI preparation mechanism.
The version-1 656-byte session contains the 440-byte pattern session and a
216-byte audio suffix. It supports offline observation only. The suffix binds PCM, memory and electrical record digests, an audio
authority, the frame count and the existing 104-byte DMA handoff. The packer
requires the actual native image's geometry and result-publication symbols.
It retains the shared HDMI clock profile and checks the expected raw reference
at session byte offset 300. Raw 24 MHz selects 74250000 Hz TMDS; raw 26 MHz
selects the documented approximate 73937500 Hz profile.

The audio admission retains the HDMI analog, power and supervision records.
Additional typed records establish exclusive PCM and DMA memory, physical alias
exclusion, cache visibility, quiet DMA0 producers, prior transaction drain,
owned interrupts and immutable buffers and descriptors. They bind the complete
DMA handoff, PCM range and the complete uncached pool at
`0x41c00000..0x41e00000`. Electrical evidence covers the analog profile,
settled rails, clock and I2S3 route ownership, and sink support for stereo
48 kHz 16-bit LPCM. The digest-bound sink EDID must advertise that support through
a CTA audio descriptor or the CTA Basic Audio flag.

Admission also requires the shared [DMA0 Nonsecure access
record](dma-pool-session.md) through `dma_nonsecure_access`. It binds the reviewed
board identity and port, image, session IDs and complete 104-byte DMA handoff.
The packer freezes the record and every digest-bound supporting observation.
Repeated supporting references are refused. Copied USB capture additionally
requires the USB entry authority described below. Admission does not authorize
security-register probes.

These records describe prerequisites; nonzero IDs and asserted flags do not
establish physical facts. Production preparation rejects modeled authority.
Software fixtures mark their records as models and replace only that authority
boundary while checking the actual session, geometry and input bindings.
Native preparation must still validate the inherited fabric configuration and
observe the actual sink. Memory, DMA, clock and rail ownership remain retained
until manual whole-board power removal.

The packer emits a frozen upload bundle and builds the existing HDMI supervised
launcher. The session hash authenticates the suffix, while the suffix digest
binds the separately uploaded PCM. Preparation performs no device access.

```sh
python3 apps/hdmi-audio/prepare.py --image /cache/audio.elf \
  --session /cache/session.bin --admission /cache/admission.json \
  --pcm /cache/input.pcm --output /cache/new-bundle \
  --clang /path/to/clang --lld /path/to/ld.lld --objcopy /path/to/llvm-objcopy
```

`apps/hdmi-audio/observe.py` archives and decodes supplied RAM-observation files.
It is an offline entry, not an executable live-capture transport. It must never
read running native firmware through FEL. Its output directory must be new,
and its parent directory must already exist.

```sh
python3 apps/hdmi-audio/observe.py --image /cache/audio.elf \
  --session /cache/session.bin --pcm /cache/input.pcm \
  --result /cache/result.bin --ready /cache/ready.bin --output /cache/observation
```

The native result contains 160 bytes. A separate four-byte ready value
publishes its availability for RAM observation. Terminal stages distinguish input-ended retention from retained
failure. Successful input consumption requires the complete PCM extent, DMA
service completion and the observed empty I2S3 FIFO. The result records the
admitted TMDS rate and 48 kHz audio rate. `wire_drained` must remain zero:
DMA completion and FIFO emptiness do not establish serializer completion,
transmitted audio packets or rendered sound.

The result includes the PCM digest and the complete admitted-session digest.
The session digest binds the linked image, epoch, generation and evidence records.
The decoder requires both digests to match the supplied immutable inputs.
A ready value supplied as a file does not establish a coherent observation of a
running board; the result's provenance remains part of the evidence.

The archive durably stores the supplied image, session, PCM, result and ready
bytes before publishing its decoded JSON record. It synchronizes the new archive
directory's parent entry as well as the archived files and directory entries.
A failed archive operation cannot report successful completion. The focused
checks consume the final native producer's exact session and actual result;
software observation does not qualify physical HDMI audio output or recovery.

Preparation and offline observation passed on 20 October 2026 using the final
native result for 2049 frames, or 8196 PCM bytes. This input crosses three source
pages and includes a partial final burst. The host checks passed valid terminal
and retained-failure decoding, refused altered session/PCM/proof/clock/sink
bindings and invalid completion claims, and refused archive write failures.
Production preparation rejected the modeled admission records.

The exact final launcher also passed root's bounded 20-second handoff and
request/contract digest-refusal cases. Host results and immutable inputs are
preserved under
`~/.cache/sunstoneos/evidence/hdmi-audio-host-nc-20261020/`, with the selected-file
snapshot at `frozen/manifest.json`. Root's launcher result is
`~/.cache/sunstoneos/evidence/hdmi-audio-nc-20261020/root-launcher/result.json`.
The native run used instruction emulation and modeled device effects. These
results establish software behavior; physical audio, clock, DMA visibility,
serializer completion and recovery remain unqualified.


## Optional copied USB observation

The optional USB image uses a version-2 816-byte session. Its first 656 bytes
retain the audio layout; a 160-byte suffix binds a separate USB owner and
handoff, initial/electrical/supervision record digests, cold reference 1, four
supply voltages and three shared fabric selections and rates. The USB owner must
differ from the HDMI, PMIC, audio and DMA owners. ROM reference mode 2 is refused.
The packer requires the corresponding revision-2 native geometry marker.

Three additional typed records bind the board identity and port, exact image,
session IDs and USB owner and handoff. They establish cold USB0/USB1 reset and
gate ownership, exclusion of remote PHY consumers and DMA writers, prior USB DMA
drain, owned quiet interrupts, and absence of concurrent ROM or firmware access.
Electrical authority covers the exact rails and fabric profile, stable shared
fabric and a dedicated sink with no VBUS drive. Supervision requires retention
for the full 20-second lifetime, one attempt, no reuse after fault, and manual
whole-board power removal. Existing HDMI, audio and DMA Nonsecure authority
remains required.

Version-2 admission also carries the existing shared `reservation`, `ddr`,
`platform` and `recovery` fields. The packer applies the shared supervised entry
checks for DDR initialization and exclusion, CPU publication and ownership,
platform conditions and demonstrated manual recovery. It snapshots every
referenced observation. The supervised caller checks those same frozen bytes
again when binding the actual board identity and port.

`tools/check-a733-hdmi-audio-supervised.py` uses the existing supervisor and its
reviewed queue entry. It accepts the version-2 finite copied-USB image and the version-3 streaming
image described below. It never
reads running native RAM through FEL. In finite mode, each sequential 32-byte request names the
epoch and generation. The 512-byte reply binds that request, the image build ID,
the complete admitted-session digest, the remaining native lifetime and an
optional immutable 160-byte audio result. All 224 reserved bytes must be zero.
The host reads 513 bytes so the native terminal zero-length packet completes the
512-byte response.

The evidence directory must be new, and its parent must already exist. The
caller synchronizes that parent before receiving replies. Finite capture allows at most 200
requests, one second per exchange and 19 seconds for the whole capture. Every returned byte string is saved and synchronized
before decoding or rejecting a late response. Success requires the published
input-ended result, stopped HDMI, ready USB and no fatal application flag.
Retained faults are archived without inventing absent source observations.
There is no acknowledgment or resource-reclamation command; native replies and
all audio and USB resources remain retained until whole-board power removal.
Copied observation does not establish physical sound or serializer completion,
and `wire_drained` remains zero.

Copied USB preparation and host capture passed on 20 October 2026 using the
actual final image, its exact 816-byte session and the native 512-byte terminal
reply. The native mailbox and TinyUSB full-speed path produced that reply and
its terminal zero-length packet. The native fixture seeded the previously
accepted finite-audio disposition; these cases cover the new publication and
transport without replaying PCM or DMA behavior.

The focused host batch passed 33 framing and admission refusals. It also checked
pending and retained-fault records, the 200-request bound, native deadline
consistency, strict 513-byte reads, archive parent synchronization and failed
archive writes. Stale and late replies remained available as durable raw files
while successful completion was refused. Production admission rejected the
modeled records. The actual supervisor readiness case passed and stopped before
queue or device access.

The copied-capture host results and selected immutable inputs are preserved at
`~/.cache/sunstoneos/evidence/hdmi-audio-usb-host-corrected-20261020/frozen/manifest.json`.
The source provenance record distinguishes the Python bytes included in the
native image identity from the external producer and capture implementation.
These software results do not qualify physical USB, HDMI audio or recovery.

## Authenticated streaming preparation and capture

The streaming image accepts stereo S16LE PCM at 48 kHz, with one to 576,000
frames within the admitted 20-second lifetime. Its version-3 session contains
848 bytes. The final 32 bytes authenticate a period manifest; the existing PCM
digest authenticates the complete original file. Preparation requires the linked
128-byte streaming geometry marker and the matching image identity.

The manifest has a 64-byte header and one SHA-256 digest for each ordered
1,024-frame period. The final digest covers only the supplied frames. The header
binds the full PCM digest, frame count, format and exact manifest length. Its
maximum size is 18,080 bytes. The bundle uploads `manifest.bin` at `0x41f10000`
and preserves `input.pcm` for the USB producer. The memory authority's
`pcm_range` covers that manifest extent in a streaming session. Bundle validation
reconstructs the manifest and checks every upload address and frozen file.

The supervised caller accepts either the finite copied-USB session or the
streaming session. Streaming uses version-2 requests with a 48-byte header.
Each WRITE supplies the next complete period. Host writes contain at most
512 bytes, and all fragments share one absolute exchange deadline. The native
producer publishes a staging slot only after receiving the complete payload.
The host sends END after the last accepted period, including an exact short
final period. STATUS uses the accepted period count and waits 22 milliseconds
between queries when staging credits are exhausted. The request budget is twice
the declared period count plus 64; each exchange retains the one-second bound
and the capture retains the 19-second bound.

Streaming replies retain the 512-byte copied response and terminal zero-length
packet. Their 40-byte extension reports staging credits, command disposition,
accepted, committed and consumed frames, and the echoed command sequence.
The host verifies those counters against its submissions and earlier replies.
It saves each raw response before decoding, including malformed and late
responses. A transport failure ends capture without retry or cancellation.

The version-2 audio result permits partial source consumption in retained failure
and adds a canceled stage. Healthy completion requires explicit END, all declared
frames accepted, committed and consumed, DMA completion, an empty FIFO, stopped
HDMI and ready USB. Cancellation requires the native service's actual CANCELED
result and remains a separate retained outcome. The host never
interprets cancellation, a timeout, FIFO emptiness or USB delivery as serializer
completion. All outcomes retain resources until whole-board power removal.

The host protocol model exercises a 49,153-frame input with a short final period,
manifest and session corruption, invalid commands and counters, early EOF,
fragmented device writes, retained cancellation, stale or late replies and failed
archive writes. The preserved finite copied response also passes the existing
host refusal and durability checks. Results are under
`~/.cache/sunstoneos/evidence/finish-hdmi-host-1028/`. These checks use modeled
transport and establish no physical USB throughput, audio continuity or recovery.

The streaming ELF used for the native completion checks, with SHA-256
`5c3c32840187ded24a755873da51012c28c475780bc80da752b18f19ffff3f90`,
passes preparation with a 50,177-frame model input. The actual producer creates
an 848-byte session, exact manifest and supervised launcher bundle. Production
preparation refuses the model authority. With only that authority boundary
replaced, the actual supervisor caller accepts the frozen bundle and reaches
readiness before queue or device access. The evidence is in
`finish-hdmi-host-1028/mmio-image/` and `finish-hdmi-host-1028/mmio-entry.json`
under the evidence directory above. Targeted cancellation checks refuse pending,
complete and failed DMA dispositions when a copied result claims cancellation.

The host decoder also accepts the actual full-speed and high-speed terminal
replies from that image's native USB fixture. Each reports 50,177 frames accepted,
committed and consumed, with 200,708 source bytes consumed, completed DMA, an
empty FIFO and `wire_drained=0`. The host durably archives both copied replies
and the decoded result with the exact image, session and PCM. This evidence is
in `finish-hdmi-host-1028/actual-reply/result.json`; native playback and USB
completion remain instruction-emulated observations with modeled devices.

The provider's controlled-stop correction uses the successor ELF with SHA-256
`54abd2ead295d72bb575d1c682e6b93a09ec48a8cd2d3cb3ec083c5dd39c94d3`.
Preparation and the actual supervisor entry pass for its generated session
`0ec2470e8d1f2b67ac57044d431358e03dee392cbccee0d50755d8a294d4b272`.
The bundle and entry evidence are in `controlled-stop-image/` and
`controlled-stop-entry.json` under the same host evidence directory. The host
and USB code are unchanged; the copied-reply evidence above retains its original
image and session identities. The native component evidence covers the affected
provider stop cases.
