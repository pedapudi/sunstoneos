# Concurrent GPU and NPU services in the OS

The first combined profile is supervised recorded recognition with eight CPU
workers, A76 admission, CPU clock grants, GPU linear dispatch and the selected
ShuffleNetV2 NPU application. Performance commands remain off. The GPU uses
its existing 1,008 MHz / 960 mV point; the NPU uses 852 MHz / 800 mV on the
supported bins. The finite NPU application remains independent of Mimus.
The software integration is implemented and checked with the complete native
image. Combined physical execution remains unqualified.

[Eight-worker GPU recognition](gpu-eight-workers.md) supplies worker admission
and joined CPU readout. Combined-device admission checks owner identities,
asset placement and the complete session. Existing
[accelerator composition](../accelerator-composition.md) establishes the
provider-sharing pattern; it is a different application and memory layout.

## Concrete conflicts and changes

| Integration requirement | Implementation |
| --- | --- |
| Combined build and image admission | CMake and the link checker accept supervised recorded GPU plus NPU; unrelated device exclusions remain |
| Ordinary CPU clock grants with both accelerators | The combined profile accepts startup grants without performance commands |
| Complete session size | GPU host admission derives 1,080 bytes with NPU and 1,176 with NPU plus the existing trailing CPU grant |
| Distinct accelerator identities | Native and host admission reject NPU owner aliases with GPU firmware owner `0x4f53475055464d31` or session owner `0x4f534731`, and reject NPU PCK client 7 |
| Fixed executable mapping | All newly generated OS NPU payloads use `.data.os_npu_assets` |
| Actual linked NPU asset authentication | Supervised preparation, frozen validation and card preparation share the pointer/hash helper |

The identity checks run in [native session admission](../../apps/os/admission.c)
before provider construction and in [host admission](../../apps/os/prepare.py).
The existing GPU pre-provider guard also checks the NPU identity. The GPU
constants reside in its existing `dispatch.h` interface.
No session field, queue protocol, loader ABI or memory-manager abstraction is
needed. GPU authority remains an external record bound to the image and
supervised lifetime; the NPU retains its existing 272-byte session extension.

## Providers and clock transitions

Both consumers already borrow `providers.shared` and `providers.domains`.
[The NPU startup](../../drivers/npu/a733_npu_startup.c) acquires its
`pck_client` through `a733_gpu_resources_acquire`; [the GPU](../../apps/os/gpu.c)
uses client 7 on that same manager. They share stable oscillator/PCK leases.
The NPU may additionally hold a stable PERI0 lease for its admitted fabric.
The GPU's LOCAL lease owns PLL_GPU0 and excludes PERI0 from its required set.
These are compatible leases with distinct registered clients.

The NPU owns PLL_NPU and local NPU clocks, retains DCDC2 at 800 mV and registers
a domain-4 row. The GPU owns its local clock/reset and CORE6, changes DCDC4 to
960 mV through the existing PMIC owner and registers a domain-5 row. The
[power-domain owner](../../drivers/power/a733_power_domains.c) already traverses
multiple rows, and its existing rail/register locks serialize shared accesses.
CPU grants retain their separate CPU PLL and rail owners. No consumer writes
another consumer's supply or domain directly.

