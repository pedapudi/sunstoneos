# Prepare finite NPU computation and normal ROM return

This single-core image runs two jobs using its immutable model and input,
compares both outputs, reaps them, closes the NPU service and gates its local
clocks and asserts engine resets. It parks the power service only after requests,
borrows and mutexes are idle. The monitor then restores interrupts and returns
to ROM. The shared NPU SRAM, domain 4, PLL and rail remain powered and owned
through that return. No power-off or in-place NPU restart is established.

The host saves the entire 18 MiB interval `0x41000000..0x42200000` before
uploading. It restores and compares that interval only after a responsive new
ROM instance supplies matching completion reports and copied numerical output.
The native limit is 20 seconds; ROM discovery is bounded by 30 seconds.
A failed or uncertain attempt retains ownership for physical recovery.

The SMP USB thermal image is a separate consumer. Its local CLOSE leaves
USB active until its 20-second deadline and supplies no normal ROM return.
The ordinary numerical image has no thermal admission or sensor-stop integration.
Numerical return qualification therefore cannot establish thermal behavior.

The monitor report uses the NPU-specific magic `0x415337334e505231`.
The host check executes the linked monitor initializer and feeds its report
identity into the modeled completion parser. It refuses the generic cached-SMP
magic. Contract admission, time and later completion fields are modeled in
that check; it establishes no physical entry or numerical result.

## Construct the session from explicit physical inputs

Use the [prerequisite capture specification](CAPTURE.md) to obtain current
clock, power and pin observations and the separate PMIC supply evidence.

The `session` operation reads the native layout and build identity from the
ELF and serializes the supplied typed boot bytes with fresh nonzero session
identities. It validates the wire record before writing. Optional `--admission`
configuration can supply epoch, generation, lease, supervisor and whole-platform
identities. Optional `--manifest` metadata does not select the native profile.
The producer refuses an existing output file and prints the exact session path.

The compiled session layout is read from the image. For the current producer,
the session has 456 bytes and the typed boot payload has 232 bytes. Within
that payload, the nine profile identities occupy bytes 0–71, fabric words and
rates occupy 72–95, PMIC configuration occupies 96–127, and the pin handoff
occupies 128–231. The layouts are defined by `a733_npu_application_boot`,
`a733_pmic_config` and `a733_pin_handoff`; preserve zero padding and the zero
uploaded PMIC deadline. Supply the actual inherited PL0/PL1 settings and
explicit pin authority. A software fixture is not a physical boot payload.

```sh
~/.cache/sunstoneos/venv/bin/python -B apps/npu-finite/host.py session \
  --image IMAGE --boot TYPED_BOOT --output SESSION
~/.cache/sunstoneos/venv/bin/python -B apps/npu-finite/host.py prepare \
  --image IMAGE --session SESSION --output BUNDLE \
  --clang CLANG --lld LLD --objcopy OBJCOPY
~/.cache/sunstoneos/venv/bin/python -B apps/npu-finite/host.py validate --bundle BUNDLE
```

Physical execution requires exclusive DDR and shared-resource ownership,
secondary CPUs OFF, correct cache mappings, independent DRAM clocks, NPU SRAM
exclusion and supported PMIC pin and 800 mV DCDC2 state. Native guards enforce
their configuration and hardware conditions. Packaging validates supplied bytes;
it does not establish those physical conditions.

## Verify the actual forced-ROM entry

The linked return tail writes RVBAR `0x20` and RMR `2`. That ROM entry sets
`r6` to `0x5c` and branches through `0x22c` to `0x2514`, bypassing the normal
boot-input branch at `0x10134`. The normal boot GPIO is therefore not a
precondition for this return.

