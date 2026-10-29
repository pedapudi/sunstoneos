# Recorded CPU recognition with successful ROM return

Status: implemented and software-tested. Physical recognition, normal return,
cleanup timing and subsequent rail restoration remain unqualified.

The optional `SUNSTONE_A733_OS_CPU_RETURN` profile runs one recorded recognition
with eight CPU workers, the PMIC provider and the thermal service. It requires
supervised staged workload 2, Cortex-A76 admission and CPU clock grants for both
clusters. The optional `GPU_LINEAR` configuration also supports this return;
its [final parking sequence](../../docs/designs/gpu-normal-return.md) precedes
CPU cleanup. NPU, radio, microphone, headset and performance-mode services are
excluded. Existing supervised and resident profiles keep their existing lifetime.

The first functional attempt may use the dedicated
`a733-os-cpu-return-clock-under-test-v1` approval. It asserts no passed B1
result. Other profiles retain the existing passed-B1 requirement. This original
under-test schema still requires historical steps 1 through 6 on the same
physical board. Its semantics and frozen evidence are unchanged.

The dedicated `a733-os-cpu-return-clock-compatibility-under-test-v1` alternative
reuses historical steps as mechanism evidence through an exact compatibility
review. Historical records keep the actual source board identity, port, date,
SID and evidence. The review binds the current target, image, session and source
pins, and reviews SoC/CPU implementation and revision, board topology and
regulator wiring applicability. It asserts no shared physical identity and no
target-device qualification. Current electrical, thermal, security, continuity
and writer exclusions remain mandatory. Target regulator transients, runtime
DVFS, recognition/thermal/IRQ integration and return/restoration remain under
test. Both under-test schemas preserve exact root-claim and helper-hash binding.

The distinction follows the old binder's actual port 3-1 refusal: its bin,
oscillator and supplies had not been read (`docs/designs/cpu-top-opp.md:531`).
The native guard now checks those current configuration predicates. The earlier
same-board multicore and integrated B1 observations remain separate
qualification (`docs/designs/cpu-a76-dvfs.md:325–363`). A PMIC snapshot proves
programmed voltage; it does not measure analog settling on the new target.

Before the first requested transition, the sole CPU clock owner completes
read-only inspections of both clusters and their enabled, decoded supplies.
The profile requires both supplies at 800 mV, no intervening transition, bin 4,
a valid 26 MHz oscillator, all three exact boot PLL/mux pairs and disabled
pattern/LFM controls. The thermal driver must have completed calibration and
startup without fault, and all five policy channels must have fresh valid
NORMAL readings under the 70 °C CPU critical policy. Missing first readings
wait; an invalid initial state retains the session. The entry review excludes
all competing native and external writers. These configured predicates are
checked by native code before any voltage or frequency request; they are not
prior captured measurements or physical qualification.

The under-test approval binds the image, build, session identities, fixed
inference configuration, exact thermal configuration and evidence, grant,
source obligations, historical qualification, root claim and exact rail helper.
To avoid a hash cycle, its template hash zeros only the grant approval field
at bytes 872 through 903 of this exact 904-byte session. The completed return
review then binds the full template and approval hash. The host refuses a
claim that differs from the reviewed claim and preserves the approved helper
hashes in the successful return receipt. Follow-up requires those exact ELF,
binary and raw manifest hashes.

The dedicated USB0 task publishes staged readiness and suspends before native
USB startup. The constructor chain creates metadata and PMIC bindings without
accessing the USB controller. The linked check rejects native USB startup
symbols; close also requires zero USB steps, preparation, writes, tokens and
requests. The inherited ROM USB state remains untouched until forced-FEL return.
The host collects the transcript through the returned ROM, after recognition.

Recognition must finish at least five seconds before the session deadline.
The recognizer requires no outstanding parallel or asynchronous job, sends one
stop job to each inference worker, receives seven explicit acknowledgments and
copies its transcript. Each acknowledged worker suspends without another job.
The recognizer then publishes one completion and suspends.

The thermal owner keeps its existing safety policy while completing CPU-B's
request for 1014 MHz. It suppresses automatic restoration to higher clocks during
close. The driver can lower DCDC3 to its recorded voltage floor after lowering
frequency. DCDC5 continues supplying CPU-L and the DSU until ROM lowers their
clocks. At least four grant changes cover three initial requests and one close
request. Thermal transitions can consume the remaining grant; exhaustion retains
the session rather than claiming successful cleanup.

