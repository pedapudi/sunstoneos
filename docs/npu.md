# NPU implementation and model format

NPU startup, one inference and a two-model runtime are **board-qualified** on
port 3-3 at 852 MHz and 800 mV. Each returns normally through the host-armed
watchdog contract after CLOSE, local stop and matching output checks.
The evidence is under `~/.cache/sunstoneos/build/npu-wdt-1031/runs/`:

| Operation | Evidence directory | Result |
| --- | --- | --- |
| Startup without inference | `01-startup-only-28c8e15a2b62-834973ec/` | `NPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0` |
| One inference | `02-single-inference-0dd327616ce6-834973ec/` | `NPU_INFERENCE=MATCH; inference_jobs=1` |
| Two-model runtime | `03-runtime-8f51a95699b7-834973ec/` | `NPU_INFERENCE=MATCH; inference_jobs=2` |

The queued returning diagnostics have these results on port 3-3 at the same
852 MHz and 800 mV point. Evidence directories are under
`~/.cache/sunstoneos/board-queue/entries/`. Each entry contains `run/result.json`,
its frozen image and transfer evidence. Successful entries also contain validated
retirement reports.

| Operation | Result | Evidence directory |
| --- | --- | --- |
| Local stop and restart | Two matching jobs, four interrupt completions, normal return and restored DRAM | `1793514579021691740-d818fe48/` |
| Switch conv12 to conv8 and back to conv12 | Three matching jobs, four interrupt completions, normal return and restored DRAM | `1793514712212240796-1a65224c/` |
| Public application API | Two matching models, three interrupt completions, normal return and restored DRAM | `1793514867262701593-6a1aa965/` |
| Physical domain OFF and restart | Unsupported; the frozen diagnostic has no accessible return within 46 seconds, and the instance is quarantined | `1793514996007883230-40e64d60/` |

The [board queue](board-runner.md) validates finite NPU sessions and retirement
receipts. The power-cycle run has no returned breadcrumb or first-failure record;
its failing sub-step is unknown. Port 3-3 is physically reconnected in FEL mode;
power-state changes require coordinator approval. The finite SDK diagnostic uses an EL1 application task;
full OS EL0 mappings remain unqualified. These bounded successes do not qualify
repeated transition reliability. Active-job reset recovery, thermal transitions,
long-duration operation and representative concurrent workloads remain unqualified.
The [native application record](npu-application.md) preserves narrower historical
attempts and their recovery evidence.

## The supported NPU power boundary preserves shared SRAM

**Observed:** the failed image is `image.elf`, SHA-256
`5523f7c97f2896611ac7f3230d5a3df9c6e647085abd97cf43d251d0f1347e98`,
from native source `593ce329`. Its queue entry is the domain-OFF row above.
For this failed run, the coordinator confirms that port 3-3 disappeared from
USB entirely. No native report or retained fault was recovered. The completed
host transfers establish watchdog arming and acceptance of the launch command
for `0x52000`. They do not establish which native power sub-step executed.

### Supply sharing and the effective vendor path

**Observed:** the cached Orange Pi V1.2 schematic, SHA-256 `4e0e92afbb1cca97…`,
sheets 2, 6 and 8, connects NPU VDD to DCDC2 with VDD-SYS, VDD-DRAM, VDD-VE,
VDD08-UFS, VDD-COMB0/1 and VDD08-HDMI. DCDC2 is an always-on shared supply.
VDD-DRAM here names the SoC-side supply; it does not identify every LPDDR5
supply. The board device tree binds `npu-supply` to DCDC2 and marks it
boot-on and always-on (`board-kernel/sun60i-a733-orangepi-zero3w.dts:2123–2131,2931–2933`).

| Consumer | Schematic supply | Relationship to DCDC2 |
| --- | --- | --- |
| NPU, SoC system fabric and SoC-side DRAM logic | DCDC2 | Shared; must remain powered |
| A76 and A55 CPU cores | DCDC3 and DCDC5 | Separate CPU supplies |
| USB core and management processor | DCDC9/ELDO7 | Separate core supply; shared combo-PHY digital supplies still include DCDC2 |
| Analog PLLs, oscillator and VDD18-DRAM | CLDO3 | Separate analog supply |
| USB analog and I/O circuitry | Several 1.8 V and 3.3 V rails | Separate from the NPU's internal PCK switch |