After protected preload has saved SRAM scratch, the host reads five pinned ROM
instructions in one `readl` batch. Addresses `0x20`, `0x24`, `0x28`, `0x3c`
and `0x40` must contain `0xeb00008f`, `0xe3a00911`, `0xe3a0193d`,
`0xe3a0605c` and `0xea00004d`. Any mismatch refuses execution. The same
instance, port, disabled watchdog and native session checks still apply. `readl`
uses 48 bytes of scratch at `0x71000`; raw FEL reads of ROM are not a substitute.
The source-backed ROM capture is preserved in
`~/.cache/sunstoneos/evidence/gpu-rom-entry-20261024/snapshot/manifest.json`.

## Execute through the protected returning transport

The coordinator selects the physical port explicitly and uses the current host
adapter with the prepared transfer bytes:

```sh
SUNSTONE_USB_PORT=3-3 ~/.cache/sunstoneos/venv/bin/python -B \
  apps/npu-finite/host.py run --bundle BUNDLE --fel FEL --evidence RESULT
```

The shared runner holds the port lock, validates the live ROM instance and uses
the USB transition guard. Before snapshots or uploads, it tests and restores one
reserved DRAM word. An uncertain probe restoration retains ownership. Existing
result directories receive unique timestamped children.

Stop at the first transport failure. A deadline does not permit another request,
memory restoration or lock release. After normal return, old SRAM belongs to ROM
and is never restored. Another NPU start still requires the startup contract,
including a disabled PLL; successful return does not itself satisfy that contract.

## Return before constructing providers

`build.py --entry-checkpoint --evidence <directory>` produces
`npu-entry-mbus-checkpoint-v1`. It uses the finite image's authenticated
session, CPU/GIC entry, write-back mappings and memory reservation. It constructs
no task or provider. The linker removes the NPU, PMIC and scheduler startup
functions. The historical `npu-entry-memory-checkpoint-v1` profile remains
accepted for its original frozen images.

The EL3 monitor records the MBUS control word before and after its existing
source restoration, together with the restore result. The source restoration
can normalize the ROM's 400 MHz state to 600 MHz through the existing SRAM
window. It first checks the bridge and bus-master guards. Results 8, 9 and 10
leave the bus unchanged when a master is clocked or busy. The checkpoint can
return those results because it starts no device. Results 1 through 7 still
retain the image before EL1; they do not authorize a diagnostic return.

The checkpoint log includes `ENTRY_MEMORY_READY`, zero jobs and a mandatory
`NPU_MBUS` block containing the three captured values. An immutable marker binds
that log format to the image. The host reports `functional_success=false` and
the actual before/result/after values. A zero restore result alone does not
establish 600 MHz: the helper can also leave an unsupported incoming state
unchanged. Interpret the captured word with the source and divider fields.

Before return, the checkpoint requires zero native application and job-result
storage, an initialized empty write-back pool, and no non-cacheable pool. Its
published identities and MBUS record must match the secure monitor's admitted
state. The monitor accepts this pre-scheduler caller only in EL1h. It uses the
existing bounded cache publication, GIC restoration and forced ROM return.
The checkpoint omits the numerical image's SRAM-routing restoration entirely:
it never changed that routing, and other routing writers must remain excluded.

The same `host.py session`, `prepare`, `validate` and `run` operations support
the diagnostic. Native immutable markers select its parser and layout. The
session and receipt sizes stay 456 and 88 bytes. Typed boot fields retain their
syntax and grant no provider operation. The CPU, cache, DDR, secondary-OFF, ROM
transport and remote-writer conditions still apply.

The diagnostic performs no NPU-engine, PLL factor/gate, PCK policy or SRAM-routing
write. Its existing MBUS helper reads NPU/GPU clock state and conditionally reads
DMA0/CE idle state before changing only MBUS and its MSI bridge. The shared
monitor also performs the existing vendor security-permission setup before
EL1; those permission writes are part of entry, not NPU initialization. Native
USB mappings, PIO takeover and IRQ188/189/190 state are unnecessary because the
checkpoint constructs no USB provider. ROM USB clock, power, supply and writer
exclusion authority still applies.

