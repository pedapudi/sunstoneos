# Design: one OS image, with services chosen at run time

Status: the foundation (granted set, session records, fixed service ids, refusal table, ungranted-service checks) is **implemented and host checked**. The CPU series (series 2: CPU clock grant, A76 placement and performance modes as run-time grants) and the service series (series 3: I2S capture and the radio in every image, the headset as a library linked by manifest) and the NPU series (series 4: the service, its EL0 ring and its observer in every image) are **implemented**; the lifetime series is **proposed**. The sizes in "Size" are measured on the all-in link and, for the headset library, on the headset fixture image.

Readers are assumed to know [os-image.md](../os-image.md), [app-abi.md](app-abi.md) (manifest, session extents, device bitmap) and [application-memory-layout.md](application-memory-layout.md).

## Problem

The OS is built from compile-time options. The integration gate builds 16 OS variants from eight `SUNSTONE_A733_OS_*` options, `A733_A76_ADMITTED` and the NPU catalogue and model variables (`VARIANTS` in `tools/integration-gate.py`). The options appear in 151 lines of CMake, and the C, header, Python and JSON files that depend on them number more than 70. No shipped configuration needs more than one OS. Each option multiplies builds and checks, and each pairing that the options forbid is a rule the build must enforce and a combination no check covers.

## Decision

One OS image contains every service. Three inputs decide at run time which services start:

1. **The application manifest** declares which services the application uses: device bits (`devices_required`, `devices_optional`), the `npu` section, and a new `cpu` clock declaration.
2. **The board description** states what the board has and allows: radio module present, I2S microphone wired, A76 cores present, the speed bin. The A733 values come from the fixed board topology, the SID words and the MIDR; they are compiled-in tables of the board port, not build options.
3. **The sealed session** carries the facts and approvals of each granted service as a typed extent (type, length, payload). The extent mechanism already exists: thermal (1), audio (2), NPU (3), CPU grant (4), JPEG (5), USB memory (6), DMA (7).

EL3 admission computes the **granted set** once: a service is granted when the manifest declares it, the board description allows it, and the session carries a valid extent for it. The set is sealed in EL3 monitor memory. Every later decision reads the set.

## The rules that carry over

### Hardware is touched only when requested

The compile flag guaranteed that an unqualified driver contained no code that could run. The run-time form has to guarantee that no register of an ungranted service is accessed, even though its code is in the image. Four layers provide that:

1. **Start entries return first.** Every service start entry (`os_radio_start`, `os_audio_*`, `os_headset_start`, `os_npu_start`, `os_cpu_clock_step`, the perf owner) begins with `os_granted(SERVICE)` and returns success without creating a task, unmasking an interrupt or reading a register when the service is not granted. The test is a read of the sealed set; it costs one load at start and nothing afterwards.
2. **Task creation and interrupt enabling follow the grant.** The generated tables list every service. The interrupt route table (`a733_irq_routes`) only sets the group, priority and target core of an INTID in the GIC distributor; the enable of the INTID is in the granted service's own start entry, so an ungranted INTID (97 NPU, 143 I2S and DMA0, 95 GPU) is routed but never enabled. INTID 143 is shared by I2S and DMA0; the refusal table below grants at most one of them.
3. **Approvals stay in the session.** Writes that need a coordinator approval stay behind the record that already authenticates them: the CPU grant record (`a733_cpu_clock_grant_load`: board epoch and generation, change count, approval hash binding the image, board, session and step), the radio profile digest and first-run BLDO5 guard, the NPU `command_admission` and thermal ALLOW. A service with no valid extent cannot be granted, so a session without a CPU grant cannot write DCDC3, DCDC5 or a CPU PLL, and a session without a radio profile cannot power the module. The record check moves nowhere: it still runs at EL3 admission (`os_session_valid`), now keyed by the granted set instead of by `#ifdef`.
4. **Host checks prove it.** For each service a check runs the real start entries against a session that does not grant it, and every call into the service stops the program (an unresolved-call trap in `checks/os-npu-service.py` case 32, a counted wrapper of the stream open in `checks/os-image-audio.py`, an unreadable profile and zero authorized pins in `checks/os-image-radio.py`). A mutation of the NPU guard fails case 32. The compile flag could not be tested this way; the run-time gate is.

One gap is closed by this work. Today the A76 cores are released by the cached SMP boot loop for every image (`platform/a733-smp-cached/boot.c`), so "A76 admitted" is a placement permission, not a power-on. The run-time form keeps that: the permission is the board description plus the manifest's `cluster`, and a manifest that asks for the A76 cluster on a board or session that does not allow it is refused at admission. (Today it is silently placed on core 2.)

### No artificial caps

Any table that varied between variants is sized from the declaration at registration, not fixed at a maximum:

