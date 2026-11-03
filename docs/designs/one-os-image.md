# Design: one OS image, with services chosen at run time

Status: the foundation (granted set, session records, fixed service ids, refusal table, ungranted-service checks) is **implemented and host checked**. The service, CPU, NPU and lifetime series are **proposed**. The sizes in "Size" are measured on the all-in link.

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
- **NPU.** Slots, graphs, links, graph bytes and tensor bytes already come from the declaration (`npu-declaration.c`). The remaining fixed pieces are removed: the model table (see NPU), the EL1 metadata arena (`os-npu-meta.h`, being deleted by `work/umem-dynamic-pool-1103`; this work builds on it once it lands and does not touch pool sizing, `SUNSTONE_OS_NPU_POOL_BASE` or `SUNSTONE_OS_NPU_POOL_BYTES`), and the queue depth. The depth sizes `entry[]` in `struct ss_npu_queue`, an ABI shared with EL0 (`SS_NPU_QUEUE_MAX_DEPTH`, default 64). The ring becomes a header with `depth` and a flexible entry array, allocated at registration to the manifest's `in_flight`.
- **Audio.** The capture buffer already follows the manifest (`64 + max(8192, ceil(max_frames*channels*bytes/4096)*4096)`). The 119,680-byte `.bss.audio` (period ring of 64 entries, 4-page DMA pool, 8 KiB stack) becomes allocated at the grant from the period and frame counts in the audio extent.
- **Headset.** `.bss.voice` (440,448 bytes), the headset application state (353,888) and the 96,044-byte WAV buffer are sized from the Bluetooth profile at the grant.
- **Placement.** `A733_SMP_APPLICATION_CORE` and `_WORKER_CORE` become run-time values read from the grant; the `_Static_assert`s in `apps/os/start.c` become admission checks.

## Inventory by option

Counts are tracked files that mention the option, including docs.

### `SUNSTONE_A733_OS_RESIDENT` (lifetime)

- **Compiles in or out:** `SUNSTONE_BOARD_RESIDENT`: the contract magic (`A733_OS_RESIDENT_CONTRACT_MAGIC` against the supervised one) and the board timing validator in `apps/board/admission.c` (resident: no deadline, expiry at armed time plus 4 s, watchdog fields set; supervised: 60 s, zero watchdog fields); the `supervise()` renewal task and `apps/os/watchdog.c`; the manifest lifetime and return-reserve checks (`session.c:161`) and `grant.finish_by_us` (`application.c:409`); the USB0 product id; the audio DMA established flag (0x3f against 0x5f); the NPU end-of-work cap; `radio_profile.c` timing (120 s and 130 s offsets against 60 s); extra held-policy steps in `platform/a733/cpu_start.c`; `boot-resident.patch` (drops one EL3 readback). The stage-2 loader builds only the resident contract.
- **Why an option:** commit 5be126a5 says every board image ended at a fixed deadline and `SUNSTONE_BOARD_RESIDENT` admitted a contract with its own magic that arms the four-second watchdog. `os-image-history.md` keeps the supervised lifetime "for the first physical runs". It is risk containment, not a size or ISA reason.
- **Run-time form:** the lifetime is already encoded in the contract the loader or host writes: the magic. EL3 admission accepts the set of magics, and selects the timing validator and the watchdog expectation from the verified magic before any readback. Application, session decode, NPU, audio and radio code read the admitted lifetime from a sealed field, not a macro. The choice never comes from session or application bytes: a wrong choice would leave a watchdog armed with no renewer.
- **Stays a build option:** the stage-2 loader (a separate program, 32,608 bytes against a fixed limit). `boot-resident.patch` is the only EL3 boot difference and folds into a run-time branch on the magic. The lifetime series does not touch the EL3 stop handler.

### `SUNSTONE_A733_OS_CPU_RETURN` (lifetime, not among the 16 variants)

- **Compiles in or out:** 12 patch files (about 800 lines) over 11 units, including the EL3 stop handler (WDT0 shortened to 0.5 s and restarted so the board returns to FEL), the vector and terminal code and the boot; `cpu-return*.c`, `cpu-return-el0.S`; the GPU park and retire path; the NPU retirement wait in `ss_finish`.
- **Why an option:** commit 6976bc48 keeps other lifetimes unchanged while it is qualified.
- **Run-time form:** a third contract magic, handled like resident. It is the last piece. If its EL3 patches cannot become branches without changing the EL3 stop handler or the supervised and resident EL3 stop paths, it stays a second OS image and the gate builds two, and the reason is recorded here.

### `SUNSTONE_A733_OS_AUDIO` (I2S capture)

