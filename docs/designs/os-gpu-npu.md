# Concurrent GPU and NPU services in the OS

Status: **host-tested**. The combined profile is implemented in the OS identity checks (`apps/os/gpu.c`, `apps/os/os.h`, `apps/os/admission.c`) and the mimus checkout's preparation, and the complete native image passes its software checks. Combined physical execution is **open**.

The profile is supervised recorded recognition with eight CPU workers, A76 admission, CPU clock grants, GPU linear dispatch and the selected ShuffleNetV2 NPU application. Performance commands are off. The GPU uses its 1,008 MHz and 960 mV point and the NPU 852 MHz and 800 mV on the supported bins. The finite NPU application is independent of Mimus. [Eight-worker GPU recognition](gpu-eight-workers.md) supplies worker admission and joined CPU readout, and [accelerator composition](../accelerator-composition.md) establishes the provider-sharing pattern (a different application and memory layout). The [resident cold-card path](resident-gpu.md) accepts this composition through the four-extent card producer.

## Integration requirements

| Requirement | Implementation |
| --- | --- |
| Combined build and image admission | CMake and the link checker accept supervised recorded GPU plus NPU; unrelated device exclusions remain |
| CPU clock grants with both accelerators | The profile accepts startup grants without performance commands |
| Complete session size | GPU host admission derives 1,080 bytes with the NPU and 1,176 with the NPU plus the trailing CPU grant |
| Distinct accelerator identities | Native and host admission reject NPU owner aliases with the GPU firmware owner `0x4f53475055464d31` or session owner `0x4f534731` and reject NPU PCK client 7 |
| Fixed executable mapping | Generated OS NPU payloads use `.data.os_npu_assets` |
| Linked NPU asset authentication | Supervised preparation, frozen validation and card preparation share the pointer and hash helper |

The identity checks run in [native session admission](../../apps/os/admission.c) before provider construction and in host admission (`prepare.py`), and the GPU pre-provider guard in [apps/os/gpu.c](../../apps/os/gpu.c) also checks the NPU identity. No session field, queue protocol, loader ABI or memory-manager abstraction is added. GPU authority is an external record bound to the image and supervised lifetime, and the NPU keeps its 272-byte session extension.

## Providers and clock transitions

Both consumers borrow `providers.shared` and `providers.domains`. [The NPU startup](../../drivers/npu/a733_npu_startup.c) acquires its `pck_client` through `a733_gpu_resources_acquire`, and [the GPU](../../apps/os/gpu.c) uses client 7 on the same manager; they share stable oscillator and PCK leases. The NPU may also hold a stable PERI0 lease for its fabric, while the GPU's LOCAL lease owns PLL_GPU0 and excludes PERI0 from its required set, so the leases are compatible with distinct registered clients.

The NPU owns PLL_NPU and the local NPU clocks, holds DCDC2 at 800 mV and registers a domain-4 row. The GPU owns its local clock and reset and CORE6, changes DCDC4 to 960 mV through the PMIC owner and registers a domain-5 row. The [power-domain owner](../../drivers/power/a733_power_domains.c) traverses multiple rows, and its rail and register locks serialize shared accesses. CPU grants keep their CPU PLL and rail owners, and no consumer writes another consumer's supply or domain.

Register serialization does not serialize whole PLL transitions or establish analog independence, and [the NPU PLL audit](../npu-pll.md#enabling-the-pll-while-dram-runs) keeps the manual's cross-PLL disturbance warning. Combined supervised GPU admission therefore adds one condition reference for the simultaneous powered lifetime: NPU PLL enable while GPU, CPU and DDR clocks can be active, and GPU PLL start and stop while the NPU PLL stays enabled. It binds the image, board, epoch, generation and lease of the NPU clock authority, and single-device records cannot satisfy it.

NPU availability does not wait for GPU OPEN, and startup and queues are independent. A short NPU job may finish before GPU recognition starts, which is valid and claims no simultaneous hardware execution. Software overlap is checked through shared-provider fixtures, and a startup rendezvous solely to observe device overlap is outside production scope.

## Memory and scheduling

The executable block is `0x41000000..0x41200000`. ShuffleNet's 964,224-byte model, 150,528-byte input and two-byte reference use initialized XN RAM from `0x41200000`, const and 64-byte aligned. Their CPU mapping is writable, so immutability relies on trusted ownership, and authentication follows the linked pointers. BSS, heap and the 2 MiB write-back device pool end before the noncacheable pool at `0x41c00000..0x41e00000`.

The GPU keeps the Mimus models at `0x44000000` and `0x44800000`, the recording at `0x45000000`, the arena at `0x46000000..0x46c00000` and the CPU read-only view at `0x46c00000..0x47000000`. The NPU's DMA32 pool is `0x47400000..0x47c00000`, and neither device acquires the other's backing.

The recognition owner runs on core 6 and a worker on core 7 at priority 3, with six A55 workers at priority 2. The GPU service (priority 4) and IRQ95 stay on core 6; the NPU bootstrap and service (priority 4), client (priority 3) and IRQ97 stay on core 1; both device IRQ priorities are `0xd8`, and thermal IRQ71 stays on core 0. The exact-sum hook submits only from its owner and joins the CPU readout workers before a GPU result's output is reused, and each service queue keeps its caller, ticket and completion ownership. The combined admission adds no per-job hashes, translations, copies, tasks or queue round trips, and the host authenticates the NPU payload once per validation.

## Thermal and failure

The thermal owner is the only sampler and CPU clock owner. Fresh NORMAL readings admit NPU work, and hot or stale readings request its terminal local stop within five seconds. The GPU recognizer closes a healthy session between completed calls at THROTTLE and reopens its retained backing at NORMAL. CRITICAL or either uncertain accepted-device failure ends the board lifetime, and an unaccepted GPU refusal keeps its CPU fallback. NPU completion retains its stopped resources, GPU CLOSE does not release those leases, and the NPU's immutable report, live atomic stopped proof and finite wait and deadline checks stay intact.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Combined native image sections | 1,138,848 B code, 194,057 B CODE read-only; write-back pool ends at `0x41b27000` with 888,832 B left | `os-gpu-npu-software` |
| Growth over the eight-worker GPU plus CPU image | executable +76,800 B, read-only +1,888 B, writable +1,254,144 B | `os-gpu-npu-software` |
| Host checks; CPU TOP grant checks; linked GPU startup guard checks | 14; 2; 4 | `os-gpu-npu-software` |
| Shared-provider check | 7 service steps, 145 PPU reads, 13 PPU writes, 180 PMIC bus writes | `os-gpu-npu-software` |

Records: [os-gpu-npu.jsonl](../evidence/os-gpu-npu.jsonl). Nominal fixture records assert physical fields only to exercise admission, are nondeployable and supply no board authority.

## Open work

1. Run the combined image on a board and observe GPU and NPU execution together.
2. Qualify the combined cross-PLL behavior and thermal operation; the shared clock condition rests on a manual warning, not a measurement.
