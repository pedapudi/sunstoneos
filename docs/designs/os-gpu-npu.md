# Concurrent GPU and NPU services in the OS

The first combined profile is supervised recorded recognition with eight CPU
workers, A76 admission, CPU clock grants, GPU linear dispatch and the selected
ShuffleNetV2 NPU application. Performance commands remain off. The GPU uses
its existing 1,008 MHz / 960 mV point; the NPU uses 852 MHz / 800 mV on the
supported bins. The finite NPU application remains independent of Mimus.
This design is based on source revision `6a945b30`; implementation and combined
physical execution are not yet qualified.

The [GPU eight-worker work](gpu-eight-workers.md) owns worker admission and
readout. The combined-device change owns optional-device guards, native and
host owner admission, asset placement and the complete session. The shared
files are split by those branches and integrated sequentially. Existing
[accelerator composition](../accelerator-composition.md) establishes the
provider-sharing pattern; it is a different application and memory layout.

## Concrete conflicts and changes

| Current restriction or gap | Scoped change |
| --- | --- |
| CMake and the link checker reject GPU plus NPU | Admit the combined supervised recorded profile; preserve unrelated device exclusions |
| Supervised NPU plus CPU clocks requires PERF, while GPU rejects PERF | Permit ordinary CPU startup grants for the combined profile without adding performance commands |
| GPU host admission assumes an 808- or 904-byte session | Derive 1,080 bytes with NPU and 1,176 with NPU plus the existing trailing CPU grant |
| NPU admission does not reserve GPU identities | Reject NPU owner aliases with GPU firmware owner `0x4f53475055464d31` or session owner `0x4f534731`, and reject NPU PCK client 7 |
| Supervised NPU payloads consume CODE | Use `.data.os_npu_assets` for all newly generated OS NPU payloads |
| Actual NPU pointer/hash checking exists only in card preparation | Share that helper with supervised preparation and frozen validation |

The identity checks run in [native session admission](../../apps/os/admission.c)
before provider construction and in [host admission](../../apps/os/prepare.py).
The existing GPU pre-provider guard also checks the NPU identity. Move the GPU
constants into its existing `dispatch.h` interface to avoid divergent values.
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

Keep the executable block `0x41000000..0x41200000`. ShuffleNet's 964,224-byte
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
This predicts 1,331,768 CODE bytes and 889,334 bytes of RAM margin before
alignment and changes in shared objects. These are planning estimates from
accepted objects, not a combined link result. A final link must enforce both
fixed boundaries; no executable-map or pool expansion is proposed.

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

Implementation validation is one combined eight-worker native build, actual
linked layout/IRQ/start-list/payload checks, complete producer and frozen host
admission, and focused native alias refusals. Shared-provider cases cover both
rows active, independent healthy stop, shared-owner failure and retained
engine failure without reclaiming the other engine's leases. Existing GPU
arithmetic, NPU inference, thermal and CPU-readout evidence is reused. No old
inference run or hardware access is part of this implementation.

Resident GPU-plus-NPU card operation remains a separate admission gap. Current
card preparation rejects GPU, and supervised manual power-removal authority
cannot qualify cold GPU firmware retirement, retained faults or remote-writer
exclusion after watchdog reset. The resident NPU cold condition also needs a
record explicitly covering the active GPU. This design adds no guessed reset,
OFF sequence or successful ownership release.