- **Session.** `struct os_session` loses its conditional members (`npu`, `audio`, `cpu_clock`, `dma`) and the `OS_SESSION_BYTES` arithmetic. It keeps the thermal core. Each service's facts are read from its extent by `os_session_extent`. The session page limit is 4,096 bytes (the sealed copy in `apps/board/session.c`); the largest combination today, NPU with CPU grant and DMA, is well below it.
- **Service table.** `OS_SERVICE_*` ids become a fixed enum that includes every service (today the ids shift with the options). `services_state` already keeps a `required` mask; it extends to the granted set.
- **NPU.** Slots, graphs, links, graph bytes and tensor bytes already come from the declaration (`npu-declaration.c`). The remaining fixed pieces are removed: the model table (see NPU), the EL1 metadata arena (`os-npu-meta.h`, being deleted by `work/umem-dynamic-pool-1103`; this work builds on it once it lands and does not touch pool sizing, `SUNSTONE_OS_NPU_POOL_BASE` or `SUNSTONE_OS_NPU_POOL_BYTES`), and the queue depth. The depth sized `entry[]` in `struct ss_npu_queue`, an ABI shared with EL0 (`SS_NPU_QUEUE_MAX_DEPTH`, default 64). The ring's entries are now carved with its tables at admission, `in_flight` of them, and the constant is gone.
- **Audio.** The capture buffer already follows the manifest (`64 + max(8192, ceil(max_frames*channels*bytes/4096)*4096)`). The 119,680 bytes of stream state (period ring of 64 entries, 4-page DMA pool, 8 KiB stack) come from the unified ledger at the first open of the stream (implemented); sizing them from the period and frame counts in the audio extent is open.
- **Headset.** `.bss.voice` (440,448 bytes), the headset application state (353,888) and the 96,044-byte WAV buffer are sized from the Bluetooth profile at the grant.
- **Placement.** `A733_SMP_APPLICATION_CORE` and `_WORKER_CORE` become run-time values read from the grant; the `_Static_assert`s in `apps/os/start.c` become admission checks.

## Inventory by option

Counts are tracked files that mention the option, including docs.

### `SUNSTONE_A733_OS_RESIDENT` (lifetime)

- **Compiles in or out:** `SUNSTONE_BOARD_RESIDENT`: the contract magic (`A733_OS_RESIDENT_CONTRACT_MAGIC` against the supervised one) and the board timing validator in `apps/board/admission.c` (resident: no deadline, expiry at armed time plus 4 s, watchdog fields set; supervised: 60 s, zero watchdog fields); the `supervise()` renewal task and `apps/os/watchdog.c`; the manifest lifetime and return-reserve checks (`session.c:161`) and `grant.finish_by_us` (`application.c:409`); the USB0 product id; the audio DMA established flag (0x3f against 0x5f); the NPU end-of-work cap; `radio_profile.c` timing (120 s and 130 s offsets against 60 s); extra held-policy steps in `platform/a733/cpu_start.c`; `boot-resident.patch` (drops one EL3 readback). The stage-2 loader builds only the resident contract.
- **Why an option:** commit 5be126a5 says every board image ended at a fixed deadline and `SUNSTONE_BOARD_RESIDENT` admitted a contract with its own magic that arms the four-second watchdog. `os-image-history.md` keeps the supervised lifetime "for the first physical runs". It is risk containment, not a size or ISA reason.
- **Run-time form:** the lifetime is already encoded in the contract the loader or host writes: the magic. EL3 admission accepts the set of magics, and selects the timing validator and the watchdog expectation from the verified magic before any readback. Application, session decode, NPU, audio and radio code read the admitted lifetime from a sealed field, not a macro. The choice never comes from session or application bytes: a wrong choice would leave a watchdog armed with no renewer.
- **Stays a build option:** the stage-2 loader (a separate program, 32,608 bytes against a fixed limit). `boot-resident.patch` is the only EL3 boot difference and folds into a run-time branch on the magic. The resident lifetime adds no stop-path branch; the lifetime word it introduces is described in "Lifetime word and sealed session".

### `SUNSTONE_A733_OS_CPU_RETURN` (lifetime, not among the 16 variants)

- **Compiles in or out:** 12 patch files (about 800 lines) over 11 units, including the EL3 stop handler (WDT0 shortened to 0.5 s and restarted so the board returns to FEL), the vector and terminal code and the boot; `cpu-return*.c`, `cpu-return-el0.S`; the GPU park and retire path; the NPU retirement wait in `ss_finish`.
- **Why an option:** commit 6976bc48 keeps other lifetimes unchanged while it is qualified.
- **Run-time form:** a third contract magic, handled like resident, built after it. The EL1 patches (main, start, CPU, thermal, admission, `continue.c`, runtime, the interrupt policy in `platform.patch`), the GPU park and retire path and the NPU retirement wait become branches on the sealed lifetime. The returning-only code (`cpu-return*.c`, `cpu-return-el0.S` and the terminal unit) is linked into every image and is reached only through SVC 0x735 or the success dispatch. Its admission rule (CPU clock grant, workload 2, or 3 in the scanning image, clusters 3, at least four changes, no performance mode) becomes a refusal-table entry keyed by the lifetime.
- **EL3 changes:** five places branch on the EL3 lifetime word:
  - EL3 admission's watchdog check takes three arms: supervised requires WDT0 disarmed, resident requires mode 0x41, returning requires mode 0xb1, configuration 1 and output 2.
  - `a733_smp_stop` reads the WDT0 mode for every lifetime. Its crumb store and the WDT0 shortening (0x16aa0001 to 0x02050014, then a restart) run only when the word is returning.
  - The EL3 fatal vector in `continue.S` loads the word once after the record store. For a returning lifetime a status-0 SMC branches to `a733_qualification_success_dispatch`, and the RTC fault words at 0x07090110 to 0x0709011c are stored. Supervised and resident continue to storage adoption and `a733_smp_stop`.
  - `a733_qualification_boot` and the SPI save run only for a returning lifetime.