This change adds a 32-byte immutable marker, a 32-byte retained record, two CCU
reads at entry and three printed values. It adds no task, allocation, queue or
per-inference operation. The selected build still cleans 35,840 cache lines.
Nineteen native checkpoint cases cover publication, normalization, a busy-master
skip and retained corruption; selected host cases reject missing, partial,
invalid and duplicate diagnostic blocks. The historical checkpoint still passes
its selected host preparation and nominal-return cases. Hardware effects remain
modeled. Evidence is under
`~/.cache/sunstoneos/evidence/sol-npu-entry-diagnostic-1029/`.

The physically accepted checkpoint on port 3-1 used image `9afb6738…` and the
same 1,804-byte ARM32 launcher `3a754016…` as the failed normal-restart capsule.
That checkpoint has neither the current MBUS SRAM-window symbol nor IRQ97
installation. The failed capsule adds those entry paths and later provider
execution. The new diagnostic exercises the current shared entry and MBUS
normalization, then returns before providers or IRQ97 installation. Its result
cannot qualify the failed capsule's remaining paths.

An early contract, architectural, secondary, GIC or MBUS failure still parks.
No safe normal-return proof exists for those refusal states, so the diagnostic
does not relax their retention. No diagnostic is available before successful
checkpoint entry. A missing return therefore remains ambiguous and requires
physical recovery; it does not authorize another launch.

### Return after IRQ97 installation

`build.py --entry-irq-checkpoint --evidence DIRECTORY` builds the distinct
`npu-entry-irq-checkpoint-v1` profile. It adds the existing private EL3 monitor's
IRQ97 capture and installation to the provider-free MBUS checkpoint. The base
NPU interrupt application stays disabled. Only the route helper is added;
there is no ISR entry point, task, device provider or NPU engine access.

Before normal return, the monitor requires protected snapshot phase 2,
CPU0 affinity, Group1NS, level trigger and priority `0xd8` for IRQ97. It also
requires the source disabled, inactive and pending-clear, with the deadline
checked after all field reads. The existing restoration writes the captured
route fields back and verifies them before restoring the shared GIC state.
Capture, install, authentication or restoration failure retains the image.
IRQ97 is never enabled or cleared; the shared monitor keeps its existing timer
PPI handling. No early guard becomes a normal-return path.

This mode adds a 32-byte immutable profile marker and the existing protected
route snapshot, with five owned-field writes during installation and five
during restoration. It adds no task, allocation, DMA buffer or output copy.
Existing startup and return deadlines bound the register polling. It uses the
same 456-byte session, 88-byte receipt, ENTRY_MEMORY_READY and MBUS log.
The host selects this profile from its immutable IRQ checkpoint marker and
checks its linked route implementation directly.

The focused native check uses `checkpoint-check.py --case irq` and executes
actual session validation, shared entry, pool preparation, checkpoint publication,
installed-state authentication, captured-route restoration and host parsing.
It covers inherited enabled/pending/active sources, a failed install readback,
corrupted installed fields, a deadline reached after field reads and a failed
restore readback. Existing generic checkpoint corruption and MBUS-normalization
cases retain their earlier evidence. GIC effects and privileged registers are
modeled; physical IRQ installation and return remain unqualified for this image.
This checkpoint does not authorize a full NPU startup retry or establish the
cause of the earlier failed restart.

## Finite thermal admission bounds work and cleanup

`build.py --thermal --evidence DIRECTORY` selects the optional
`npu-finite-thermal-return-v1` profile. The existing bootstrap task owns the native
thermal sensor controller (THS) and fixed board policy. All five channels must
produce individually fresh NORMAL readings within one second before startup.
Hot, critical or stale readings close admission permanently. Every submission
checks freshness before and after its atomic reservation. A first refusal starts
a fixed five-second drain bound, limited by the session deadline. The client
independently detects a stalled bootstrap, so stale monitoring cannot admit work.

