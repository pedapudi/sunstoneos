# Offline GPU-to-HDMI preparation

`apps/gpu-display/prepare.py` prepares immutable RAM inputs for the retained
GPU-to-HDMI application. It packs two caller-selected texture inputs with the
HDMI provider configuration and combined physical-admission records. Preparation performs no device or queue access. A separate supervised caller
observes copied GPU hashes, selected pixel samples and presentation/terminal
evidence through the native USB owner. Those observations do not establish
visible pixels or complete framebuffer correctness.

The supported native geometry is a 840-byte authority prefix followed by the
existing 144-byte application input header and two canonical texture payloads.
The entire session must fit in the 1 MiB reservation at 0x41f00000. Preparation
checks the image's fourteen geometry words and DWARF structure sizes and offsets.
It refuses stripped images, unsupported layouts, overlapping loads, writable
geometry, unexpected extended pools and initialized session/monitor allocations.

Supply these files and configured tool paths:

```sh
python apps/gpu-display/prepare.py \
  --image image.elf --manifest manifest.json \
  --hdmi-session hdmi-session.bin --inputs inputs.bin \
  --admission admission.json --output prepared \
  --clang /usr/bin/clang \
  --lld /home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21 \
  --objcopy /usr/lib/llvm-21/bin/llvm-objcopy
```

The image manifest must name `sunstone-a733-gpu-display`, execution profile
`gpu-display-supervised-v2`, workload
`copied-two-frame-HDMI-720p60-usb-observation`, and the
exact ELF and build digests. `inputs.bin` comes from `apps/gpu-display/host.py`.
That producer packs two existing `texture.input_bytes()` results within the
remaining session capacity. The preparer rechecks payload ordering, digests,
profile-2 vertex bits, indices and complete texture extents before assembling the
session. It never substitutes embedded display content for the caller's inputs.

`hdmi-session.bin` is the exact 544-byte native HDMI provider record. Its image
digest must match the ELF, and its entry digest must match the supplied combined
admission JSON bytes. It retains the analog, power, supervision, isolation and
memory record digests. This bounded profile requires inherited PL0/PL1 mux 2,
pull-up and drive code 1, authorization of those two pins only, no GPIO IRQ owner,
and a complete 14-bit PL latch handoff owned by the PMIC provider. These checks
reject impossible constructor inputs; they do not establish electrical state.

## Combined physical admission

The admission schema is `a733-gpu-display-supervised-admission-v2`. It binds the
target identity and USB port, image, input digest, epoch, generation, boot lease,
supervisor, display owner, framebuffer authority and extended-memory authority.
It requires the original 20-second lifetime and combined GPU/HDMI retention.
The external DRAM record must cover exactly 0x41000000–0x42200000,
0x48000000–0x78000000 and 0x78000000–0x79000000.

Every proof reference contains an absolute `path` and its SHA-256. The following
typed records contain the exact condition sets declared in `CONDITIONS` beside
the preparer's validator:

| Record | Required authority |
| --- | --- |
| `analog` | Complete qualified analog image, charge-pump and test/reserved provenance, inactive VO1 and resistor-word ownership; exact HDMI request digest |
| `power` | Inherited PMIC/pin configuration, qualified rails and operating point, shared clock/power lifetime; exact provider bytes |
| `supervision` | One attempt, stop after failure, retained resources and manual whole-board power removal |
| `isolation` | All system-IOMMU masters excluded, prior transactions and walks drained, remote writers excluded, DE/TV0/IOMMU IRQ ownership |
| `memory` | Full extended reservation, no aliases, retained 4 MiB display parent, master-8 read-only display bank, private GPU confinement and cache visibility |
| `gpu_initial` | Existing GPU initial-entry conditions, including initial drain, reset/clocks, mappings, remote-master exclusion and shared ownership |
| `gpu_memory` | Full extended reservation and confined firmware/graphics address paths, with the same display-parent authority |
| `gpu_electrical` | Qualified GPU PLL/supply and shared GPU/HDMI clock/power exclusion |
| `gpu_supervision` | Retention through the combined lifetime and manual whole-board power removal |
| `usb_initial` | Owned ROM PIO lifetime, external DMA exclusion, owned and quiet IRQs 188–190, and no concurrent ROM access |
| `usb_electrical` | Exact four rails, stable shared fabric and PMIC/pin snapshot, and a dedicated retained output sink |
| `usb_supervision` | Manual whole-board power removal, one attempt and retention for the full 20-second lifetime |