- **Stop-path rule:** a change to the EL3 stop handler or the fatal vector is allowed when a host check proves it trace-identical for the other lifetimes. The check runs `a733_smp_stop` for every status, and the fatal vector for a status-0 SMC and for an exception, under the supervised and resident lifetimes. It compares the folded build with a reference built from the tree before the change. It requires identical MMIO write traces and identical final records, and that the stop paths load the EL3 lifetime word and never the EL1 copy. Three mutations must fail it: ungating one crumb store, dropping the returning success-dispatch branch, and pointing the stop handler at the EL1 copy. Supervised and resident stops gain one load and branches that are not taken, and no MMIO store.

### Lifetime word and sealed session

**Status: proposed.** The RESIDENT series introduces both pieces.

- **EL3 lifetime word:** EL3 admission writes it once, after it verifies the contract magic, in `.bss.a733_smp_monitor` inside the monitor interval at 0x42000000. EL1 has no mapping of that interval (level-2 slot 16 is invalid in the EL1 tables). The stop handler, the fatal vector, the watchdog check and the returning boot steps read only this word.
- **EL1 copy:** the application, the session decode and the services read the lifetime from a copy in the sealed session section. EL3 writes the copy together with the session; EL1 and EL0 never write it, and EL3 never reads it back.
- **Placement:** the `sealed` object in `apps/board/session.c` holds the 4,096-byte session copy that `board_session_sealed()` serves, its flags and the lifetime copy. It moves from `.bss` into a page-aligned NOLOAD output section, `.sealed`, placed last in the link script's CODE region (0x41000000 to 0x41200000). An `ASSERT` in the link script fails the link when `.sealed` does not end inside CODE. The text segment is 764 to 954 KB in the measured images, so the section fits. The CODE block is the 2 MiB block that `a733_cpu_coherence_tables` maps with AP = 0b11, read-only at EL1 and EL0. It is executable at EL1, and the section stays executable: splitting the block to give one page execute-never would cost TLB reach on the text, and the bytes are never writable at EL1 or EL0.
- **Write path:** EL3 writes the section in `board_session_prepare`, before `a733_cached_prepare` builds the tables, with translation and caches off. Those stores are Device-nGnRnE and allocate no cache line. A NOLOAD section is not zeroed by any loader, and DRAM keeps the previous boot's bytes across a watchdog reset, so EL3 zeroes the section before the copy and writes the `ready` flag last. The sequence:
  1. EL3 runs `a733_cpu_coherence_flush` (DC CIVAC, clean and invalidate to the point of coherency) over the section, so no Secure line of it can later be written back over the new bytes.
  2. EL3 zeroes the section, copies and checks the session, writes the lifetime copy, then `ready`.
  3. EL3 runs `a733_cpu_coherence_flush` over the section again. `a733_cpu_coherence_publish` is not enough here: it issues DC CVAC, which cleans and leaves a line valid.
  4. Each core, at EL1 entry with translation off, flushes the Nonsecure alias of the section with DC CIVAC, in the same step that flushes the EL1 table pages ([CPU coherence](../cpu-coherence.md)). The first cached read then misses to DRAM.
- **Stale lines:** a second session in the same boot cannot exist: `board_session_prepare` refuses a second attempt, and EL3 admission refuses a result record that already carries the contract's generation. A line from the previous boot is excluded by reset-state cache invalidity, the condition [CPU coherence](../cpu-coherence.md) already requires for all image memory. The flushes in steps 1, 3 and 4 remove a clean stale line. A dirty line surviving reset would break that condition for every page of the image, not only this section. The rules relied on are architectural: with SCTLR_ELx.M = 0, data accesses are Device-nGnRnE, and DC CIVAC by address reaches the point of coherency. No copy of the Arm Architecture Reference Manual or of the Cortex-A55 and Cortex-A76 technical reference manuals is pinned in the repository.
- **Cache-order check:** the instruction models have no cache, so they check the order of operations: every 64-byte line of the section receives DC CIVAC at EL3 before the first store and after the last store, each core issues DC CIVAC over the Nonsecure alias before it enables translation, and no store reaches the section at EL1 or EL0. Three mutations must fail it: removing the first flush, replacing the second flush with the DC CVAC publication, and removing the EL1 flush.
- **Page-table check:** it walks the EL1 regime's tables as admission leaves them and requires every descriptor whose output range covers the section to be invalid or to have AP[2] set (read-only). The walk covers:
  - the level-1 table and every level-2 block from 0x40000000 to 0x48000000, including slot 14 (0x41c00000), whose attributes admission rewrites, and the GPU read-only output and GPU-written blocks;
  - the level-3 tables of the USB memory alias (`a733_cached_usb_el1_l3`) in images with the generic USB host;
  - the DRAM blocks above the boot image (`dram_map`), at level 1 and level 2;
  - every application address space that `apps/os/large.c` builds from `os_kernel_l1_head` and `os_kernel_l2_slot1`, and the ranges the unified allocator maps into it.
  
  Two mutations must fail it: linking `sealed` back into `.bss`, and clearing AP[2] on the CODE block. Device MMU mappings (GPU, NPU, IOMMU) are outside this check.
- **QEMU:** `platform/qemu-os` seals at EL1 under its own link script and keeps `sealed` in `.bss`. It is a host model and enforces no protection.

### I2S capture (formerly `SUNSTONE_A733_OS_AUDIO`)

- **Status: implemented.** Every OS image links `audio.c`, the DMA service and the I2S and audio-lease drivers.
- **Grant:** the session grants the service when `devices` has `SS_DEVICE_AUDIO_I2S` and the audio record validates. When it is not granted, `os_audio_pins`, the DMA0 open and the interrupt enable do nothing.
- **Memory:** the stream's state (119,680 bytes: the period ring, the DMA pool session and the task stack) comes from the unified ledger at the first open of the stream, so an image without a capture session holds only a pointer.
- **Refusals:** admission refuses the grant with the DMA service, which shares INTID 143, with the NPU, in a returning image, in an image with the USB camera decoder, in a supervised GPU image and with resident CPU grants.