An accepted startup finishes through OPEN and CLOSE; an accepted job completes
and is reaped before CLOSE. A `NEVER_STARTED` result proves that startup was
not polled, worker admission stayed clear, startup tokens are absent and the
power-domain row was never requested. It does not manufacture CLOSED or STOPPED.
Configured THS disable and quiet IRQ71 precede suspension of the bootstrap and
worker tasks, followed by PMIC parking. The first refusal deadline also bounds
parking, cache publication, secondary-state checks and GIC restoration.

The polling configuration keeps all peripheral interrupt enables zero. Admission
checks the monitor's saved incoming IRQ71 enable state and its current pending
and active bits before THS writes. Cleanup checks IRQ71 again after GIC
restoration. Disabled-converter readback proves the configured stop only; analog
conversion drain and release of inherited clocks remain unproved.

The version-3 session is 616 bytes, with a 392-byte boot payload. Its consumed
160-byte thermal suffix supplies a distinct owner, authority, four native digest
fields, APB1 selection/rate and the GPADC configuration choice. Exact image layout
and immutable profile markers reject cross-profile, truncated and extra records.
The thermal owner must differ from all four existing provider owners. THS
operation still requires correct electrical, clock, SID and exclusion conditions.
Supervision covers matched CLOSE/local stop or proved NEVER_STARTED, followed by
configured THS stop and provider parking. It grants no clock release.

A distinct 112-byte receipt records completed jobs, the first refusal reason and
the fixed cleanup deadline in 24 MHz counter ticks. Status 0 requires two matching
jobs. Status 1 preserves a numerical mismatch even when thermal refusal follows.
Status 2 means a safely cleaned thermal refusal after zero or one matching job.
Sensor or ownership faults cannot produce a normal-return receipt. The original
numerical and checkpoint receipts remain 88 bytes. After validated reports and
full DRAM restoration, nonzero normal outcomes also carry the existing kernel
controlled-outcome fields for ROM continuity. Their failed numerical or refused
work status remains unchanged; restore failures publish no controlled outcome.

The thermal image adds no task or stack. Compared with the selected numerical
image, application storage grows by 640 bytes and the receipt by 24 bytes. The
first measured build adds 17,656 code bytes and 384 read-only data bytes. Normal
monitoring runs every 25 ms. A modeled five-channel sample executes 19 MMIO reads,
one status-clear write and 39 counter queries; a successful work reservation uses
two counter queries. Startup and initial admission retain one-tick waits.

Evidence in `npu-finite-thermal-20261024` covers linked native THS operations,
policy, claims, caller cleanup, native receipt publication and host parsing.
Registers, scheduling, coherence and engine completion remain modeled at their
stated boundaries. Physical thermal qualification remains outstanding. The
operating point stays 492 MHz at 800 mV; PLL, domain, rail and clock leases remain
retained, with no in-place restart or speculative voltage transition.

## Finite numerical execution leaves USB to ROM

The numerical and entry-checkpoint profiles construct no native USB provider.
The finite diagnostic excludes observer-service startup, and single-core provider
initialization excludes USB clock acquisition. The host rejects a linked native
USB function in either profile; filename symbols left by the linker are harmless.
Consequently, finite admission does not require native USB mappings, PIO takeover
or disabled IRQ188/189/190. The monitor preserves inherited SPI enables and
restores them before ROM return. ROM USB clock, power, supply and writer-exclusion
authority remains required, as do all numerical NPU and PMIC operating conditions.

## A numerical failure can preserve ROM continuity

`continuity.py --bundle BUNDLE --session SESSION --output NEW_RECORD` is an
offline producer for a completed numerical failure. It uses the current host
validator, checks captured monitor, platform and receipt bytes,
verifies upload readbacks and compares the full 18 MiB DRAM backup with its
restoration. Only status 1 with a validated normal return and unchanged failed
numerical result can produce the existing kernel controlled-outcome record.

The new record references the original session and all consumed inputs by hash.
It preserves `functional_success=false`, never edits the original session and
preserves an existing output path in a unique directory. It records the
validated return and restoration for inspection. Later runs use live checks
instead of requiring this derived record. PLL, domain and rail retention still apply.