**Observed:** the vendor PCK switch for NPU is domain 4, USB2 is domain 8,
and GPU_TOP is domain 5 (`gpu-power/include/dt-bindings/power/sun60iw2-power.h:5–15`).
Internal PCK domain gating does not turn the external DCDC2 regulator off.
The native PMIC provider admits selector changes only for DCDC3/4/5 and has
no regulator-enable write API (`drivers/power/a733_pmic.c:235–255`). Its
DCDC2/4 verification consists of register reads. Rail unpin/release changes
only software accounting (`drivers/power/a733_rail_leases.c:162–200`).
The failed path therefore has no intentional DCDC2-disable or DCDC2-retune write.

**Observed:** the pinned VIP2 platform initializes `enable_pm` false and sets
it only under `CONFIG_AW_PM_DOMAINS` (`vip_drv_device_platform.c:102,1212–1214`,
SHA-256 `a47f85f2266ec5a9…`). The selected board configuration leaves that
option unset (`board-kernel-config/linux-sun60iw2-current-a733.config.a95129bf:892`).
Its runtime OFF/ON hook consequently makes no PCK transition. The optional,
enabled branch calls runtime put before local clock shutdown, and initializes
local clocks before synchronous runtime get (`:419–439`). It does not disable
the PMIC rail. The PCK callback writes a policy nibble, completes it with DSB,
and polls status for at most 10 ms (`gpu-power/drivers/pm_domain/pck600_domains.c:105–123`,
SHA-256 `2d9c2b60b3b264d1…`). That optional branch is not proof that the
selected vendor board configuration cycles NPU power.

### The failed program's write order

**Observed source reconstruction:** the CPU memory-mapped register write inventory is saved
under `~/.cache/sunstoneos/build/npu-wdt-1031/power-fault/` as
`write-inventory.csv` and `write-inventory.json`. They enumerate conditional
writes and repeated bus transactions in producer order. The accompanying
`system-register-writes.json` records every linked `MSR` instruction by address
and function; `failed-image.disasm` preserves the binary evidence. Interrupt,
watchdog and service-task interleaving prevents a single source-derived total
order. **Unknown:** the prefix that actually executed on the failed board.
Graph-internal register programming belongs to the model command data;
it does not address the SoC power controller or PMIC.

| Phase, in dependency order | Register writes and controls |
| --- | --- |
| EL3 entry and interrupt routing | GIC distributor `0x03400000` and banked disable/group/configuration/priority/route words; CPU0 redistributor `0x03460000` and private interrupt page `0x03470000`. The inventory expands each offset and saved-state restoration. |
| MBUS and security | Conditional update/reset sequence at `0x02002588` and `0x02002594`, then SPC/TZMA/security words from `include/a733_el3_security.h:47–55`; no PMIC mutation. |
| Shared resource acquisition | Set NPU AHB bit 6 at `0x020025c0`, MBUS bit 18 at `0x020025e0`, and PCK clock bit 0 at `0x070101ac`, only when needed. |
| PMIC pins and reads | Temporarily select disabled mux on PL0 then PL1 at `0x07025000`, restore function 2 in the same order. S_TWI0 DATA/CNTR at `0x07083008` and `0x0708300c` send address/index/read/STOP sequences for PMIC registers `ff,0e,0f,10,13,ff`, then `ff,0e,0f,10,15,ff`. No PMIC register data byte is written. |
| Initial domain acquisition | Inherited ON causes no PCK write. Inherited coherent OFF would cause the five timing writes listed below, followed by ON policy and a status poll, before local opening. |
| PLL and local open | Optional PLL_NPU gate/control writes at `0x02003920` and `0x020022a0`; LOCK and settle precede output enable. Clear SRAM-route bit 1 at `0x03000008`; release AHB, AXI, core resets at `0x02002b04`; select PLL/divider at `0x02002b00`; enable bus then module gates. |
| Controller and inference | NPU `0x03600000` offsets `000,104,10c,3a8` implement two reset passes; then `55c,090,000,3a8,03c,100,3b4,388,104,10c,090,414,014` configure the engine. Command address `654` precedes barrier/fetch trigger `3a4`. The inventory lists every repeated value. |
| CLOSE and local stop | Engine interrupt `+014=0`, MMU `+388=0`; IRQ97 mask/retirement. Clear module gate, bus gate, AHB/AXI/core reset-release bits; set CPU SRAM-route bit 1. PLL and shared rails remain owned. |
| Added physical OFF | `0x07064170=1f1f1f`, `174=1f1f`, `c00=08080808`, `c04=808`, `c10=8`, then `0x07064000` policy low nibble 0; DSB and status `+8` polling. No PMIC write. |
| Added physical ON | The same five timing writes, then policy low nibble 8 and status poll; local reopening, a fresh bootstrap/job, CLOSE and stop follow. Native master-gate checks precede ON, while the optional vendor path opens local clocks first. |
| Recurring progress and watchdog | RTC offsets `100,104,10c,110,114,118,11c` from `0x07090000`; WDT restart `0x0205000c=14af`. Fault exit may select short WDT mode. |
| Normal ROM return | Restore saved GIC state, ensure CPU SRAM routing, disarm WDT at `0x02050014`, clean caches, write CPU0 RVBAR low `0x08001004=20` and high `0x08001008=0`, then `RMR_EL3=2`. No USB-controller or PMIC-rail write occurs. |