Register serialization does not serialize whole PLL transitions or establish
analog independence. [The NPU PLL audit](../npu-pll.md#enabling-the-pll-while-dram-runs)
retains the manual's cross-PLL disturbance warning. Combined supervised GPU
admission adds one condition reference covering the simultaneous powered
lifetime: NPU PLL enable while GPU/CPU/DDR clocks can be active, and GPU PLL
start/stop while the NPU PLL remains enabled. It binds the same image, board,
epoch, generation and lease as the existing NPU clock authority. Single-device
records cannot silently satisfy this condition.

There is no source-backed need to make normal NPU availability wait for GPU
OPEN. Startup and queues remain independent. A short NPU job may finish before
GPU recognition starts; that is valid application behavior and provides no
claim of simultaneous hardware execution. Software overlap is checked through
shared-provider fixtures. A later controlled workload can observe device overlap.
A startup rendezvous solely for that observation is outside production scope.

## Memory and scheduling budgets

The executable block remains `0x41000000..0x41200000`. ShuffleNet's 964,224-byte
model, 150,528-byte input and two-byte reference use initialized XN RAM from
`0x41200000`; they remain const and 64-byte aligned. Their CPU mapping is
writable, so immutability relies on trusted ownership. Authentication follows
the actual linked pointers. BSS, heap and the 2 MiB write-back device pool must
end before the unchanged noncacheable pool at `0x41c00000..0x41e00000`.

The GPU retains Mimus models at `0x44000000` and `0x44800000`, recording at
`0x45000000`, arena at `0x46000000..0x46c00000` and the CPU read-only output/view
at `0x46c00000..0x47000000`. The NPU's separate DMA32 pool remains
`0x47400000..0x47c00000`. Neither device acquires the other's backing.

The accepted GPU+CPU two-worker ELF has 1,055,712 executable bytes and 193,124
read-only bytes. Its write-back pool ends at `0x41993000`, leaving 2,543,616
bytes before the noncacheable pool. Allocated sections from the 19 NPU-only
objects add 80,944 executable, 1,988 CODE read-only, 1,114,816 initialized-RAM
and 140,490 BSS bytes. Six additional worker stacks and slots add 398,976 bytes.
The planning estimate was 1,331,768 CODE bytes and 889,334 bytes of RAM margin
before alignment and changes in shared objects. The actual combined image has
1,138,848 bytes of `.text` and 194,057 bytes of `.rodata`. Its write-back pool
ends at `0x41b27000`, leaving 888,832 bytes before the noncacheable pool. Both
fixed boundaries remain enforced by the linker and image checker.

Compared with the accepted eight-worker GPU+CPU image, allocated executable
sections grow by 76,800 bytes, read-only sections by 1,888 bytes and writable
sections by 1,254,144 bytes. The writable total includes pools, stacks, heap
and padding. These costs add the existing NPU application, providers and
1,114,754 payload bytes; `cost.json` records each linked object contribution.
The combined guards run at admission and before GPU provider construction.
Host preparation and frozen validation authenticate the linked NPU payload
once per validation. Runtime service requests gain no hashing or copying.

Recognition remains owner core 6 and worker core 7 at priority 3, with six A55
workers at priority 2. GPU service priority 4 and IRQ95 remain on core 6.
NPU bootstrap/service priority 4, client priority 3 and IRQ97 remain on core 1.
Both device IRQ priorities are `0xd8`; thermal IRQ71 remains on core 0. The
existing exact-sum hook submits only from its owner and joins CPU readout
workers before another GPU result reuses output. Both service queues retain
their own caller, ticket and completion ownership. The combined admission adds
no per-job hashes, translations, copies, tasks or queue round trips.

## Thermal, failure and validation

The existing thermal owner remains the sole sampler and CPU clock owner.
Fresh NORMAL readings admit NPU work; hot/stale readings request its terminal
local stop within five seconds. The GPU recognizer closes a healthy session
between completed calls at THROTTLE and reopens its retained backing at NORMAL.
CRITICAL or either uncertain accepted-device failure ends the board lifetime.
An unaccepted GPU refusal preserves its existing CPU fallback. NPU completion
retains its local stopped resources; GPU CLOSE does not release those leases.
The NPU's immutable report, live atomic stopped proof and existing finite
wait/deadline checks remain intact.

One combined eight-worker native build passed actual linked layout, IRQ,
startup-list and payload checks. Fourteen host checks cover complete
preparation, frozen entry, owner aliases, combined authority and payload
tampering. Two further checks admit CPU TOP grants through preparation,
frozen entry and native admission. Four checks execute the linked GPU startup
guard, refusing aliases before provider construction and accepting distinct
owners. One current shared-provider check observes both domain rows pending
together, NPU local stop with retained tokens, and GPU shutdown in vendor
order without disturbing NPU ownership. It passes with seven service steps,
145 PPU reads, 13 PPU writes and 180 PMIC bus writes in the model. Historical
failure cases retain their original source identities; the current case covers
subsequent GPU gate/PPU and rail-verification changes. Existing GPU arithmetic,
NPU inference, thermal and CPU-readout evidence is reused. No inference or
hardware workload was replayed.

Evidence is in `~/.cache/sunstoneos/evidence/concurrent-gpu-npu-1026`. The
combined ELF SHA-256 is
`b4d130c6b100aee263ba7e60218d86d6e89d7dd32898814a4bc0d4d98c96370c`.
The complete compiler-input closure, generated payloads, models, tools,
producer bundles and results are frozen with the existing snapshot tool.
Nominal fixture records assert physical fields solely to exercise admission;
they are labeled nondeployable and supply no board authority. An initial
fixture failed on a malformed DWARF layout key before executing a case. Its
log is retained; correcting that checker lookup required no image rebuild.

Resident GPU-plus-NPU card operation remains a separate admission gap. Current
card preparation rejects GPU, and supervised manual power-removal authority
cannot qualify cold GPU firmware retirement, retained faults or remote-writer
exclusion after watchdog reset. The resident NPU cold condition also needs a
record explicitly covering the active GPU. This design adds no guessed reset,
OFF sequence or successful ownership release.