## Preserve stopped buffers before restoring a numerical failure

The host captures returned RAM for a validated status-1 numerical failure when
the ELF symbols, allocation intervals and immutable slot table match the supported
capture layout. A compatible build can have a different image hash. Thermal and
checkpoint profiles never capture.
Three fixed metadata reads total 440 bytes. They validate the six buffer slots,
stream lengths, parent extent and translation-table allocation against the
audited ELF layout. Only then does the host read the 1,085,440-byte parent
reservation, including command padding and translation tables. A fifth,
read-only read then copies the 101,376 bytes of the NPU SRAM CPU alias that the
12×12 model uses, `0x74000..0x8cc00`, ending below the ROM scratchpad at
`0xee800`. The image never powers domain 4 or the SRAM off, so the read targets
powered SRAM. Whether the CPU alias returns the NPU's contents in that retained
state is not established; the read runs last, after the DRAM evidence is saved.
All five addresses and lengths come from the image-bound plan; captured
pointers never authorize reads. Every DRAM range lies within admitted DRAM and
the native published interval.

Each read is synchronized to disk and journaled with its address, length and
hash before the next read. Metadata differences, short reads, transport failures
or persistence failures retain the session without restoration. Payload and
returned-output differences remain diagnostic evidence. The host restores DRAM
and publishes the controlled outcome only after capture completes; the numerical
result remains failed. Successful numerical runs, unsupported native layouts, checkpoints and thermal
profiles perform no additional reads.

This adds five ROM reads totaling 1,187,256 bytes and no native instructions.
The helper checks the native layout and bounded physical ranges directly. A
future attempt still requires recovered hardware and exclusive resource ownership.
The earlier physical capture has already been restored and is unchanged. The
focused modeled evidence is in `npu-stopped-capture-20261024`; numerical
correctness and physical buffer contents remain unresolved.

## Compare stopped RAM with the accepted prepared artifacts

`analyze-capture.py` reads an existing numerical bundle, its session and raw
reports, and the four DRAM stopped-capture files, and journal-checks the SRAM
file when the capture includes it. It validates their internal report
consistency and journal hashes, then compares the fixed allocation contents.
The command takes `--bundle`, `--session`, `--assembly`, `--bootstrap` and an
`--output` JSON path. An existing file is preserved in a unique directory. It performs no device operations and grants no
execution, restart, continuity or recovery authority.

Use the accepted `npu-models-20261018/conv12/assembly-comparison` directory for
`--assembly` and `npu-convolution-20261016/fixtures/expected-bootstrap.bin` for
`--bootstrap`. The helper checks the supplied reference geometry, model identity and bootstrap
structure, then compares their bytes with the captured allocations. The assembly
comparison uses this layout's device addresses, so no compiler or assembler rerun
is needed. Tensor formats and affine quantization parameters
come from the immutable model and its existing catalogue entry. They describe
the model; they do not establish the cause of channel saturation.

The report separates descriptor and command changes, coefficient or input
changes, translation-table changes, and disagreement between stopped output
and the native copied result. It checks the complete 20 KiB table allocation,
including unused entries, permissions and padding. Address decoding stays
within the captured arena and known buffer ranges. Differences include counts
and bounded offset examples; malformed or missing evidence is reported as
incomplete. Original files remain unchanged.

Agreement across prepared buffers and copied output narrows the next inquiry
to execution-time visibility and hardware interpretation. Returned RAM does
not show what the engine fetched during a job. The nineteen decoder cases in
`npu-stopped-analysis-20261024` use explicitly synthetic parents and preserved
historical result bytes; they establish no new physical observation.

### Return after constructing the native providers

`build.py --entry-provider-checkpoint --evidence DIRECTORY` produces the distinct
`npu-entry-provider-checkpoint-v1` diagnostic. It runs the real
`a733_npu_application_init` after the accepted private IRQ97 installation. It
constructs the counter, clock registry, register/I2C mutexes, resource manager,
pin and rail ledgers, reserved NPU memory, PMIC and power-domain providers,
startup row, convolution service and IRQ binding. Four static tasks and the
service queues exist. The scheduler never starts.

