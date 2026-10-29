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

Use the exact image and manifest together. The `session` operation takes an
already reviewed admission record and the complete typed boot bytes. It
serializes their identities and hashes, validates the result with the existing
admission predicate, and refuses an existing output file. It supplies no
ownership, clock, electrical or recovery defaults.

The compiled session layout is read from the image. For the current producer,
the session has 456 bytes and the typed boot payload has 232 bytes. Within
that payload, the nine profile identities occupy bytes 0–71, fabric words and
rates occupy 72–95, PMIC configuration occupies 96–127, and the pin handoff
occupies 128–231. The layouts are defined by `a733_npu_application_boot`,
`a733_pmic_config` and `a733_pin_handoff`; preserve zero padding and the zero
uploaded PMIC deadline. Supply the actual inherited PL0/PL1 settings and
explicit pin authority. A software fixture is not a physical boot payload.

```sh
python apps/npu-finite/host.py session --image IMAGE --manifest MANIFEST \
  --boot TYPED_BOOT --admission ADMISSION --output SESSION
python apps/npu-finite/host.py prepare --image IMAGE --manifest MANIFEST \
  --session SESSION --admission ADMISSION --output BUNDLE \
  --clang CLANG --lld LLD --objcopy OBJCOPY
python apps/npu-finite/host.py validate --bundle BUNDLE
```

The admission must bind the current target and continuity, the complete DDR
reservation, secondary CPUs OFF, cache and mapping authority, independent
DRAM clocks, NPU SRAM exclusion and drain ownership, PMIC pin ownership and
stable DCDC2 at 800 mV. The separate returning-lifetime review binds the exact
image, launcher and host sources. It authorizes retirement only after verified
CLOSE, local stop, provider quiescence and ROM return. The power domain's
coherent ON observation does not supply any of those authorities.

## Verify the actual forced-ROM entry

The linked return tail writes RVBAR `0x20` and RMR `2`. That ROM entry sets
`r6` to `0x5c` and branches through `0x22c` to `0x2514`, bypassing the normal
boot-input branch at `0x10134`. The normal boot GPIO is therefore not a
precondition for this return.

After protected preload has saved SRAM scratch, the host reads five pinned ROM
instructions in one `readl` batch. Addresses `0x20`, `0x24`, `0x28`, `0x3c`
and `0x40` must contain `0xeb00008f`, `0xe3a00911`, `0xe3a0193d`,
`0xe3a0605c` and `0xea00004d`. Any mismatch refuses execution. The same
instance, port, disabled watchdog and admission checks still apply. `readl`
uses 48 bytes of scratch at `0x71000`; raw FEL reads of ROM are not a substitute.
The source-backed ROM capture is preserved in
`~/.cache/sunstoneos/evidence/gpu-rom-entry-20261024/snapshot/manifest.json`.

## Freeze before the coordinator claims the board

Preserve every `HOST_SOURCES` entry, all runtime imports and the external USB
guard with the image, manifest, actual launcher and complete native compiler
input closure. Build and import the frozen producer offline. Supply current
DDR and survey evidence plus every accepted warm return in the continuity
chain. The run command uses the existing root-owned claim and never creates
or closes queue records:

```sh
python BUNDLE/host/apps/npu-finite/host.py run --bundle BUNDLE --fel FEL \
  --evidence RESULT --ddr-evidence DDR --platform-survey SURVEY \
  --warm-return ACCEPTED_RETURN --root-owned-claim CLAIM
```

Stop at the first transport failure. A deadline does not permit another
request, memory restoration or claim release. After normal return, old SRAM
belongs to ROM and is never restored. Another NPU start still requires the
startup contract, including a disabled PLL; successful return does not itself
satisfy that contract.

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
it never changed that routing, and admission requires fresh ROM lineage plus
exclusion of other routing writers.

The same `host.py session`, `prepare`, `validate` and `run` operations consume
the diagnostic manifest. The session and receipt sizes stay 456 and 88 bytes.
The existing `a733-npu-checkpoint-platform-v1` schema requires
`providers-never-constructed` and
`read-only-NPU-clock-guard-before-MBUS-normalization`. Its clock record also
requires `MBUS-source-restore-with-idle-masters-and-exclusive-SRAM-window`.
Power admission requires `providers-never-constructed` and
`unconsumed-typed-boot-no-provider-grant`. Typed boot fields retain their syntax
and grant no provider operation. Current CPU, cache, DDR, secondary-OFF, ROM
transport and remote-writer exclusion evidence remain necessary.

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
Host admission additionally requires `exclusive-IRQ97-route-capture-install-restore`;
the old MBUS and memory profiles cannot label this image.

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
160-byte thermal suffix supplies a distinct owner, authority, four evidence
hashes, APB1 selection/rate and the GPADC configuration choice. Exact image layout
and immutable profile markers reject cross-profile, truncated and extra records.
The thermal owner must differ from all four existing provider owners. Physical
records retain the existing THS electrical, clock, SID and exclusion requirements.
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
offline producer for a completed numerical failure. It reuses the bundle's
frozen host validator, checks captured monitor, platform and receipt bytes,
verifies upload readbacks and compares the full 18 MiB DRAM backup with its
restoration. Only status 1 with a validated normal return and unchanged failed
numerical result can produce the existing kernel controlled-outcome record.

The new record references the original session and all consumed inputs by hash.
It preserves `functional_success=false`, never edits the original session and
refuses an existing output path. This establishes ROM continuity for later
admission; it does not authorize an NPU restart, close a queue claim or grant
peripheral ownership. PLL, domain and rail retention still apply.

## Preserve stopped buffers before restoring a numerical failure

The host captures returned RAM for a validated status-1 numerical failure from
image `9e4adabf7c8fb19c2cfc2fb4260971cc32a3b6e5ee0fa09bcad0d4b4208c0e95`. The
capture is bound to that non-thermal numerical image, which runs without
thermal supervision; the thermal and checkpoint profiles never capture.
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
result remains failed. Successful numerical runs, other images, checkpoints and
thermal profiles perform no additional reads.

This adds five ROM reads totaling 1,187,256 bytes and no native instructions.
The helper is part of the frozen host source closure. A future attempt requires
a newly reviewed host bundle and operating admission in a permitted lifetime.
The earlier physical capture has already been restored and is unchanged. The
focused modeled evidence is in `npu-stopped-capture-20261024`; numerical
correctness and physical buffer contents remain unresolved.

## Compare stopped RAM with the accepted prepared artifacts

`analyze-capture.py` reads an existing numerical bundle, its session and raw
reports, and the four DRAM stopped-capture files, and journal-checks the SRAM
file when the capture includes it. It validates their internal report
consistency and journal hashes, then compares the fixed allocation contents.
The command takes `--bundle`, `--session`, `--assembly`, `--bootstrap` and an
exclusive `--output` JSON path. It performs no device operations and grants no
execution, restart, continuity or recovery authority.

Use the accepted `npu-models-20261018/conv12/assembly-comparison` directory for
`--assembly` and `npu-convolution-20261016/fixtures/expected-bootstrap.bin` for
`--bootstrap`. Their hashes are pinned by the helper. The accepted assembly
comparison already uses this image's device addresses, so no compiler or
assembler rerun is needed. Tensor formats and affine quantization parameters
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