The controller order is defined by `a733_npu_controller.c:105–178,277–288`.
The failed power API starts at ELF `0x4100f3a0`, restart at `0x4100fdbc`,
and power service at `0x4101dd24`. All addresses name the SHA-256 above.

### Recovery evidence does not establish whether WDT0 expired

**Observed:** A733 manual V1.00 §2, Arm A76/A55 memory map, describes shared
SRAM `0x74000–0xf3fff` and explicitly says “NPU cannot power off.” The cached
text is `camera-research/A733_User_Manual_V1.00.txt:1291–1308`, SHA-256
`219d3c21e5377d18…`. ROM uses this window for its `0xf3400` stack and `0xee800`
FEL scratch (`docs/vendor-firmware/boot-rom-and-boot0.md`).

**Inferred:** physical NPU OFF can remove SRAM needed by ROM. If OFF closes
a master gate, the failed path can refuse its own ON precheck and retain
PCK4 OFF. A subsequent reset could then fail before USB enumeration if that
OFF state persists. **Unknown:** whether PCK4 survives WDT reset and whether
this chain occurred. Absent USB does not establish that WDT failed to fire.
Full-reset boot dispatch can also select storage rather than FEL.

**Observed alternate gap:** the frozen image disarms WDT at call address
`0x41026d34`, before whole-cache cleaning in the return tail at `0x4102b048`.
A stall there has no armed watchdog. **Unknown:** whether execution reached
that tail. No rail-loss or watchdog-expiry cause is established by this run.

### Correction and next control

**Implemented, host-verified:** the power-domain request API and owner release
path refuse domain-4 OFF before changing generation, pins, count or hardware.
The NPU physical-OFF/restart APIs record shared-supply/shared-SRAM refusal
and retained authority/ownership inputs before returning terminal retention.
A pending counted-row observation is recorded as unknown. DCDC2 is treated as
physically shared even when no other software token exists. No new PMIC or
domain read is needed to enforce the board policy.

Local clock stop/restart remains supported. New group-6 stages precede each
local stop/open table step, SRAM route, PLL output opening, PCK delay word,
policy and status operation. `NPU_POWER_PRECHECK`/`NPU_POWER_REFUSED` distinguish
policy refusal from a later generic failure. The first-failure value has bit 0
for shared DCDC2, bit 1 for SRAM power dependence, and bit 2 for a previously
verified supply. Full ownership observations remain in the startup object.
The [power contract](../apps/npu-finite/POWER-CYCLE.md) specifies these limits.
The current development runner refuses physical-OFF profiles before capture.
The board-image inspector independently refuses them even when a selected
frozen host parser still accepts the old image. Host cases exercise the failed
ELF and establish refusal before board-backend construction.