After clock completion and lease release, the thermal owner masks its SPI,
waits for distributor write propagation and stops the converter and local
interrupt enables. Its owner acknowledgment follows the driver's STOPPED result.
The converter may still have an analog conversion in flight; its stopped local
interrupts cannot access application memory. THS clocks and configuration remain
owned and powered. Later peripheral admission must account for this state.

Only then does the supervisor request PMIC parking. The provider acknowledges
idle request slots, absent rail borrows, no retained fault and unheld register
and I2C mutexes. Eight terminal tasks remain blocked throughout recognition.
The supervisor runs on CPU0 and notifies its own terminal last, so it cannot
be preempted before notifying the other CPUs.

The monitor snapshots THS interrupt fields before the IRQ table's first write.
The per-core terminals reuse the success SVC/SMC provenance checks and Secure
Device cleanup stacks from the finite CPU qualification image. Every CPU stops
its timers and interrupt delivery, reaches the writer barrier and drains
SGI0/PPI30. The GIC-600 erratum 2384374 workaround toggles DPG bits after group
disable and before redistributor sleep; THS SPI delivery prevents using the
older timer-only erratum exclusion.

Each CPU cleans writable image RAM through `__heap_end` and the 16 MiB
recognizer arena at 0x46000000 through the validated Nonsecure write-back mapping.
No Normal-memory writer executes after the final barrier. Permitted architectural
controls are restored while enabled SRE bits remain set. CPU0 requires matching
secondary acknowledgments and independently observed PPU OFF before restoring
THS interrupt fields and shared GIC state. RVBAR 0x20 and RMR_EL3 2 then enter
the reviewed forced-FEL ROM path. ROM resets CPU-L and DSU clocks; it does not
restore CPU-B.

A successful host capture establishes RAM continuity. It checks every CPU's
capture, drain, clean, restore and OFF records and the copied transcript's session
identity before restoring backed-up DRAM. SRAM belongs to the returned ROM and
is never restored. The receipt sets `cpu_rail_restoration_pending=true` and
`ownership_release_permitted=false`. The coordinator retains the same queue
claim and admits no unrelated successor.

The dedicated bundle33 helper variant requires DCDC3 at 800 mV and DCDC5 at
1000 mV. It checks both supplies, all three boot PLL/mux pairs and disabled
pattern/LFM controls before any selector write. DCDC3 remains unchanged. The
existing rail helper holds CPU-L and DSU clocks, rechecks their words before
submission, lowers DCDC5 to 800 mV, waits at least 1 ms and verifies readback.
Other entry voltages refuse; this variant does not restore arbitrary inherited
rail states. The original bundle33 entry at DCDC3 1050 mV remains unchanged.

The claim remains retained until the helper succeeds, its normal ROM return
completes, and the coordinator records the resulting clock and rail state.
The existing helper runner overwrites its private RAM; it does not restore that
RAM's previous contents. CPU recognition capture has a separate explicit DRAM
restoration path. Subsequent devices still require their own authority. A helper refusal
keeps the claim retained. In the recorded OS, a missed acknowledgment, expired
bound, source/readback mismatch, interrupt drain failure or architectural fault
keeps the existing halt and retention behavior. This
profile does not provide recovery from a stopped CPU, interconnect or terminal
thermal fault.

The inference fast path receives no extra recurring operations. The thermal
owner adds three atomic close-state reads per existing pass, two in its clock
step. The existing supervisor checks completion every 20 ms. Normal
completion adds seven stop jobs and seven acknowledgments, owner acknowledgments,
eight terminal notifications and one clean of each owned cached range per CPU.
There is no recurring terminal-task polling and no native USB detach interval.
The full native close must fit the fixed five-second reserve; physical timing
remains unmeasured.

The source comparison uses cached Arm IHI 0069G sections 4.1, 11.1 and 12.1.6,
GIC-600 SDEN-892601 version 12.0 erratum 2384374, the pinned TF-A
`cpu-continue/drivers/arm/gic/v3/gicv3_main.c` and `gic-x00.c`, and the Linux
boot contract in [the reference index](../../docs/reference-index.md). Existing
physical CPU timer-return evidence applies to its older exact images. Accepted
rail-helper checks remain limited to their recorded inputs.

Build the dedicated image with the existing configured dependency wrapper:

```sh
~/.cache/sunstoneos/bin/cfg.sh "$PWD" BUILD \
  -DSUNSTONE_A733_OS=ON -DSUNSTONE_MIMUS_WORKERS=8 \
  -DA733_A76_ADMITTED=ON -DSUNSTONE_A733_OS_CPU_CLOCK=ON \
  -DSUNSTONE_A733_OS_CPU_RETURN=ON
cmake --build BUILD --target sunstone-a733-os
```

The dedicated host adapter is `apps/os/cpu-return-host.py`. Its `prepare`
subcommand takes the image, linked manifest, reviewed admission, session template,
three recorded inputs, exact return review, compiler and linker. The ordinary
OS host adapter refuses this linked profile. The return review binds the exact
image, build identity, session, supervisor and source evidence. Board and thermal
parsers retain unconditional retention by default. The dedicated alternate
requires explicit failure-only retention and the successful-quiescence review.

The helper build command is:

```sh
~/.cache/sunstoneos/venv/bin/python apps/board-bundles-power/build-opp.py \
  --bundle 33 --cpu-return-dcdc3-800 --platform-build BUILD \
  --evidence HELPER_EVIDENCE
```

It emits `execution_variant=os-cpu-return-dcdc3-800-v1` and the immutable
`os_cpu_return_rail_profile` marker. Before helper launch, the coordinator checks
its exact ELF, binary, marker and retained successful return receipt:

```sh
~/.cache/sunstoneos/venv/bin/python apps/os/cpu-return-host.py check-rail-followup \
  --return-session RETURN_EVIDENCE/session.json \
  --image HELPER_EVIDENCE/build/probe.elf --binary HELPER_EVIDENCE/probe.bin \
  --manifest HELPER_EVIDENCE/input-identities.json --output RAIL_BINDING.json
```

This check admits only that conditional helper. It does not release the claim
or create device authority. The coordinator uses the existing reviewed returning
runner and board queue for execution.

Software evidence is under `~/.cache/sunstoneos/evidence/sol-cpu-normal-return-1028/`:

- `terminal/`: fourteen linked terminal, GIC, restoration and mapping cases.
  Cache maintenance uses contiguous modeled batches while executing each block's
  mapping and range checks. These checks do not establish physical cache effects.
- `services3/`: eight actual owner/coordinator cases, including THS RWP and PMIC
  deadlines. Completed CPU DVFS, thermal STOPPED and RTOS calls are modeled API
  boundaries. Actual masking, idle predicates and terminal notification order run.
- `workers/`: real eight-core FreeRTOS stop success and a blocked worker's bounded
  failure, using the actual parallel adapter.
- `preparation/`: the actual dedicated bundle producer with labeled modeled
  authority and a top-point grant. Only its failed layout/seal step was resumed.
- `host/`: thirteen preparation-binding, native-output and capture/restore cases.
  `helper-host/` adds exact helper admission and default-retention coverage.
- `rail-admission2/`: ten linked helper application cases that reach rail
  acquisition only after the expected supplies and every boot clock word match.
  Thermal/provider initialization and CPU inspection are modeled API boundaries.
- `rail-provider/`: the actual helper with the actual PMIC provider and ownership
  registries: one DCDC5 write, three helper requests, twenty clock reads and
  unchanged DCDC3. Bus registers, time and task yielding are modeled.

The initial terminal, worker and helper records above remain evidence for
their exact unchanged inputs. The image with the additional entry guard has
thirty linked guard cases in `entry-guard2/`, nine owner ordering and readiness
cases in `entry-owner/`, nineteen under-test parser cases in `entry-review/`,
one selected successful terminal case in `entry-terminal-success/`, and a new
production preparation/capture path in `entry-model/`, `entry-host/` and
`entry-host-helper/`. The authority in these preparation records is explicitly
modeled and cannot admit hardware. The helper integration checks all three
approved hashes and missing pins. No physical run is recorded.

The frozen OS ELF SHA-256 is
`fba722c5e33cbdeccc41b1b09a4a91752506e884c79e5f09f8c533a4cff74b70`.
The rail helper ELF SHA-256 is
`ae00ce1d4a1f82b77ab1e016dc1a08f795e56c9c6f4c92d166f6434d75590098`;
its binary SHA-256 is
`f48a6be72b336c8990a124fd608bba5fddb5c8617ec62eac5642b2d50fb27c75`.
Independent source review found no blocking defect. None of these software
records grants physical execution or qualifies normal return.