### Wi-Fi station (formerly `SUNSTONE_A733_OS_RADIO`)

- **Status: implemented.** Every OS image links `radio.c`, `wifi_credential.c` and the progress reports of `main.patch` for `OS_SERVICE_RADIO` and `OS_SERVICE_NETWORK`; a resident image also links `radio_profile.c`. The board's `main.c` holds `.bss.radio` (354,832 bytes), so it is in every image.
- **Grant:** the device bit `SS_DEVICE_RADIO` (bit 8; `SS_DEVICE_ALL` is `(1u<<9)-1`). The radio has no session record, because the sealed board prefix already carries the profile's address, size and digest. The decode requires the board prefix's workload to agree with the bit: workload 1, the full station, with USB storage, or, in the scanning image, workload 3 without it. A radio workload without the bit is refused.
- **Fixed addresses:** radio profile at 0x43000000 (1,872 bytes), credential record at 0x43004000 (256 bytes), firmware span from 0x43010000 (0xf0000 bytes).
- **Power-on:** unchanged. The module is powered only after the profile digest matches, the `platform_authority` and owner token check passes and the BLDO5 first-run guard holds.
- **Credential:** a session that grants the radio wipes the credential record at its end and on a failure; any other session neither waits for the record nor writes it. The runner refuses a credential for an application that does not declare the radio device and for a session that does not grant it (`tools/board.py`, `tools/board_images.py`).
- **Scanning image:** `SUNSTONE_A733_OS_RADIO_SCAN` stays a build option, because it depends on the returning lifetime: it adds the scan workload, the module shutdown with readbacks and the scan result.
- **Refusals:** admission refuses the radio with the NPU, in a returning image other than the scanning one, in an image with the USB camera decoder, in a supervised GPU image and with resident CPU grants.

### Headset and mSBC (formerly `SUNSTONE_A733_OS_HEADSET` and `SUNSTONE_A733_OS_MSBC`)

- **Status: implemented.** The headset service, the Bluetooth stack, its UART driver and the mSBC codec form one object library, `sunstone-a733-os-headset`. `cmake/sunstone-image.cmake` links it into an image whose application declares the `audio-headset` device, required or optional. Only such an image compiles the external sources, so the library needs `SUNSTONE_BT_SECURITY_DIR` and `SUNSTONE_BT_SBC_DIR` only there. The build directory keeps the libsbc license (LGPL-2.1-or-later) and its source record in `os-image/bluetooth/msbc-license/`.
- **Core interface:** the OS core reaches the library through weak references, so the core objects are the same with and without it. The session decode grants the headset (grant bit 5) when the session declares the device, and refuses the device in an image without the library. The EL3 identity hook reads the sealed session's devices: for a headset session the library copies the 832-byte Bluetooth profile from 0x43008000 and publishes it with the 1 MiB profile span; an image without the library refuses that session. Admission requires SID evidence for the grant.
- **Codec:** mSBC is always linked, so the profile's mode chooses CVSD (2) or mSBC (3) at run time.
- **Module owner:** the radio and the headset share one AIC8800 module. When the session grants the radio, the headset attaches to the radio owner's module; otherwise it runs its own Bluetooth owner task.
- **Refusals:** admission refuses the headset with the NPU, in a returning image, in an image with the USB camera decoder and in a supervised GPU image.
- **Fixture:** the `headset-fixture` application declares USB storage and the headset; the gate's `headset` and `resheadset` variants build it.

### `SUNSTONE_A733_OS_NPU`, `_NPU_QUEUE`, `_NPU_OBSERVE`

- **Compiles in or out:** the NPU runtime, service and client (`apps/os/npu*.c`, `drivers/npu/*`, `runtime/npu_app.c`): +102 KB `.text`, +2.3 KB `.data`, +233 KB `.bss` at the same commit (728,212 against 626,260 bytes of text with no NPU, on the umem-core build). The 296-byte `os_npu_session`; service `OS_SERVICE_NPU`; route `{97,OS_NPU_CORE,0xd8,os_npu_irq}`; the pool link script. The queue adds the EL0 ring (`npu-queue.c`, `runtime/npu_app_queue.c`) and forces the main core to 1 and refuses the A76 cluster. OBSERVE adds diagnostic sampling and needs CPU_RETURN.
- **Why an option:** commits 70403fba ("an optional supervised OS profile … work in progress"), a600b85d (queue), defb4947 (observe). `docs/designs/npu-resources.md` already says a graph named in a build list "is an optional convenience".
- **Hardware first touched:** `os_npu_start` binds the interrupt and tasks only; the first clock, reset, power-domain and SRAM-route writes come from the bootstrap task, and only after the thermal gate is ALLOW and the session extent validated.
- **Run-time form:** the manifest's `npu` section is the declaration. The extent keeps owner, `command_admission`, supervisor, `pck_client` and the evidence digests, but loses `catalogue` and `client_model`. The core-1 rule of the queue becomes an admission rule (the queue image refuses the A76 cluster), not a macro. OBSERVE is a diagnostic: it moves to the manifest `observe` flag in the same extent; it stays refused outside a returning lifetime.

*Status: implemented. Every image links the service, the ring and the observer.*