- **Compiles in or out:** `audio.c`, the DMA service, the I2S and audio-lease drivers: about +76 KiB `.text`, +120,128 bytes `.bss` (`resaud` 670,860 against `res` 594,412 bytes of text). The 184-byte `os_audio_session` extension; `OS_SERVICE_AUDIO`; route `{143,0,0xd8,os_audio_irq}`; pins PB5, PB6, PB8; `OS_STAGED_WORKLOAD` forced to 0.
- **Why an option:** commit d3fa6637: the loader admitted only the 808-byte session. e4005b17 allowed resident once the loader admitted 992 bytes. The exclusion with HEADSET ("an application takes one capture source") is software convenience; the manifest already refuses two capture devices.
- **Today's gate:** `os_session_valid` demands a valid extent in an AUDIO build; I found no test of the I2S device bit in `os_capture_start`; the bit gates only `ss_capture`.
- **Run-time form:** granted when `devices` has `SS_DEVICE_AUDIO_I2S` and the board description lists the wired microphone and the extent validates. Not granted: `os_audio_pins`, the DMA0 open and the route do nothing. The workload word is derived from the granted set.

### `SUNSTONE_A733_OS_RADIO` (Wi-Fi station)

- **Compiles in or out:** almost nothing. The board's `main.c` already links all radio code, so `.bss.radio` (354,832 bytes) is in every OS image; the option adds `radio.c`, `wifi_credential.c`, `main.patch` (progress reports for `OS_SERVICE_RADIO` and `OS_SERVICE_NETWORK`) and, for resident, `radio_profile.c`. `resradio` is +576 bytes of text over `res`. It forces workload 1 and sets the fixed addresses: radio profile at 0x43000000 (1,872 bytes), credential record 0x43004000 (256 bytes), firmware span from 0x43010000 (0xf0000 bytes).
- **Why an option:** commit 5a5672c1 added the radio and headset as build options so the first images stayed as qualified. No size reason is recorded.
- **Today's gate:** the board activates the radio from the session word (`a733_board_radio_workload(workload)` is `workload & 1`), so part of the gate is already run time. The module is powered only after the profile digest in the sealed board prefix matches, the `platform_authority` and owner token check, and the BLDO5 first-run guard passes. There is no device bit and no extent.
- **Run-time form:** new device bit `SS_DEVICE_RADIO` (bit 8; `SS_DEVICE_ALL` is `(1u<<8)-1` today), a radio extent carrying the profile digest and the credential record location, and a board-description entry for the module. Workload 1 is derived from the bit. The power-on guard is unchanged.

### `SUNSTONE_A733_OS_HEADSET` and `SUNSTONE_A733_OS_MSBC`

- **Compiles in or out:** the Bluetooth stack, UART driver and headset capture client: +151,616 bytes `.text` and +940,928 bytes `.bss` over `sup`. The 832-byte Bluetooth profile at 0x43000000, or 0x43008000 with the radio. Services `bluetooth` and `headset`. MSBC adds libsbc, `msbc.c`, `playback.c` (about 147 KiB, 6 KiB `.bss`, measured on an older build) and capture mode 3.
- **Why an option:** commit 5a5672c1 ("supervised only, because the Bluetooth application ends 60 seconds after it starts"); 61321c21 lifted the pairing with the radio by sharing one module owner. External pinned sources (`SUNSTONE_BT_SECURITY_DIR`, `SUNSTONE_BT_SBC_DIR`) were optional inputs.
- **Run-time form:** the one OS image has no headset code. A headset is not a current priority (the USB priorities are HID, UVC webcams and USB serial), and its sources (Zephyr Bluetooth security, libsbc) must not become inputs of every build. The Bluetooth stack, the headset client and MSBC form an object library linked by the application's manifest, like `sunstone-a733-os-sd`, into an image whose manifest requests the headset. The library's directory records the libsbc LGPL notice where it is built. Until that series, the headset stays a build option with its present rules (`headset_workload_valid` in `apps/os/headset.c` keeps the narrower workload rule that `os_workload_valid` no longer expresses).
- **Real constraint:** the radio and the headset share one AIC8800 module owner. Run time picks the owner from the granted set; it is a hardware fact, not an option.

### `SUNSTONE_A733_OS_NPU`, `_NPU_QUEUE`, `_NPU_OBSERVE`