The constructor reads the architectural counter and the installed GIC route.
It copies and authenticates the model, creates command buffers and page tables,
and fills the 512 KiB bootstrap buffer with END commands before writing its
bootstrap stream. These writes use the native reservation inside the saved
`0x41000000..0x42200000` interval. The linker confines the pool below
`0x41f00000`; task stacks, queues, ledgers and monitor storage also lie within
the saved interval. No NPU register, PMIC bus, power-domain, PLL or SRAM-routing
operation executes.

The return guard requires the never-started scheduler, initial startup stage,
zero startup clock/rail/PCK tokens, no row request or answer, an unopened
register window, initial PMIC state without requests or held resources, empty
platform lease ledgers and an inactive service with no job. Exactly one native
memory allocation remains owned by the constructed service. The guard checks
its owner, generation, extent and DEVICE state. That software ownership lasts
through receipt publication and ROM return; the host then restores and compares
the saved RAM. This diagnostic does not claim a device CLOSE, provider park or
hardware drain. A failed guard cannot produce a normal-return receipt.

The immutable provider marker prevents relabeling this image as a provider-free
checkpoint. Its platform conditions require
`providers-constructed-never-scheduled`, and its power conditions require that
condition plus `no-provider-hardware-operation-or-lease`. The typed boot record
contains valid constructor configuration, including pin syntax; those values
grant no provider hardware operation. IRQ97 remains disabled, inactive and
pending-clear. The existing EL3 installed-state authentication and captured-route
restoration still gate return.

A successful returned log begins with
`NPU_CHECKPOINT=PROVIDERS_CONSTRUCTED; scheduler never started; jobs=0`, followed
by the existing MBUS diagnostic. The host reports `functional_success=false`.
This boundary tests the actual memory, task, queue and native-binding path that
follows IRQ installation. It establishes no scheduler startup, power sequencing,
IRQ delivery, inference or restart result.

The source audit uses the pinned vendor VIPLite driver and Arm's GIC-600 errata
notice SDEN-892601 v12.0, pages 12–14. Erratum 2384374's CLEAR case requires a
recalled SPI and a CPU interrupt-group disable. The finite monitor's return
clears distributor group enables; it does not clear a CPU group enable. Its
receipt also requires IRQ97 disabled, inactive and pending-clear. That audit
identifies no corresponding NPU return defect and does not explain the earlier
nonreturning restart. The vendor's effective initialization and power sequence
remain documented in [the reference audit](../../docs/reference-backsolve-npu.md)
and [startup contract](../../docs/npu-startup.md).

Focused software evidence resides in
`~/.cache/sunstoneos/evidence/sol-npu-diagnosis-1031/`. Linked instruction checks
execute the real constructors, receipt and monitor return. They model privileged
registers, GIC effects, translation and coherence admission. Host equivalents
supply libc byte operations and the existing convolution SHA-256 digest. They
check CPU write bounds and refuse mutated provider, scheduler, allocation and
IRQ state. Separate host cases cover the new image/admission profile and final
writer format. These checks do not establish physical operation.

The constructor diagnostic has a physical normal-return result on port 3-3.
Image `09ec8dd6e736c59f84038c4f28539b343c7c77dcb25cbd4752ccb59de23a906f`
reported stage 5, status 0 and zero jobs. MBUS normalization reported
`0x84000000` to `0x81000000`. The host restored and compared all saved 18 MiB
and retired ownership. The canonical record is
`~/.cache/sunstoneos/evidence/sol-resume-20261031/port-3-3/npu-provider-construction/session.json`.
This result establishes provider construction and return before scheduling.

### Return after controller bootstrap without inference