Independent correctness, hardware-safety and recovery panels approve the design
before coding and the final code. Host cases prove no forbidden OFF hardware
write or ownership mutation, DCDC2 refusal with sole shared/exclusive software
holders, delayed supported PCK transitions and unchanged local restart.
Physical domain OFF is unsupported; this change does not qualify it or identify
the failed board's exact stopping point.

**Observed control result:** the fresh local clock stop/restart image returns
normally on port 3-3 with two matching jobs, four interrupt completions and
verified DRAM restoration. It records no first failure or quarantine. Queue
entry `1793558381244643310-6a29a71d/run/result.json` is under
`~/.cache/sunstoneos/board-queue/entries/`. Native source is `32f18fe5`; the
runner is `6ebe687a`. The ELF is
`~/.cache/sunstoneos/build/npu-wdt-1031/control-1101/images/local-restart/image.elf`,
SHA-256 `dd826f5ff75795b6beb2b96b352e28439574156fc39c222cd00e338d70e8eac3`.
The control retains PCK4 and shared DCDC2 ownership. It does not establish
physical-OFF safety or active-job watchdog recovery. The separate physical-OFF
image must not be resubmitted.

The next single proposed run switches conv12 to conv8 and back to conv12 at
852 MHz/800 mV with domain power retained. It requires coordinator approval
before a fresh image is built. Expected results are three matching jobs,
four interrupt completions, normal return and restored DRAM. The risk is stale
model or mapping state producing a wrong output or a stalled command;
watchdog recovery during active SRAM routing remains unqualified.

The host model inspector passes a reference comparison.
[Page-table construction](npu-mmu.md) and the
[controller core](npu-controller.md) pass host models and compile for AArch64.
The [memory policy](npu-memory.md) passes host integration for private mappings,
table validation, cache publication, and retained faults. The
[native CPU adapter](npu-native.md) passes a FreeRTOS emulator check.
The [FreeRTOS service](npu-service.md) passes a two-core check with EL0 clients,
a preemptible EL1 worker, task ownership, and retained faults. The checked
[address relocation component](nbg-relocation.md) passes host bounds checks
and comparison with SDK instructions. A [complete host loader trace](nbg-loader-trace.md)
reconstructs all active relocations and matches SDK command buffers at two
allocation bases. The [native container decoder](nbg-model.md) passes bounded
view and malformed-input checks and supplies the same reconstruction from C
records using saved SDK evidence. The [native assembler](nbg-assembly.md)
now plans allocations and constructs the observed neural-operation profile,
matching saved SDK bytes after address and completion-event adjustments.
The [checksum verifier](nbg-checksum.md) matches two published models and is
required before assembly.
The assembler also validates [encoded circular-buffer ranges](nbg-rings.md),
with host field-packing and complete-submission comparisons.
The [pinned host compiler](npu-compiler.md) now produces a small numerical fixture
whose simulator output matches the quantized CPU reference. Its `0x20000` format
now has native decoding support. The [directory investigation](nbg-directory.md)
records its changed layouts and checksum coverage. Native checksum verification
passes host checks. The [convolution capture](nbg-conv-loader.md) supplies allocation
and command evidence now matched by native assembly with no working-pool allocation.
The [native convolution application](npu-application.md) implements platform
resource ownership, private memory, trusted compiled-model registration and
repeated-job inference through the FreeRTOS service. Three pinned convolution
artifacts are selectable before scheduling; their native modeled-output checks
and host numerical references do not establish physical inference. An independent
boot-CPU profile is implemented alongside the cached all-core profile. All-core
hardware support remains required.

The [session runtime](npu-session.md) runs up to eight catalogue models on
the NPU at once, one client task per model, with round-robin job starts
through the service's shared mode. The general assembler now accepts graphs
with several inputs and outputs, UINT8, INT8 and INT16 tensors, and the
transpose steps that the compiler emits around fully connected layers; its
output matches the pinned SDK loader for every catalogue graph. Sessions take
their memory from the platform pool by model size, which in the extended
cached profile is the DDR reservation. Emulator checks with modeled device
output pass. The bounded two-model runtime above also has physical evidence. The
[mimus layer study](npu-mimus-linear.md) shows that the simulator computes a
mimus int8 linear layer's sums exactly but rounds the output to INT16.