- **Compiles in or out:** the NPU runtime, service and client (`apps/os/npu*.c`, `drivers/npu/*`, `runtime/npu_app.c`): +102 KB `.text`, +2.3 KB `.data`, +233 KB `.bss` at the same commit (728,212 against 626,260 bytes of text with no NPU, on the umem-core build). The 296-byte `os_npu_session`; service `OS_SERVICE_NPU`; route `{97,OS_NPU_CORE,0xd8,os_npu_irq}`; the pool link script. The queue adds the EL0 ring (`npu-queue.c`, `runtime/npu_app_queue.c`) and forces the main core to 1 and refuses the A76 cluster. OBSERVE adds diagnostic sampling and needs CPU_RETURN.
- **Why an option:** commits 70403fba ("an optional supervised OS profile … work in progress"), a600b85d (queue), defb4947 (observe). `docs/designs/npu-resources.md` already says a graph named in a build list "is an optional convenience".
- **Hardware first touched:** `os_npu_start` binds the interrupt and tasks only; the first clock, reset, power-domain and SRAM-route writes come from the bootstrap task, and only after the thermal gate is ALLOW and the session extent validated.
- **Run-time form:** the manifest's `npu` section is the declaration. The extent keeps owner, `command_admission`, supervisor, `pck_client` and the evidence digests, but loses `catalogue` and `client_model`. The core-1 rule of the queue becomes an admission rule (the queue image refuses the A76 cluster), not a macro. OBSERVE is a diagnostic: it moves to the manifest `observe` flag in the same extent; it stays refused outside a returning lifetime.

#### The model catalogue goes

`SUNSTONE_OS_NPU_CATALOGUE`, `apps/os/npu-models.json`, `apps/os/npu-catalogue.py` and the generated `npu-models.c` exist to link a reviewed allowlist of graph hashes into the image. The policy is now to run graphs as compiled, confined by the NPU page tables (proven on the board), with no program allowlist. The OS runtime already admits by structure and address (`os_npu_register`, `ss_npu_register_graph`, external models by address). So:

- **The OS links no graph and no model list.** `os_npu_models`, `os_npu_model_info`, `os_npu_model_count`, `os_npu_client_entry`, `os_npu_client_model`, `os_npu_catalogue_digest`, `os_npu_inputs` and `os_npu_references` leave the image. `SUNSTONE_OS_NPU_CLIENT_MODEL`, `_QUEUE_MODELS`, `_MODEL_NAME`, `_ALL_MODELS`, `_EXTERNAL_MODELS` and `_CATALOGUE` go with them. The session's `catalogue` digest (compared in `admission.c`) is removed.
- **Applications supply graphs by address**, as application data in an INPUT or large region, or by file once the SDK file API exists. The runtime reads tensor counts and sizes from the graph header, which it already does for registered graphs, so the model-info metadata needs no separate table. The runtime table is grown at registration.
- **The reviewed compiler outputs, with their reference inputs and outputs, become fixtures** (`tools/npu-session-models.json` renamed for what it is, plus the artifact map) for host and board checks. Fixtures are not part of any image build.
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

`drivers/npu/npu_session.c`, `apps/npu-board`, `include/npu_admitted_models.h`, `tools/npu-session-catalogue.py` and `tools/npu-session-models.json` still use the allowlist. Their only image is the `SUNSTONE_A733_NPU_BOARD` composition (`cmake/a733-npu-board.cmake`), with the host tools `tools/check-a733-npu-board-supervised.py` and `checks/npu-board-capture.py`. The OS NPU runtime (`a733_npu_runtime.c`) is the successor and shares the NPU startup sequencer and IRQ file with it (`platform/a733/npu_irq.c` is included by both `apps/os/npu-irq.c` and `apps/npu-board/irq.c`).

I propose retiring it: delete `npu_session.c`, `apps/npu-board`, `npu_admitted_models.h`, the NPU board cmake and its two checks, after confirming that no evidence record needs to be re-run from the board image (the runs in `docs/evidence/npu-board.jsonl` stay as records). Open points I have not resolved: `apps/npu-convolution` and `tools/check-a733-npu-supervised.py` and `check-a733-accelerators-supervised.py` read `tools/npu-models.json`, a separate list, so they depend on the older catalogue tooling and not on the OS one; they need their own look. `apps/npu-finite` also reads it. Retirement is a separate series after the OS catalogue is out, and I will ask before deleting anything in that group.

### `A733_A76_ADMITTED`, `SUNSTONE_A733_OS_CPU_CLOCK`, `SUNSTONE_A733_OS_PERF`, `SUNSTONE_A733_OS_PERF_BOOT`

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

The all-in link (`all` in `VARIANTS`): audio, radio, NPU with the queue, A76 admission, CPU clock grants and performance modes in one supervised image, with no application. Measured with `aarch64-none-elf-size` and the linker symbols:

| Image | text + rodata | data | bss | RAM end |
| --- | --- | --- | --- | --- |
| `sup` | 635,284 | 2,680 | 1,759,872 | 0x416af000 |
| `all` | 867,100 | 7,944 | 2,038,912 | 0x416f4000 |
| `headset` (separate, for comparison) | 788,476 | 2,680 | 2,700,800 | 0x41795000 |

The symbol-union estimate in the first version of this document (0.96 MB) was high; the link is 0.87 MB.

Fit:

- **Code and read-only data:** the `CODE` region is 2 MiB, shared with the application. The all-in OS takes 0.83 MiB and leaves 1.17 MiB. The headset library, when an image links it, adds about 0.15 MB (0.30 MB with MSBC).
- **Data, bss, heap and device pool:** `RAM` is 10 MiB (0x41200000 to 0x41c00000). The all-in image ends at 0x416f4000, 5.0 MiB in. The headset adds 0.94 MB of `.bss`, which still ends below 0x41800000.
- **Fixed regions untouched:** the NPU pool, the monitor interval and the uncached block keep their addresses. The pool is sized by the umem branch.
- **If something does not fit,** the first candidates are the headset state and the audio ring, both of which the "no artificial caps" section moves to allocation at the grant, and then object libraries linked by manifest like `sunstone-a733-os-sd`.

## Real-time cost

A disabled service costs one load at start and its code size. Nothing polls. Granted services are created exactly as they are today; their tasks, interrupts and windows are unchanged. The supervisor's `required` mask is computed once. Service ids are fixed, so `board_service_progress` indexes the same arrays.

## Implementation order

One option family per commit series. Each series lands on its own, passes its checks and removes its variants from `VARIANTS` before the next is built on it:

1. **Foundation (done).** The granted set (`struct os_session` holds every record and `granted`; the wire form carries a record only for a granted service and the thermal record's size follows the records present), the fixed service ids, the refusal table (`apps/os/grants.c`), the start-entry guards of the NPU, I2S and radio, the EL3 identity hook reading the radio profile and chip ID only for workload 1, the `all` variant and the ungranted-service checks. No option removed yet; the options now decide only which code is linked.
2. **CPU family** (`A76_ADMITTED`, `CPU_CLOCK`, `PERF`): removes `cpu-clock`, `perf` and `perf-npu-low`. Its design goes to the three-reviewer panel before any code.
3. **Services** (`AUDIO`, `RADIO`, `HEADSET`, `MSBC`): removes `resaud`, `resradio`, `resradioaud`, `headset`, `resheadset`, `radio-headset`, `resradio-headset`. The radio gets its device bit and extent, the I2S gate becomes a real admission check (device bit and record together, done in the foundation), and the headset and MSBC become the manifest-linked object library.
4. **NPU** (`NPU`, `NPU_QUEUE`, `NPU_OBSERVE`, the catalogue): removes `npu` and `npu-queue`. Builds on the umem dynamic-pool branch.
5. **Lifetime** (`RESIDENT`, then `CPU_RETURN`): removes `res` and, if it folds, the returning variants.

After the series the gate builds one OS image (`sup` becomes `os`). The images that stay are separate programs, not OS variants:

- `stage2`: the stage-2 loader, a separate 32 KB boot program.
- `storage-read`, `storage-read-p1`: raw storage probes (no OS).
- `ret-gpu`, `ret-latency`, `ret-npu-load`: returning application images. They are the one-OS image plus an application, not other OS builds, once series 5 is done. Until then they carry their option flags.
- The probe and diagnostic images built by `cmake/a733-*.cmake` (standalone boot, memtest, SMP observe, hw-facts and similar) stay as probe programs under the [probe contract](../board-probe-contract.md), because each is a raw program with its own interrupt table and cannot share the OS's.

Options outside the 16 variants (`GPU`, `DMA`, `USB_CLASSES`, `USB_JPEG`, `SD_WRITE`, `GPU_RECOVERY_TEST`) follow the same mechanism and are the next series after these. `GPU`, `DMA` and `USB_CLASSES` are compiled in returning images that this series leaves as they are.

## Risks

- The session layout change touches the EL3 admission code and the host tools that build sessions (`apps/os/host.py`, `tools/gate_session.py`, `tools/host_admission.py`). Every session format change has to be checked against the stage-2 loader's rules (656 to 4,096 bytes, no extent read).
- Lifetime selection from the contract magic puts a branch in EL3 code that arms or leaves the watchdog. It does not change the EL3 stop handler.
- Two other streams change files this work touches: `work/gate-streamline-1103` (`checks/gate-inputs.json`, `tools/land.py`) and `work/timing-checks-1103` (QEMU checks). I merge `bringup` before each series.