- *Grant.* The session grants the NPU (`os_granted(OS_GRANT_NPU)`) only to an application whose manifest has an `npu` section; the image records it as `ss_application_npu`, which the host tools read. Every NPU entry point returns first when the NPU is not granted.
- *Ring.* A declaration of more than one job in flight selects the ring (`os_npu_session_queued`); the admission then puts the main task on core 1 and refuses the A76 cluster. The ring's entries are carved with its tables, `in_flight` of them; the application reads the depth as `<NAME>_NPU_IN_FLIGHT` from its generated header.
- *Observer.* `"observe": true` in the `npu` section sets `OS_NPU_OBSERVE` in the extent's flags word, which named the client model before (always zero, so the record keeps version 4). The image build and the admission refuse it outside the returning lifetime, and `ss_npu_observe` refuses an application that does not declare it. Each observation site is one load and one branch on the service's observer pointer, set once before the scheduler starts: on the image's compile flags, 2 instructions per register write, 2 each in the job start and the settle, at most 2 per worker iteration and 3 in the synchronous interrupt path, about 13 instructions for a synchronous job with its two register writes. The ring's submissions and its direct interrupt path pay none.
- *Size.* The supervised image with no application, before the series and after it: text 764,028 and 898,764 bytes, data 2,752 and 2,768, `.bss` 7,144,944 and 7,259,632, heap end 0x414b7d80 and 0x414d3a80. Of the 114,688 bytes of `.bss`, 102,848 are the `npu` state in `apps/os/npu.c`; taking it from the ledger at the grant is open work.

#### The model catalogue goes

*Status: the OS image links no graph. Stage 2 removed the model table, the metadata, the linked vectors, the diagnostic client, the generator and the build options (`SUNSTONE_OS_NPU_MODELS`, `_CATALOGUE`, `_CLIENT_MODEL`, `_ALL_MODELS`, `_QUEUE_MODELS`, `_MODEL_NAME`, `_EXTERNAL_MODELS`); applications register graphs with `ss_npu_register_graph`. Step 1 copies each registered graph once into kernel memory and removes both hashes. The seal does not run at the first queue open, as first designed: the runtime's prepare, bind and service-worker creation require the scheduler not to have started, and moving them would rewrite the qualified service start. Instead the manifest's `npu.graph_region` names the input region that holds the application's graphs behind an index; admission copies them and prepares before the scheduler with each slot sized by its own graph (the encoder's pool: 33,554,432 bytes against 123,731,968 for uniform bounds). A registration after admission is refused as `SS_NPU_REGISTER_SEALED`. See docs/designs/npu-resources.md. The session's client-model word is now its flags; only the NPU report still names `os_npu_client_model`, always zero.*

`SUNSTONE_OS_NPU_CATALOGUE`, `apps/os/npu-models.json`, `apps/os/npu-catalogue.py` and the generated `npu-models.c` exist to link a reviewed allowlist of graph hashes into the image. The policy is now to run graphs as compiled, confined by the NPU page tables (proven on the board), with no program allowlist. The OS runtime already admits by structure and address (`os_npu_register`, `ss_npu_register_graph`, external models by address). So:

- **The OS links no graph and no model list.** `os_npu_models`, `os_npu_model_info`, `os_npu_model_count`, `os_npu_client_entry`, `os_npu_client_model`, `os_npu_catalogue_digest`, `os_npu_inputs` and `os_npu_references` leave the image. `SUNSTONE_OS_NPU_CLIENT_MODEL`, `_QUEUE_MODELS`, `_MODEL_NAME`, `_ALL_MODELS`, `_EXTERNAL_MODELS` and `_CATALOGUE` go with them. The session's `catalogue` digest (compared in `admission.c`) is removed.
- **Applications supply graphs by address**, as application data in an INPUT or large region, or by file once the SDK file API exists. The runtime reads tensor counts and sizes from the graph header, which it already does for registered graphs, so the model-info metadata needs no separate table. The runtime table is grown at registration.
- **The reviewed compiler outputs, with their reference inputs and outputs, become fixtures** (`tools/npu-fixtures.json` renamed for what it is, plus the artifact map) for host and board checks. Fixtures are not part of any image build.
- **The diagnostic client.** `os_npu_client_task` and its reference-vector comparison are application behavior. `npu-graphs` and `cpu-npu-concurrency` are the applications that take it over. The first step of the NPU series checks that `npu-client.c` (the SDK back end) needs nothing from the linked vectors.

Checks and tools that depend on linked models, and where each gets its graph afterwards:

