# Native GPU firmware memory operations

The [A733 memory adapter](../platform/a733/gpu_memory.c) is **implemented** and
passes a two-core FreeRTOS emulator check. It connects the GPU lifecycle service
to native CPU address translation, cache maintenance, shared-memory accesses,
and barriers. Physical cache/interconnect behavior remains unqualified.

The [interface](../include/a733_gpu_memory.h) requires reserved code, data, and
firmware page-table buffers plus the nineteen firmware resources. Initialization
copies their descriptors and verifies CPU/physical correspondence, read/write
permissions, alignment, physical page uniqueness, and CPU overlap restrictions.
It does not allocate memory or access GPU registers. The platform must reserve
the RAM and prevent other users or aliases; translation alone cannot prove those
conditions. Firmware and resource builders retain responsibility for image and
ABI validation.

## Shared records remain uncached throughout the firmware lifetime

Code, private firmware data, and page tables use Normal write-back,
inner-shareable CPU mappings with attribute byte `0xff`. All nineteen shared
resources use Normal Non-cacheable mappings with byte `0x44` and inner or outer
shareability. Arm defines each `0x4` half-byte as non-cacheable for the
corresponding inner or outer memory attribute in its
[architecture reference](https://documentation-service.arm.com/static/62c410c8b334256d9ea8dffb).

Shared resources contain fields owned by the CPU and firmware within the same
cache line. Cleaning a dirty CPU line could overwrite a firmware update;
invalidating it could discard an unpublished CPU update. Uncached mappings
avoid that ownership conflict. No cacheable CPU alias may exist for those pages.
This policy applies to firmware control resources; application workload buffers
remain separate work.

The adapter verifies the affected pages before each shared access. Four-byte
queue accesses use a single aligned word load or store. Other shared reads use
volatile byte loads. Full-system barriers order publication, completion, and
acquisition. The callbacks cover queues, the firmware-started byte, compatibility
records, and the power synchronization word.

Offline queue formatting may write every queue region. After publication, writes
are restricted to the host producer cursor, complete host commands, zeroing a
return slot, and updating the event consumer cursor. These checks do not admit
application commands or validate their payload semantics.

## Publication and reclamation bracket device ownership

Before publication, the service builds code/data, firmware mappings, initialization
records, and queues. Publication rechecks every mapping, cleans and invalidates
the cached firmware buffers to the point of coherency, and enters the published
state. The CPU must not access those cached buffers while the device owns them.
Shared resources remain accessible through their uncached callbacks.

After independent platform isolation blocks new GPU traffic and drains outstanding
transactions, the platform may call reclamation. It rechecks mappings, invalidates
the cached buffers, and returns to offline preparation. Reclamation does not
establish device isolation itself. The production isolation implementation and
its physical qualification remain outstanding.

An invalid callback, changed mapping, or cache failure retains the buffers and
latches failure. Later callbacks refuse before touching backing memory or issuing
translation/cache operations. A failed adapter cannot be reclaimed through this
API. The lifecycle service also retains its owner and stops backend callbacks
when the adapter reports failure.

## Focused emulator evidence

The native variant of the [service fixture](../checks/pvr-gpu-service.c) uses the
actual adapter, AArch64 translation/cache instructions, firmware builders, and
FreeRTOS lifecycle service. A separate generated platform reserves uncached RAM
before either emulator CPU enables its MMU. Register and firmware responses
remain a software model; GPU firmware instructions do not execute.

Two selected cases passed:

- Preparation, publication, compatibility, health, event handling, shutdown,
  reclamation, reopen, and ownership transfer across the privileged worker's
  migration between cores. A later startup expiry retains published resources.
- A deliberate change of shared pages to read-only permissions causes the next
  native queue access to fail. The service retains its owner and stops callbacks;
  all allocation guards remain intact.

Initialization also refuses cached shared resources, a mismatched physical page,
read-only uncached memory, and absent mappings without changing the adapter.
The fixture retains guard pages around every allocation. QEMU evidence does not
establish physical cache coherence, interconnect visibility, GPU bus transactions,
or board recovery. No performance measurements or physical device requests ran.
Existing component checks and unrelated service failure cases were reused.

Evidence is under `~/.cache/sunstoneos/evidence/gpu-memory-20261016/`, including
input snapshots, commands, logs, images, and stack reports. The executable hash is
`8a742c9667643d1304595f9829af3aecffb066c219994fe846df9c776983b3df`.
The build contains 207,272 code/constant bytes, including embedded firmware,
four initialized data bytes, and 684,064 bytes of BSS/reserved stacks. The
adapter's initialization frame is 832 bytes; other individual frames are at
most 96 bytes. Called functions add their own frames.

After [optional interrupt delivery](gpu-interrupts.md) changed the service,
one native-memory lifecycle case passed with interrupt callbacks disabled.
This checks the integrated service layout and polling fallback. Its evidence
is in `gpu-memory-20261016/irq-service-integration/`; the mapping-failure case
and the interrupt component's own cases retain their earlier evidence.

After relevant inputs change, configure the pinned firmware as described in
[the service record](gpu-service.md), then run:

```sh
cmake -S . -B ~/.cache/sunstoneos/build/qemu-virt
cmake --build ~/.cache/sunstoneos/build/qemu-virt --target gpu-memory-model -j 8
python3 checks/pvr-gpu-service.py \
  --elf ~/.cache/sunstoneos/build/qemu-virt/gpu-memory-model.elf \
  --qemu ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  --nm /usr/lib/llvm-21/bin/llvm-nm \
  --output ~/.cache/sunstoneos/evidence/gpu-memory-local --cases 1 4
```

Next, connect the reserved mappings to the board allocator and combine this
adapter with power/reset control, physical interrupts, event-specific handlers,
and qualified isolation. Independent watchdog recovery remains the prerequisite
for new physical peripheral accesses.
