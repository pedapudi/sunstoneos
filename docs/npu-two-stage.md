# Two Conv2D and ReLU stages in the native NPU application

The native application supports a fixed two-stage UINT8 convolution graph as
catalogue entry `subsample2`. The pinned host compiler accepts the graph, and
native assembly matches the pinned SDK's prepared buffers. The retained live-input
application constructs it, accepts two different inputs, copies both saved
numerical outputs and closes through its existing NPU service contract in an
instruction check. NPU completion, numerical output and IRQ effects are modeled
in that check. Physical inference and memory confinement remain unqualified.

This diagnostic covers a sequence used by unchanged mimus convolutional
subsampling: Conv2D, ReLU, Conv2D, ReLU. It scales tensor dimensions down for
functional coverage. It does not replace or compile the mimus model, whose mixed
precision, remaining operators, resource requirements and numerical behavior
still require support.

## The emitted graph fixes tensor and resource requirements

Both convolutions have a 3×3 kernel, stride two and one-cell padding on every
edge. NCHW shapes are `1×1×8×8` input, `1×2×4×4` intermediate and `1×2×2×2`
output. Each convolution includes bias and is followed by ReLU. The compiler
fuses each activation into its convolution; the emitted operations are INIT
followed by two NN records. No tensor-processor or external-pool operation appears.

The compiler is the existing ACUITY 6.30.22 / Vivante IDE 5.11.0 image pinned in
[npu-compiler.lock.json](../tools/npu-compiler.lock.json). It executes on the host;
the native runtime uses the existing FreeRTOS decoder, assembler, wrapper,
resource providers and copied-client service. The model is NBG `0x20000`, target
`0x1000003b`, 2,720 bytes, SHA-256
`6110a0d5a6146cd31473c217060ec2ae875025f25a39fa993476c5912d6a8544`.
The [catalogue](../tools/npu-models.json) fixes model, input and reference hashes,
exact dimensions, quantization and assembly counts. A caller cannot add entries
with a hash, checksum or runtime upload.

| Resource | Emitted or SDK-prepared bytes | Native allocation |
| --- | ---: | ---: |
| Two NN descriptors | 384 | 4,096 |
| Coefficients | 512 payload + 128 SDK tail | 4,096 |
| Command stream | 104 body + 56 completion footer | 524,288 |
| Input | 64 logical, 128 SDK minimum | 4,096 |
| Output | 8 logical, 128 SDK minimum | 4,096 |
| External working pool | 0 | 0 |
| VIP internal SRAM | 101,376 | Existing exclusive SRAM admission |

The intermediate uses VIP SRAM offsets 98,304–98,560 with a 256-byte ring extent.
Eight descriptor patches and two command-state patches bind both stages to the
selected addresses. The assembler emits one synchronization/event/END footer
after both operation bodies. The runtime therefore expects one completed graph
job, with the existing counter rule of one bootstrap plus successful jobs.
This command structure does not qualify physical interrupt delivery or prove
undocumented descriptor access extents.

The existing six payload reservations and 265-page combined extent remain
sufficient. The bootstrap, descriptor, coefficient and command allocations retain
immutable digests. Input publication, matched DONE, output cache finish, copied
output, explicit REAP and healthy CLOSE use the existing service unchanged.
Buffers and shared leases remain owned through the retained session. There is
no model switch or externally owned intermediate buffer.

## Padding is excluded from the model CRC

The emitted coefficient payload starts at byte 2112, leaving 28 zero bytes after
the command-state range table ends at 2084. The CRC over the concatenated
declared payloads is `0xe864f719`, which equals the stored value; including the
gap yields `0xa44bd11c`, which does not. The CRC therefore covers each declared
payload and skips alignment gaps. All listed section CRCs also match.

[Native checksum admission](nbg-checksum.md) preserves independent section CRCs,
section bounds and overlap checks. It requires ascending payload order, zero gaps
and no trailer. The decoder bounds section-relative table entries, and assembly
copies descriptors and coefficients only from those declared sections. Excluded
gap bytes cannot enter loaded payload through those offset paths. The earlier
three emitted models retain their interpretation. Synthetic whole-tail padding
and appended-zero-CRC assumptions follow the same payload-only coverage.

## Functional evidence uses the actual emitted artifact

[npu-two-stage-fixture.py](../tools/npu-two-stage-fixture.py) computes both stages
using exact integer cross-correlations and applies ReLU after each. Its dyadic
scales are one eighth for input/weights, one sixty-fourth for the first bias and
intermediate numerator, and one five-hundred-and-twelfth for the final numerator.
Both stages contain positive and negative pre-activation values. The saved float
CPU output equals the eight exact outputs. Quantized CPU and SDK simulator
outputs match all eight bytes. This one fixture comparison does not establish
application-model calibration or accuracy.

[npu-conv-input.py](../tools/npu-conv-input.py) gives the simulator input a
`.qnt.tensor` suffix and requires its consumed-input dump to equal the requested
UINT8 bytes before accepting numerical output.

The first output is `[0,255,57,0,35,0,0,96]`. A second same-shape input, formed by
adding 24 to each input code with saturation, produces
`[0,254,57,0,38,0,0,73]` in the same compiled SDK graph. Both outputs are nonconstant
and distinct. Host output comparison is optional in the ordinary runtime API.

Evidence is under `~/.cache/sunstoneos/evidence/npu-two-stage-20261019/`:

- `numerical-reference.json` checks saved float, quantized CPU and SDK data.
- `sdk-preparation/trace.json` records bounded AArch64 SDK preparation through
  intercepted submission; no device command executes.
- `native-sdk-comparison/comparison.json` matches every prepared allocation after
  address relocation and completion-event selection.
- `padding-check.log` records fifteen ASan/UBSan cases for the changed checksum
  coverage and malformed boundaries, including independent section CRC failures.
- `linked-graph/result.json` records actual linked construction.
  `linked-runtime-corrected/result.json` records two live-input
  request/result/REAP lifetimes followed by healthy CLOSE, with service and saved
  numerical output modeled.
- `host/result.json` checks production preparation and selected-model identity.
  `host-capture-corrected/result.json` checks corrected
  ordinary copied output and explicit runtime USB profile with scripted transport.

Existing controller, mailbox lifetime, fixed-format checksum and IRQ-route checks
are reused where their source and behavior are unchanged. Astra found no concrete
integrity, descriptor/pool lifetime or completion-structure defect. The frozen
source closure comes from the 74 objects in the actual link, rather than unrelated
compile-command targets. A later API-comment correction is recorded separately
without rebuilding the checked image, as required by selective validation.

Physical NPU inference, SRAM routing/confinement, interrupt signaling and board
recovery still require the existing independently authenticated board procedure.
This work performs no board access and supplies no launch admission authority.