The single-core terminal-USB correction integrated as `ef00a07` allows an NPU
failure frame only while USB retains its independent authenticated lifetime,
clock, lock, task and provider checks. Ten affected native cases and independent
review pass under `npu-usb-terminal-20261018/`. The corrected image has not
established physical USB reporting or NPU execution; the reporting defect does
not identify the earlier physical failure's cause.

Recovery depends on the selected profile. The watchdog profile requires qualified
watchdog recovery and ROM return. The supervised RAM-only profile instead uses
reviewed manual whole-board power removal, storage preservation and one bounded
attempt. Independent watchdog qualification is not a blanket prerequisite.
Every resource remains reserved until the selected recovery condition is met.

This document assumes familiarity with drivers, virtual memory, and neural
network inference. A Network Binary Graph (NBG) contains compiled operations,
weights, tensor descriptions, and relocation records. A relocation describes
how the loader must patch an address after allocating device memory.

## Hardware and available driver source

The pinned [board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f)
contains the VIPLite 2 driver under `bsp/drivers/npu/aw_nna_vip/vip2`.
Its core hardware, memory, and scheduling files offer a dual MIT/GPL license.
An adaptation can select the MIT terms while preserving the required notices.
Downloaded reference files retain their original notices outside the repository.

The SoC device tree supplies the following resources. Native startup checks
its admitted core identity before executing the qualified inference profiles.

| Resource | Reference value |
| --- | --- |
| Registers | Physical `0x03600000`, length `0x1000` |
| Interrupt | SPI 65, GIC interrupt ID 97, level high |
| Clocks | NPU core, parent PLL, bus, MBUS gate, AHB gate |
| Resets | NPU core, AXI, AHB |
| Power domain | `SUN60IW2_PCK_NPU` |
| Expected chip product ID | `0x1000003b`; revision `0x9202` in the feature database |
| Feature database entry | One NPU engine instance (VIP core), eight neural processing units, 512 KiB internal SRAM, 32-bit device virtual addresses |

The expected product ID also appears in the A733 SDK sample. It must be checked
against physical identity before accepting a compiled model. The device-tree
operating-point hints do not establish a safe voltage or clock rate on this board.

The driver separates register control, device page tables, memory allocation,
command construction, task scheduling, and power management. Its OS interface
covers register reads/writes, allocation, cache maintenance, interrupt waits,
mutexes, atomics, threads, signals, timers, and monotonic time. Some core files
still include Linux-specific interface headers. The native application supplies
its own checked register access, private allocations, cache publication, bounded
polling and FreeRTOS task/lock interfaces through repository source. It does not
link the Linux driver or its file and ioctl interfaces into FreeRTOS. The first
trusted convolution defaults to polling with interrupt ID 97 masked. The optional
[finite interrupt profile](../apps/npu-interrupt/README.md) implements deferred
completion on that interrupt. The bounded returning profiles require interrupt
completion and have the physical evidence above. Broader concurrent use remains
unqualified.

No external firmware request was found in the inspected VIPLite 2 subtree.
That observation does not establish that every compiler or execution mode is
firmware-independent. The driver itself is available as source; binary analysis
is currently needed for parts of model loading.

## Model compilation and loading remain separate dependencies