The containing version-2 envelope adds a 136-byte USB authority at byte 704,
with application inputs beginning at byte 840. The preparer derives that authority
from the admission fields `usb_owner`, `usb_handoff`, `usb_cold_reference` and
`usb_supply_uv`, plus the three USB record digests. The owner must differ from
HDMI, PMIC and GPU owners 9–11. The cold reference is ROM device mode 2; the four
supplies are exactly 3,300,000, 3,300,000, 1,200,000 and 900,000 microvolts.
The electrical proof binds the existing HDMI fabric settings and rates and the
same inherited PMIC/pin bytes. The USB owner creates no independent provider or
additional session window.

Each physical record must be explicitly reviewed and identified as physical
evidence. That declaration does not establish its assertions. The coordinator
must independently admit the referenced observations for the actual board and
combined lifetime. Nonzero authorities, hashes, software fixtures and register
readback do not supply missing physical qualification. Standalone compact HDMI
records cannot authorize this extended-memory application.

The `programs` record binds trusted graphics profile 2, the resident native source,
the actual retained producer script and its complete compiled-input manifest.
All manifest inputs are frozen, including shader-producing C files, their local
headers, generated includes and compiler dependencies. The retained vertex and
fragment outputs must match the selected ELF's read-only shader arrays; only
zero tail padding is permitted. The Mesa source archive is pinned to revision
`86c7176d2dd0065555521bf8c61f208bf53c75d6`. Later producer-source changes do not
retroactively change the inputs used to generate retained shader outputs.

The GPU executes opaque firmware with SHA-256
`1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`.
The firmware and its `LICENSE.powervr` must match the pins in `sources.lock.json`.
The preparer also verifies the firmware embedded in the ELF. Source availability,
execution locations and remaining GPU qualification are described in
[the GPU component record](gpu.md).

## Bundle and evidence limits

Admission succeeds before the output directory is created or the SRAM wrapper is
built. The bundle contains the ELF segments, full session, fixed request, contract,
zeroed result/report inputs, launcher and frozen proof closure. `validate_bundle()`
rehashes those files, validates admission again, reconstructs the session and
requires the exact upload list and reservations.

The request uses version 2 and the private GPU/display magic. The 288-byte
version-2 contract contains the six-word extended-memory layout, full session
digest at byte 224 and entry digest at byte 256. The root-owned wrapper preserves
the 20-second admission window. Building that wrapper does not execute it or
establish CPU startup, peripheral operation, recovery or memory ownership.

`tools/check-a733-gpu-display-supervised.py` uses the existing coordinator claim,
sealed-upload and retained-recovery flow. Its software prerequisite must cover the
exact image, launcher and host source closure. Its capture window is 19 seconds
within the original native 20-second lifetime. Every request and reply uses the
same retained USB instance and absolute deadline. Copied replies are persisted
before decoding. Every decoded reply must match the frozen epoch, generation,
lease, build identity and full-session digest. Terminal input hashes must also
match the frozen caller inputs.
Success requires the native protocol's healthy GPU, HDMI, DE and PMIC terminal
states and all three ordered presentation records. It does not authorize resource
release, a reconnect retry or reuse of frame memory.

The preparation and capture checks use the selected version-2 image. Version-1
native and host evidence remains frozen in its original scope; it is not replayed
to qualify USB observation. The focused preparation check is
`checks/gpu-display-preparation.py`. Its positive bundle
fixture substitutes only the explicit physical-record predicate, while retaining
the new binding, provenance, assembly and freeze checks. The same fixture must
fail ordinary production validation. Separate linked input checks use the actual
application/GPU validators with address and undefined-behavior sanitizers. Root's six actual generated AArch32 launcher cases cover the valid handoff and
refuse unsupported version, entry digest, layout, memory authority and input
extent. They use the preserved 2,420-byte wrapper and stop before reset with
CPU state, counter and MMIO modeled. Native CPU startup and physical operation
remain unqualified. Evidence is under
`gpu-display-host-20261020/root-launcher/result.json` and `root-frozen/manifest.json`.
The host audit at `gpu-display-host-20261020/root-integration.json` verifies
451 frozen files and all 35 current sources without rebuilding the native image.