| Check or tool | Today | Graph afterwards |
| --- | --- | --- |
| `checks/os-npu-service.py` and `.c` | synthetic 400-entry catalogue cycling five graphs; `--artifacts` runs an image's catalogue | the test registers the five fixture graphs with `os_npu_register`, 400 times, from fixture bytes |
| `checks/os-npu-fault.py` | runs `npu-catalogue.py` to make `npu-models.c` | links the fixture graph as test data and registers it |
| `checks/os-npu-graphs.py` | every catalogued graph through LOAD and SUBMIT on the host | iterates the fixture list and registers each graph |
| `checks/npu-runtime-bounds.py` | bounds from the catalogue models | bounds from the fixture list |
| `checks/npu-session-runtime.c` | five catalogue models on two QEMU cores | fixture graphs passed by address |
| `checks/npu-source-output.py` | writes a fixture `npu-models.json` | fixture list directly |
| `checks/npu-pool-size.py` | invokes `npu-catalogue.py` | deleted by `work/umem-dynamic-pool-1103`; not touched here |
| `apps/npu-sdk/catalogue-check.py` and `.c` | data-selected catalogues against the decoder and the real SDK bind | fixtures against the decoder and the SDK bind |
| `apps/npu-graphs` (`cmake/application.cmake`, `vectors.py`, `queue-aliases.py`) | needs `ALL_MODELS` and `CATALOGUE` | graphs in INPUT regions of its manifest, loaded by the application |
| `apps/cpu-npu-concurrency` (`cmake/application.cmake`, `vectors.py`) | needs `MODEL_NAME=shufflenet-v2-published` and `NPU_OBSERVE` | the 964,224-byte graph in an INPUT region; observe from the manifest |
| `checks/cpu-npu-return-chain.py`, `checks/npu-app-queue.py`, `checks/npu-sdk-events.py`, `checks/npu-nocaps.py` | define `SUNSTONE_A733_OS_NPU=1` and the meta size | define nothing; run on the single image |
| `tools/host_admission.py` and `.c` | read `os_npu_client_model`, `os_npu_model_count`, `os_npu_catalogue_digest` from the ELF | read the declaration from the manifest and session |
| `tools/gate_session.py` | builds the NPU extent from image symbols | builds it from the manifest |
| `checks/integration-gate.py` (`npu` variant test), `checks/gate-inputs.json` (`ret-npu-load`) | pass the catalogue and model name | no catalogue arguments |

#### The older finite session driver

`drivers/npu/npu_session.c`, `apps/npu-board`, `include/npu_admitted_models.h`, `tools/npu-session-catalogue.py` and `tools/npu-fixtures.json` still use the allowlist. Their only image is the `SUNSTONE_A733_NPU_BOARD` composition (`cmake/a733-npu-board.cmake`), with the host tools `tools/check-a733-npu-board-supervised.py` and `checks/npu-board-capture.py`. The OS NPU runtime (`a733_npu_runtime.c`) is the successor and shares the NPU startup sequencer and IRQ file with it (`platform/a733/npu_irq.c` is included by both `apps/os/npu-irq.c` and `apps/npu-board/irq.c`).

I propose retiring it: delete `npu_session.c`, `apps/npu-board`, `npu_admitted_models.h`, the NPU board cmake and its two checks, after confirming that no evidence record needs to be re-run from the board image (the runs in `docs/evidence/npu-board.jsonl` stay as records). Open points I have not resolved: `apps/npu-convolution` and `tools/check-a733-npu-supervised.py` and `check-a733-accelerators-supervised.py` read `tools/npu-models.json`, a separate list, so they depend on the older catalogue tooling and not on the OS one; they need their own look. `apps/npu-finite` also reads it. Retirement is a separate series after the OS catalogue is out, and I will ask before deleting anything in that group.

### `A733_A76_ADMITTED`, `SUNSTONE_A733_OS_CPU_CLOCK`, `SUNSTONE_A733_OS_PERF`, `SUNSTONE_A733_OS_PERF_BOOT`

Implemented (series 2). The four options are removed: every OS image links the CPU clock owner, the performance-mode service and the A76 placement, and a session grants them at run time ([one-os-image-cpu.md](one-os-image-cpu.md)). The inventory below records what each option compiled in.

- **Compiles in or out:**
  - `A733_A76_ADMITTED`: `apps/os/cpu.c` and the CPU clock driver in read-only form (`os_cpu_admit`: CPU-B on its locked PLL at 1014 MHz, else the thermal start refuses); PLL page mappings in `thermal.c`; core 6 and 7 placement constants in `a733_smp_placement.h` (2 and 3 without); `grant.main_core` in `application.c`. It does not change the FreeRTOS port, the scheduler (the default affinity is the A55 mask in every build), the GIC, the MMU tables or core bring-up; cores 6 and 7 are released by the boot loop in every image.
  - `CPU_CLOCK`: the 96-byte grant record in the session, the owner state machine (`os_cpu_clock_step`) in the thermal task, USB0 operation 7, `ss_clock_*` and `ss_activity_*`. This is the code that writes DCDC3, DCDC5 and the CPU PLLs.
  - `PERF`: `perf.c`, `a733_perf_mode.c`, GPU and NPU hooks, USB0 operation 8, and the speed-bin admission. `PERF_BOOT` sets `OS_PERF_BOOT_MODE` (1 high, 2 low), which also changes the GPU start rate and the NPU boot profile.