The [A733 SDK](https://github.com/ZIFENG278/ai-sdk/tree/fc90006d0f6569da2f6726c2d8395877686f5aca)
is pinned at `fc90006d0f6569da2f6726c2d8395877686f5aca`.
The inspected AArch64 VIPLite 2 package publishes public API headers and two
shared libraries. `libNBGlinker.so` implements network and buffer operations.
It imports hardware-abstraction functions from `libVIPhal.so`, plus C library,
math, and pthread functions. The hardware abstraction library imports Linux
file, ioctl, and pthread operations. These Linux libraries cannot be linked
directly into the present FreeRTOS image.

The SDK also publishes `nbinfo`, an x86-64 model inspection executable with
function symbols and DWARF debug types. Disassembly, debug types, and a reader
trace establish the inspected container layout. The native inspector uses
these format facts; it does not execute downloaded commands or load either
AArch64 shared library.

The open [TIM-VX parser](https://github.com/VeriSilicon/TIM-VX/tree/702715d714277376d7ff466b7b72833ff57564c1/include/tim/utils/nbg_parser)
provides an MIT-licensed reference for an older format. At revision
`702715d714277376d7ff466b7b72833ff57564c1`, its maximum format version is
`0x10014`. The inspected A733 sample uses `0x1001e`, which adds fields and
sections. Reusing that parser unchanged would reject this sample.

The [compiler setup guide](https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-env)
identifies the A733 host package as `ubuntu-npu:v2.0.10.2`.
The [compilation example](https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-usage)
selects hardware version `v3` and runtime version `v2.0` for A733.
The [compiler record](npu-compiler.md) pins the downloaded image, target,
dependencies, and original license information. A small convolution now compiles
and passes a host simulator comparison. Native decoding, checksum verification,
allocation planning and assembly now support its observed `0x20000` profile;
the Python inspection tool remains limited to `0x1001e`.

The runtime links repository NPU/NBG source, FreeRTOS and TinyUSB source. The
native C/AArch64 image is built with the pinned Clang/LLD toolchain. Model
compilation uses the external x86-64 Linux ACUITY 6.30.22 and Vivante IDE 5.11.0
host package; it is separate from building the native executable. The application
artifact generator checks the model/input/reference hashes in
[`tools/npu-models.json`](../tools/npu-models.json) and embeds the selected bytes
without invoking that compiler. Model and command artifacts remain external
compiler outputs, not repository compiler source.

`libNBGlinker.so`, `libVIPhal.so`, `nbinfo` and the compiler image's simulator
supply the documented format, loader and address-path reference evidence. Their
Linux shared libraries are not loaded by the FreeRTOS application. Existing
platform DDR preparation also retains its pinned boot firmware. These accepted
dependencies, their execution locations and original license records are
separate from a claim that every NPU mode is firmware-independent. Source pins,
compiler identities and artifact provenance are recorded in
[npu-compiler.md](npu-compiler.md), [sources.md](sources.md) and the component
records. No dependency replacement is needed to attempt the trusted workload.

[Issue 12](https://github.com/pedapudi/sunstoneos/issues/12) tracks an
in-repository compiler and development toolkit with native FreeRTOS execution.
This replacement of external tools is deferred until complete hardware support
and performance tuning. Documenting their provenance, execution role and license
status takes priority over replacement. Published compiler dependencies remain
acceptable during bring-up and optimization. CPU, GPU and NPU optimization
follows the [validation schedule](validation.md).

## Structural inspection is implemented

Run the host inspector with a downloaded model path:

```sh
python3 tools/inspect-nbg.py /path/to/network_binary.nb
```

It accepts format `0x1001e`, limits input to 256 MiB, and emits JSON. It reads
little-endian fields explicitly. The fixed header occupies 592 bytes and
contains 27 section descriptors after byte 376. Their serialized order differs
from the inspection executable's in-memory structure order.

The output includes tensor dimensions and quantization, logical tensor bytes,
working-pool requirements, SRAM use, command sizes, section locations,
operation counts, and relocation-source counts. Names use replacement characters
for invalid UTF-8. Other format versions fail explicitly.

The inspector checks section bounds and overlap, table strides, header counts,
relative payload ranges, tensor sizes, finite positive quantization scales,
selected operation references, relocation tensor indices, and pool alignment.
It accepts only disjoint nonempty sections. Other legitimate layouts may require
an extension supported by a compiled fixture.

Successful inspection does not authorize hardware submission. The host inspector
does not resolve relocations, validate command semantics or SRAM placement, check
device identity, or verify integrity. Unknown operation and relocation-source
values appear explicitly in its inventory. Its output reports
`checksum_verified: false`; the native assembler uses the separate
[checksum verifier](nbg-checksum.md) before constructing commands.

## Reference comparison and malformed-file checks

The checked fixture is the SDK's
`examples/vpm_run/operator/v3/network_binary.nb`. Its SHA-256 is
`fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f`.
The filename is generic; the embedded network name is `ShuffleNetV2_uint8_NCHW`.
This fixture establishes container interpretation, not model accuracy or device
execution.

| Property | Observed value |
| --- | --- |
| File length | 964,224 bytes |
| Target and format | `0x1000003b`, `0x1001e` |
| Layers and operations | 87 layers; 125 neural-network operations and one initialization operation |
| Neural command size | 192 bytes |
| Relocations | 660 records, each 40 bytes |
| Coefficient data | 892,608 bytes |
| Working pool | 1,092,352 bytes, 64-byte alignment |
| Internal SRAM use | 518,144 bytes |
| Input | UINT8, dimensions `[224, 224, 3, 1]`, 150,528 logical bytes |
| Output | UINT8, dimensions `[2, 1]`, two logical bytes |

The published inspection tool reports a 64-byte output size despite the
two-byte logical tensor. The [SDK loader trace](nbg-loader-trace.md) records a
128-byte allocation request. The native assembler reproduces that reservation
and rounds it to a 4 KiB allocation for the memory policy. Complete descriptor
access extents remain unresolved. The host inspector reports logical sizes.

One focused check compares network metadata and tensor descriptions against
recorded `nbinfo` reports. It also rejects 18 malformed variants covering
truncation, unknown layout, invalid ranges and aliases, count mismatch, tensor
size and quantization errors, invalid references, and allocation alignment.
No physical request or OS rebuild occurred.

The check is reproducible with the same fixture and recorded reports:

```sh
python3 checks/nbg-inspection.py /path/to/network_binary.nb /path/to/reference-reports
```

Local evidence is under `~/.cache/sunstoneos/evidence/npu-source-20261016/`.
It includes reference reports, the reader trace, disassembly, check results,
and an input hash manifest. Download manifests under
`~/.cache/sunstoneos/sources/npu/` record source URLs, revisions, sizes, and
SHA-256 values. Retrieved files were also checked against Git blob identities.

## Descriptor field compatibility is a research result

Mesa's [neural-command implementation](https://gitlab.freedesktop.org/mesa/mesa/-/blob/45b86c99242b3dbf8bdaec7d5f90084106338a63/src/gallium/drivers/etnaviv/etnaviv_ml_nn.c)
provides a candidate field layout at revision
`45b86c99242b3dbf8bdaec7d5f90084106338a63`. Its MIT-licensed source is cached
with the original notices, download URLs, Git blob identities, and SHA-256 values
under `~/.cache/sunstoneos/sources/npu/mesa-45b86c99242b3dbf8bdaec7d5f90084106338a63/`.

All 534 descriptor relocations in the pinned A733 fixture fall at candidate
weight, tensor-address, or circular-buffer-end fields:

| Byte offset | Candidate meaning | Relocations |
| ---: | --- | ---: |
| 20 | Weight address, shifted by six | 77 |
| 24 | Input tensor address | 125 |
| 28 | Output tensor address | 125 |
| 76 | Output circular-buffer end plus one, shifted by six | 102 |
| 84 | Input circular-buffer end plus one, shifted by six | 105 |

This correspondence is an inference. Mesa's structure occupies 136 bytes;
the A733 descriptors occupy 192 bytes. Word 35 is nonzero in 77 descriptors,
beyond that structure. Mesa assigns image heights to its named Y-stride fields,
while the A733 fixture contains values resembling per-channel image area.
The first descriptor reports an input width and height of 224 but a Y-stride
value of 50,176. These differences prevent direct adoption as an access validator.
The [simulator investigation](nbg-strides.md) now identifies direct channel-stride
packing in the target's revision-9.2 decoder. It explains this difference but
does not establish complete access extents or prefetch behavior.

The [isolated HAL comparison](nbg-rings.md) confirms circular-size/end packing
and identifies a transpose selector in the 48 descriptors without coefficient
relocations. Native assembly now checks complete encoded rings and requires the
tensor address to lie within the ring. These checks do not establish actual
access extents or prefetch behavior.

The initial saved inventory is `descriptor-research.json` under
`~/.cache/sunstoneos/evidence/nbg-checksum-20261016/`. It includes every decoded
candidate field, relocation correspondence, and descriptor tail. The ring record
adds a complete range inventory. General model admission still needs resolved
instruction-family semantics, stride units, the extra word, linear access bounds
and prefetch extents. These structural comparisons alone do not authorize a new
model. The fixed trusted convolution has a separate
[address-path review](npu-address-path.md) of translated external accesses and
owned internal SRAM; its physical confinement remains unqualified.

## Implemented path and remaining qualification

1. **Qualify the trusted convolution on hardware.** Native decoding, checksum
   verification, allocation planning, assembly and immutable registration are
   implemented for the selected compiler artifacts. The application submits two
   distinct jobs and checks both copied outputs. Saved CPU and host-simulator
   references are valid evidence for comparison; modeled device output is not
   numerical inference. Keep the SDK ShuffleNet artifact as the separate
   `0x1001e` format reference.
2. **Qualify platform and device-memory behavior.** Native providers implement
   PMIC sampling, PLL acquisition, clocks, resets and power-domain startup under
   shared ownership. The permanent reservation tracks CPU/device addresses,
   private tables, cache publication and retained faults. Actual board admission
   must establish independent DRAM/PHY sources, complete SRAM ownership, supported
   operating points and excluded writers. Qualify the effective mapping, SRAM
   routes, cache visibility and translation-cache behavior physically. The
   [memory policy](npu-memory.md) records the separate SRAM route; ordinary RAM
   page-table entries do not replace its ownership contract.
3. **Qualify completion, errors and recovery.** The default fixed workload
   polls completion once per worker tick. The single-core interrupt profile
   wakes the worker from interrupt 97 on CPU0, and its runtime client receives
   completion through a held WAIT reply. The corrected single-core USB path can report a terminal NPU
   failure only while the transport remains valid. Physical delivery remains
   untested. Keep buffers and hardware resources on timeout or fault until the
   reviewed recovery condition is met. Interrupt-driven completion, cancellation,
   reset, stale interrupts, repeated loads, concurrent CPU access and power
   transitions remain required hardware coverage.
4. **Extend admission beyond the trusted catalogue.** Concurrent models from
   the [session catalogue](npu-session.md) are implemented, including
   multi-tensor graphs and compiled fully connected layers, and are checked
   only in emulation. Uncatalogued model uploads, runtime model switching and
   tensor-processor, shader or software operations in general graphs are
   unsupported. Establish complete descriptor access extents, command
   permissions, operator semantics and output padding for each added profile.
   Reuse published source and representative compiler output; inspect binary
   loader references where source is insufficient.
5. **Broaden application inference after the fixed path works.** The current
   service already provides model/session lifetime, copied tensor input and
   output, asynchronous jobs, deadlines and explicit result reaping. Wider
   capability queries, limits and concurrent-client behavior need an explicit
   supported contract and hardware evidence. Required NPU behavior must work
   independently of mimus.
6. **Evaluate mimus separately.** Map compatible operations and shapes while
   preserving the existing model. The [linear-layer study](npu-mimus-linear.md)
   compiles mimus layers from their int8 codes and per-row scales; the
   simulator matches one rounding of the exact result to INT16, not the mimus
   float32 output, and the compiler refuses the INT32 output an exact CPU
   read-out would need. After hardware functionality, measure transfer
   and scheduling overhead, numerical differences, recognition accuracy, latency
   and resource use. Explicit CPU fallback for unsupported operations does not
   establish full NPU support. The native compiler/toolkit under issue 12 remains
   deferred as described above.

A first native launch has been attempted, but no captured result establishes NPU
execution. Physical completion with copied numerical output is still the gating
result for this application. [Issue 9](https://github.com/pedapudi/sunstoneos/issues/9)
retains both NPU and video-codec acceptance requirements.