`build.py --startup-only --evidence DIRECTORY` produces
`npu-startup-return-v1`. This interrupt profile constructs the ordinary native
application and starts its real scheduler. The unchanged PMIC/power service,
clock/rail ownership, domain adoption, PLL/local clocks and resets, SRAM routing,
2 ms settle and service activation run before the client sends OPEN.

The client holds WAIT until the controller reports READY after its bootstrap
command. That path performs identity/reset checks, configures the NPU MMU,
publishes the initialization stream and requires one interrupt completion.
The client submits no inference. It sends ordinary CLOSE from READY, then waits
for the existing bootstrap task's local stop and the existing provider parking.
CLOSE accepts READY directly; no fabricated CLOSED state or separate retirement
API is used.

The final client gate requires the stopped interrupt receipt, exactly one
completion, a legitimately closed controller, no pending request or job, no
output-ready state and zero inference result counts/tickets. Existing controller
CLOSE checks full idle and clear events, disables event reporting and the MMU,
and verifies idle again. Existing local stop closes gates, asserts local resets
and routes SRAM back to the CPU. The existing finite path suspends workers and
parks the PMIC only after its request, ownership and mutex checks pass.
Domain, PLL and rail leases remain held at return. No domain OFF is requested.

The distinct immutable marker binds zero inference jobs and one interrupt.
A separate 88-byte receipt magic and monitor detail prevent relabeling the
result as numerical or restart completion. The ordered log ends with
`NPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0`. The host reports
`startup_bootstrap=true`, `jobs=0`, `interrupt_completions=1` and
`functional_success=false`; `jobs` counts inference jobs. Preparation and capture
use `apps/npu-interrupt/host.py`, including its existing hashed IRQ authority.
The ordinary finite host refuses this profile.

This diagnostic uses the original 20-second absolute deadline and 1-second OPEN
budget. It adds no task, queue or allocation. It uses the existing buffer pool,
cache-clean interval, EL3 return, forced-FEL path and complete 18 MiB host
restoration. It executes NPU register accesses and a real bootstrap stream;
a failed startup, bus stall, missing interrupt or uncertain retirement retains
ownership until manual whole-board power removal. Shared SRAM ownership still
covers the entire routing interval. There is no inference tensor SRAM workload.

Status: staged for later physical validation. Both boards are unavailable after
separate retained attempts. The staged package contains no current target
identity or provider grant. Execution requires recovered hardware and exclusive
clock, PMIC, rail, SRAM and IRQ ownership, with live ROM and DRAM checks before
launch. No reconnect or board operation was
performed for this implementation.

Evidence is under `~/.cache/sunstoneos/evidence/sol-npu-startup-1031/`.
The source audit compares the actual historical 852 MHz successful image
`2b4b5e42de395092b166b4e5ce40530be01c0e27aba773d37c33c4551ff5fc91`
with the current providers and the pinned effective VIPLite configuration.
It identifies no additional production defect or cause of the earlier nonreturn.
The controller source is unchanged. Focused linked checks execute the new
client, scalar gates, native log/receipt writer and EL3 writer through the real
host parser. Provider completion, scheduling and hardware effects are modeled.
A focused FreeRTOS composition case also executes the real service, controller
and memory providers through bootstrap READY and ordinary CLOSE without an
inference. IRQ delivery and device effects remain modeled. Separate affected
host cases reject profile relabeling, extra or absent completions, inference
claims and incorrect receipts or stages. These checks establish software
composition; physical startup, IRQ delivery and retirement remain unqualified
for this image.

### Return after one inference job

`build.py --single-inference --evidence DIRECTORY` produces
`npu-single-inference-return-v1`. It runs the startup sequence above, then
submits one job of the network linked into the image, waits for its completion
interrupt, copies the output and reaps the job before the ordinary CLOSE. The
default build links the `conv12` catalogue entry: one 3×3 convolution from a
1×3×12×12 UINT8 input to a 1×4×10×10 UINT8 output. The client compares the
copied output with the linked reference. Status 0 means the 400 bytes match;
status 1 is a normal return with a different output. Both statuses publish the
receipt and return to ROM, and the host reports the copied bytes.