- **Why options:** `cpu-a76-dvfs.md`: "Both options are off by default. Without the second option no image writes a CPU clock register or a CPU rail voltage." `cpu-clock.md`: a grant must record the user's approval for that change. Commit af049839 for A76 placement; d3a44665 for the grant; 580e6e8b for performance modes (whose design records "loadable bytes equal to the base" for default images, a size argument that no longer holds under one image).
- **Hardware first touched and protection:** the first rail and PLL writes happen in `a733_cpu_clock_request`, called from the thermal task. Protected today by the compile flag and by the grant record (magic, version, size, epoch and generation equal to the session's, 1 to 64 changes, cluster maxima in the table, nonzero approval hash). A grant with `clusters == 0` already means "inspect only", so the record check is partly run time.
- **Run-time form:** granted when the manifest declares a `cpu` clock section, the session carries the CPU grant extent, and the board description lists the cluster. Without a grant the thermal task does the read-only inspection that `clusters == 0` gives today and nothing else. `PERF` is granted by the manifest's `perf` declaration with its boot mode (`high` or `low`), carried in the grant extent. The speed bin is read from the SID word 0x0300624c as today.
- **Defect to fix in this series:** a manifest that asks for the A76 cluster must be refused when the board description or the grant does not allow it; today `application.c` places it on core 2.
- **Gap, found while reading:** `docs/designs/cpu-a76-dvfs.md` says the approval and B1 record tools are not implemented in `apps/os/prepare.py`, while `os-image-history.md` says preparation writes them. I found no `cpu_clock_approval` code in `apps/` or `tools/`. The series design says whether the approval record exists and, if not, what replaces it, and goes to the design panel (it changes how the DCDC3, DCDC5 and CPU PLL writes are granted) before any code.
- **EL3 against EL1:** no code today checks the grant at EL3. `os_session_valid` runs there and calls `a733_cpu_clock_grant_load`, so the check happens at EL3 admission; the register writes happen at EL1. The design keeps that split.

## Mutual exclusions

Today's rules, and what each becomes at admission:

| Rule | Kind | Run-time form |
| --- | --- | --- |
| AUDIO and HEADSET | convenience: one recognizer source | manifest rule, already present (at most one capture device) |
| DMA and AUDIO | one DMA0 owner and INTID 143 | admission refuses a session granting both |
| Radio with headset | shared AIC8800 owner | not a refusal; the owner is chosen from the granted set |
| Resident with CPU grant and audio, radio or headset | policy: "recorded-card session profile" | admission refuses it until a measurement lifts it |
| NPU with audio, radio, headset | policy: "recorded recognition" | admission refuses it until a measurement lifts it |
| USB JPEG with audio, radio, headset, NPU, GPU linear | allocator serialization | admission refuses it; fixed by allocator work, not by this design |
| CPU return with resident, audio, radio, headset, USB classes | return path not qualified | admission refuses it |
| NPU with GPU or DMA in a returning image | retirement not qualified | admission refuses it |

The refusals are one table in the admission code, each row with its reason. A refusal that is a convenience becomes a candidate for removal once checks cover the combination; this design does not remove them.

**The two NPU rows.** "The NPU with the radio" and "the NPU with I2S capture" conflict with the robotics target, where a robot runs models on the NPU while it talks over Wi-Fi and listens. They stay for now.

- *Original reason.* The rows are the build's refusal of 24 October ("The NPU OS profile requires recorded recognition and a supported GPU/CPU composition"), added with the first NPU profile (commits 70403fba and d7112328, "work in progress"). The profile was qualified with recorded input only, and the build refused everything it had not been run with. I found no recorded hardware conflict, failure or shared-resource analysis behind either row; they were a scope limit of the first profile.
- *What the two compositions share.* DRAM bandwidth (NPU graphs and Wi-Fi DMA or I2S DMA), the thermal gate (the NPU's ALLOW and STOP come from the one thermal owner), the supervisor's windows (radio and network 1 s, audio 1 s, NPU 5 s), the interrupt cores, and the DMA0 channel of I2S. They share no rail: the NPU is PCK600 domain 4 and the PLL_NPU clock, the radio module is BLDO5 behind SDC1, and I2S is PB5, PB6 and PB8.
- *The measurement that lifts the NPU and radio row.* On the board at the approved top clocks, run an NPU graph stream and a Wi-Fi TCP exchange together for the length of a recognition session (at least 10 minutes) against the same two alone. Pass when (a) no supervisor window is missed and the worst radio poll and network step latency with the NPU running are within 10 percent of the radio alone at the 99.9th percentile, (b) the NPU's job latency distribution (p50, p99, maximum) with the radio running is within 5 percent of the NPU alone, (c) the thermal level never reaches STOP from the added load, and (d) the TCP exchange loses no data. The record is an evidence record with the prediction written first.
- *The measurement that lifts the NPU and I2S row.* The same, with a capture loop in place of the Wi-Fi exchange: pass when the I2S ring (4,096-byte periods, 64 entries) records no overrun and no late period over at least 10 minutes with the NPU saturating DRAM, the worst DMA0 service latency is within 10 percent of the capture alone, and (b) and (c) above hold.
- *What a pass changes.* The row is removed from `refused_compositions`, with the evidence record named in the commit, and the `all` image is then the configuration under test.

## Size

Existing builds from the wifi-stages gate (`~/.cache/sunstoneos/build/gate-os-*-wifi-stages-1103-3798495c50`), `.text` + `.rodata` / `.data` / `.bss` in bytes:

| Image | text + rodata | data | bss |
| --- | --- | --- | --- |
| sup | 634,308 | 2,680 | 1,759,168 |
| res | 635,820 | 2,680 | 1,759,168 |
| resaud | 713,260 | 2,680 | 1,879,232 |
| resradio-headset | 789,724 | 2,680 | 2,668,608 |
| headset | 787,436 | 2,680 | 2,700,096 |
| npu | 742,788 | 5,000 | 1,866,944 |
| cpu-clock | 651,076 | 2,680 | 1,793,920 |

The all-in link (`all` in `VARIANTS` until series 4, whose `sup` links the NPU and so replaces it): audio, radio, NPU with the queue, A76 admission, CPU clock grants and performance modes in one supervised image, with no application. Measured with `aarch64-none-elf-size` and the linker symbols:

| Image | text + rodata | data | bss | RAM end |
| --- | --- | --- | --- | --- |
| `sup` | 635,284 | 2,680 | 1,759,872 | 0x416af000 |
| `all` | 867,100 | 7,944 | 2,038,912 | 0x416f4000 |
| `headset` (separate, for comparison) | 788,476 | 2,680 | 2,700,800 | 0x41795000 |

The symbol-union estimate in the first version of this document (0.96 MB) was high; the link is 0.87 MB.

The headset library, measured on the service series with `aarch64-none-elf-size`: the supervised image with no application against the same image with the `headset-fixture` application, and the sum of the library's objects:

| Build | text + rodata | data | bss | heap end |
| --- | --- | --- | --- | --- |
| `sup` at the service series | 764,028 | 2,752 | 7,144,944 | 0x414b7d80 |
| `headset` (`sup` with `headset-fixture`) | 954,196 | 2,808 | 8,422,896 | 0x415ef500 |
| difference | +190,168 | +56 | +1,277,952 | +0x137780 |
| library objects | 201,421 | 8 | 948,008 | |

The library's objects include sections that the link discards. Most of the `.bss` difference beyond the library is the fixture's capture buffer: 160,000 mono 16-bit frames, rounded up to 4,096-byte pages, plus a 64-byte header (323,648 bytes).

Fit:

- **Code and read-only data:** the `CODE` region is 2 MiB, shared with the application. The all-in OS takes 0.83 MiB and leaves 1.17 MiB. The headset library with mSBC, when an image links it, adds 0.18 MiB (measured below).
- **Data, bss, heap and device pool:** `RAM` is 10 MiB (0x41200000 to 0x41c00000). The all-in image ends at 0x416f4000, 5.0 MiB in. The headset library adds 0.90 MiB of `.bss`; the headset fixture image ends its heap at 0x415ef500.
- **Fixed regions untouched:** the NPU pool, the monitor interval and the uncached block keep their addresses. The pool is sized by the umem branch.
- **If something does not fit,** the first candidates are the headset state and the audio ring, both of which the "no artificial caps" section moves to allocation at the grant, and then object libraries linked by manifest like `sunstone-a733-os-sd`.

## Real-time cost

A disabled service costs one load at start and its code size. Nothing polls. Granted services are created exactly as they are today; their tasks, interrupts and windows are unchanged. The supervisor's `required` mask is computed once. Service ids are fixed, so `board_service_progress` indexes the same arrays.

## Implementation order

One option family per commit series. Each series lands on its own, passes its checks and removes its variants from `VARIANTS` before the next is built on it:

1. **Foundation (done).** The granted set (`struct os_session` holds every record and `granted`; the wire form carries a record only for a granted service and the thermal record's size follows the records present), the fixed service ids, the refusal table (`apps/os/grants.c`), the start-entry guards of the NPU, I2S and radio, the EL3 identity hook reading the radio profile and chip ID only for workload 1, the `all` variant and the ungranted-service checks. No option removed yet; the options now decide only which code is linked.
2. **CPU family** (`A76_ADMITTED`, `CPU_CLOCK`, `PERF`): implemented. It removes `cpu-clock`, `perf` and `perf-npu-low`. See [its design](one-os-image-cpu.md).
3. **Services** (`AUDIO`, `RADIO`, `HEADSET`, `MSBC`): implemented. It removes `resaud`, `resradio`, `resradioaud`, `radio-headset` and `resradio-headset`; `headset` and `resheadset` build the headset fixture application. The radio has its device bit, the I2S gate is an admission check (device bit and record together), and the headset and mSBC are the manifest-linked object library.
4. **NPU** (`NPU`, `NPU_QUEUE`, `NPU_OBSERVE`, the catalogue): implemented. It removes `npu`, `npu-queue` and `all`; `sup` links the NPU service, the ring and the observer.
5. **Lifetime** (`RESIDENT`, then `CPU_RETURN`): removes `res` and the returning variants. The EL3 diffs (`boot.c`, `continue.S`, admission) are reviewed before any board run. After landing, one supervised, one resident and one returning run each predict their stop record from the latest record of that lifetime.

After the series the gate builds one OS image (`sup` becomes `os`). The images that stay are separate programs, not OS variants:

- `stage2`: the stage-2 loader, a separate 32 KB boot program.
- `storage-read`, `storage-read-p1`: raw storage probes (no OS).
- `ret-gpu`, `ret-latency`, `ret-npu-load`: returning application images. They are the one-OS image plus an application, not other OS builds, once series 5 is done. Until then they carry their option flags.
- The probe and diagnostic images built by `cmake/a733-*.cmake` (standalone boot, memtest, SMP observe, hw-facts and similar) stay as probe programs under the [probe contract](../board-probe-contract.md), because each is a raw program with its own interrupt table and cannot share the OS's.

Options outside the 16 variants (`GPU`, `DMA`, `USB_CLASSES`, `USB_JPEG`, `SD_WRITE`, `GPU_RECOVERY_TEST`) follow the same mechanism and are the next series after these. `GPU`, `DMA` and `USB_CLASSES` are compiled in returning images that this series leaves as they are.

## Risks

- The session layout change touches the EL3 admission code and the host tools that build sessions (`apps/os/host.py`, `tools/gate_session.py`, `tools/host_admission.py`). Every session format change has to be checked against the stage-2 loader's rules (656 to 4,096 bytes, no extent read).
- Lifetime selection from the contract magic puts a branch in EL3 code that arms or leaves the watchdog, and the returning lifetime adds lifetime branches to the EL3 stop handler and fatal vector. The trace-equality check in the CPU_RETURN section guards the supervised and resident stop paths.
- Two other streams change files this work touches: `work/gate-streamline-1103` (`checks/gate-inputs.json`, `tools/land.py`) and `work/timing-checks-1103` (QEMU checks). I merge `bringup` before each series.
