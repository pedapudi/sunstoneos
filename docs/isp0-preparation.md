# Prepare and capture one recorded Bayer frame with ISP0

The ISP0 application processes one recorded 640×480 BGGR12 frame and retains a
CPU-owned copy of its NV16 output. Preparation validates the actual linked ELF, allocator extents and complete IOMMU
page table. The retained SRAM launcher is built from those prepared inputs. Physical operation
and numerical pixel correctness remain unqualified. No camera is required for
this recorded-input path.

Input contains 614,400 bytes: each twelve-bit sample occupies a little-endian
sixteen-bit container, and every sample must be at most 4095. Input stride is
1280 bytes. The native DMA source also needs 64 zero prefetch bytes after the
input; those bytes belong to the allocation and are outside the input digest.

Output contains 614,400 bytes. Its first 307,200 bytes are the Y plane; its next
307,200 bytes are interleaved U and V. Both planes have a 640-byte stride.
The host authenticates the copied output digest and extent. An independent ISP
pixel oracle is unavailable, so a successful capture does not establish numerical
correctness.

## Seal the input

`apps/isp0/producer.py` extends an existing 592-byte supervised board template to
an 880-byte ISP0 session. It binds owner, authority, clock client, four reviewed
physical evidence hashes, input digest and eight fixed geometry words. The
reserved reference digest and three reserved geometry words remain zero.

```sh
python3 -B apps/isp0/producer.py \
  --board /absolute/board-template.bin --input /absolute/input.bggr12 \
  --owner 101 --authority 102 --client 3 \
  --initial /absolute/initial.json --memory /absolute/memory.json \
  --electrical /absolute/electrical.json --supervision /absolute/supervision.json \
  --output /absolute/isp0-template.bin
```

Owner and client arguments must match the admitted provider ownership. The board
prefix binds epoch, generation, lease and native build identity. Preparation
derives fresh runtime identities and preserves the native build identity.

## Prepare the exact native image

`apps/isp0/prepare.py` consumes the actual native ELF, image manifest, session
template, reviewed admission record and exact Bayer input file. It validates the
immutable eleven-word `isp0_geometry` marker, executable entry, ordinary board
memory reservations and protected session region. The ordinary SMP image requires
USB cold-reference mode 1; mode 2 ROM takeover is rejected. The ordinary 224-byte boot
contract binds the complete session digest. The native lifetime is 20 seconds;
the supervisor uses a single absolute 19-second host window.

Launcher preparation checks that the explicit clang and lld paths name executable
files before compiling. It preserves those invocation paths and writes the
complete command journal before starting the first command.

The prepared bundle freezes image, evidence, input, session, launch contract and
upload bytes. Runtime upload contains a 32-byte little-endian typed header,
followed by the exact Bayer input. The header binds magic `0x55495053`, version 1,
input length 614400, reserved zero, epoch and generation. There is no reference
pixel upload.

Physical preparation requires typed, reviewed target and image evidence with
`physical_authority: true` and `modeled: false`. It also requires the actual native
allocator rows and page-table artifact, including permissions, guards and the
prefetch tail. Allocation admission derives WB bounds from the linked ELF, checks every native
allocation row and reconstructs every descriptor in the 32,768-byte table.
The input mapping includes the prefetch tail; source, load and VIPP are read-only.
Output and parameter-save mappings permit reads and writes. Guard pages and
unmapped descriptor entries must remain absent. Software fixtures cannot authorize a
physical launch.

## Capture retained output

The USB identity is VID `1f3a`, PID `efe8`, revision `0100`, product
`sunstoneos native ISP0`, serial `sunstoneos-a733-isp0`. A query is one zero byte.
Pending and failure responses contain only their 192-byte header, retaining the
session identity, fixed geometry and available diagnostics. Copied success adds
614,400 output bytes.

`apps/isp0/host.py` checks epoch, generation, input digest, full session digest,
geometry, output length and output digest. Successful proof also requires native
completion, the ISP/VIPP/DMA completion conditions, TOP pattern START clear,
IOMMU master scan 6, intact guards and copied length 614400. ISP status requires bits 2 and 3 with no bits outside `0x1f`; VIPP status must
be `0x100`. DMA status requires bit 0 with no bits outside `0xa3`. TOP requires
generator bit 0 set, START bit 4 clear and cycle bits 23:16 clear.

The host uses the endpoint's 64-byte full-speed or 512-byte high-speed packet
size. It retains payload received with the header and refuses intermediate short
packets or zero-length packets. If the final read fills its request and the
response is a packet-size multiple, a separate read must consume the terminal
zero-length packet before ACK.

Capture durably saves raw fragments, the full response and `output.nv16` before
sending one 41-byte ACK. That ACK contains byte 1, generation and output digest.
A final query must return status 3 with bytes 16 through 191 identical to the copied
result header and no payload. ACK observation does not release native resources.
All DMA buffers, mappings and provider leases remain retained until admitted
whole-board power removal. A transport failure stops capture immediately.

## Check the prepared software path

`checks/isp0-preparation.py` accepts explicit paths to the real ELF, manifest,
producer session, input, native allocation JSON and raw page table. It creates
explicitly labeled model records, verifies that production refuses modeled
authority, and substitutes only the physical-authority predicate for preparation.
The fixed SRAM launcher is compiled once and preserved in the resulting bundle.

`checks/isp0-host.py` consumes that modeled bundle. Its arbitrary output bytes
exercise transport and digest authentication; they are not an ISP model. Cases
cover both USB packet sizes, pending queries, malformed identities and geometry,
proof refusal, digest corruption, premature termination, failed durability,
upload failure and retained ACK identity. Preparation passed against the preserved ELF and constructor artifacts. The focused host batch passed fifteen capture cases, eight geometry refusals,
four input refusals and thirteen completion-proof refusals. The actual supervisor
entry reached readiness and stopped before queue or device callbacks.

The processing source defaults and setters come from the pinned open GPL-2.0
implementation. Source provenance and native behavior belong to the component
record. Host checks cannot establish physical DMA confinement, cache visibility,
ISP execution, interrupt delivery, recovery or performance.

The preserved model preparation is
`~/.cache/sunstoneos/evidence/isp0-native-20261020/host-prepared/`.
It binds ELF SHA-256
`32eb5b933c9e6146618e16724a59405e86ed79ccae92888b989ed7ae1f82e2c1`.
The retained SRAM launcher SHA-256 is
`6fb4c903dba60c6b200c92a81cf2c732b81c5153371e5c7d35f246636546a217`.
Production refuses the model-only authority. Allocation checks refuse aliases,
permission changes, shifted guarded mappings, populated descriptor holes,
changed geometry and a truncated prefetch mapping. These are software results.

The model fixture uses the pinned shared workload-2 provider producer with
cold USB reference mode 1. Its deterministic Bayer pattern spans samples 0
through 4095. The preceding mode-2 fixture is retained as rejected evidence;
the ordinary SMP image does not admit ROM takeover. The native constructor
allocation and table artifacts bind the accepted cold-reference session.

The exact resumed link and objcopy commands are preserved beside the bundle in
`host-prepared/launcher-resume-commands.json`. They consumed the original compiled
objects and produced the retained launcher without recompilation.