The startup-only and single-inference profiles share one client source. Each has
its own receipt magic, monitor detail and immutable marker, so neither result
can be relabelled as the other. The final gate requires two interrupt
completions (bootstrap and job), one recorded job ticket and the same CLOSE,
local stop and provider parking as startup-only. The ordered log is:

```text
NPU_STAGE=WAIT_STARTUP, OPEN, BOOTSTRAP_READY, SUBMIT job=0x…01, REAPED,
CLOSE, WAIT_LOCAL_STOP; NPU_IRQ_COMPLETIONS=0x…02; NPU_IRQ_STOPPED=0x…01;
NPU_STAGE=QUIESCENT; NPU_OUTPUT= (one byte per line);
NPU_INFERENCE=MATCH or MISMATCH; inference_jobs=1
```

Each client stage calls `sunstone_crumb` from `crumb.h`. The call compiles to
nothing until a progress-record implementation defines it; a retained client
failure also records its reason. Use the same preparation and capture as
startup-only, with this profile in the manifest. Status: staged; no board
run, and no host check of the expected output.

### Return after one job of each of two networks on the NPU runtime

`build.py --runtime --evidence DIRECTORY` produces `npu-runtime-return-v1`.
The image replaces the fixed-model convolution object with the NPU runtime of
`drivers/npu/a733_npu_runtime.c`, which loads any admitted model into a slot
at run time. `apps/npu-convolution/native-runtime.c` keeps the providers, the
PMIC task, the startup and stop sequence and the quiescence proof of the
single-model image, and prepares one runtime slot in a 6 MiB parent region at
`0x41400000..0x41a00000`. The region holds the runtime's read-only,
read-window and write-window groups of 2 MiB each; `apps/npu-finite/link.ld`
places it after the stacks, inside the cleaned interval, and the image has no
device pool.

The client, `apps/npu-finite/runtime-client.c`, runs one job of each network
in `apps/npu-finite/runtime-models.json` through the generic job helper
`npu_job_run` (`include/npu_job.h`). The helper sends OPEN on the first call
and LOAD whenever the slot holds a different model; it copies the input,
submits and copies the output. The list names `conv12`, the baseline fixture
of the single-inference profile and the image's selected immutable model,
then `conv8`. The build checks every model, input and reference file against
its catalogue entry, generates the network table and the image's admitted
table from the same list, and keeps a linked copy of each reference
(`npu_finite_reference_0`, `npu_finite_reference_1`). Status 0 means both
copied outputs match their references (400 and 72 bytes); status 1 is a
normal return with a differing output, and the host reports both outputs.

The final gate requires three interrupt completions (bootstrap and two jobs),
two runtime jobs, a CLOSED slot with no request outstanding and the same local
stop and provider parking as startup-only. The ordered log is:

```text
NPU_STAGE=WAIT_STARTUP, JOB network=0x…00, JOB network=0x…01, CLOSE,
WAIT_LOCAL_STOP; NPU_IRQ_COMPLETIONS=0x…03; NPU_IRQ_STOPPED=0x…01;
NPU_STAGE=QUIESCENT; NPU_OUTPUT network=0x…00 and network=0x…01 (one byte
per line each); NPU_INFERENCE=MATCH or MISMATCH; inference_jobs=2
```

The runtime keeps the idle-register completion hint (`idle_hint` 1 in
`apps/npu-interrupt/irq.c`), as the board and OS images do, so a job also
completes when INTID 97 is late. While a job runs the NPU routes SRAM
`0x44000..0xf3fff` to itself. The image keeps no state there; a progress-record
implementation of `sunstone_crumb`, which the helper calls between SUBMIT and
its reply, must not write that range either. Use the
same preparation and capture as single-inference, with this profile in the
manifest. Status: software-checked by `host-check.py --interrupt` and
`checks/npu-runtime-bounds.py`; the startup, quiescence and SRAM-route
emulator checks of the single-model profiles do not cover it; no board run.
