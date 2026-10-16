# Bounded secondary CPU power sequencing

The [CPU startup controller](../platform/a733/cpu_start.c) implements the A733
secondary-core entry-address and power-request sequence. Host failure checks
pass, and its complete writes match isolated instructions from the published
board firmware for cores 1 through 7. The controller compiles for AArch64.
Physical power-on, secondary entry and multicore FreeRTOS remain unqualified.

The [interface](../include/a733_cpu_start.h) accepts one serialized privileged
boot owner, copied callbacks, a reserved entry address, a target core and an
absolute microsecond deadline. Each poll performs at most one register or
barrier operation, with monotonic time checks before and after it.

## Ownership precedes power requests

The platform must reserve the target CPU, its boot and power registers, stable
clocks and rails, interconnect configuration, and entry/stack/mailbox memory.
It must publish executable code and establish that remote firmware no longer
writes those resources. The nonzero lease identifies that existing reservation;
this component cannot authenticate or create it.

Only boot core 0 can initiate this sequence. The topology decoder uses MPIDR
affinity level 1 for the core index and requires the other affinity levels to
be zero. It accepts the architectural flag bits and refuses reserved bits or
an index outside 0 through 7. The board device tree describes six Cortex-A55
cores followed by two Cortex-A76 cores; physical MIDR identification remains
necessary before selecting per-core startup operations.

The entry must be four-byte aligned and lie in DRAM below 4 GiB. This is the
initial image-placement constraint. It does not describe the processor's full
address capability. The entry executes at reset privilege and must park without
changing the hotplug registers until the boot owner completes the power sequence.

## The controller verifies entry and request cleanup

For target index `n`, the sequence uses these register addresses:

| Register | Address | Relevant field |
| --- | --- | --- |
| Reset architecture | `0x08001000 + n * 0x1000` | Bit 0 selects AArch64 |
| Entry low/high words | Architecture address plus 4/8 | Low entry address; high word zero |
| Hotplug control | `0x07050200 + n * 4` | Bit 0 enables the request |
| Hotplug mode | `0x07050220 + n * 4` | Bit 0 requests power on |
| Power processing unit status | `0x07052008 + n * 0x1000` | Low four bits: OFF = 0, ON = 8 |

The controller requires OFF status and clear request bits before its first
write. It writes and verifies both entry words, sets and verifies AArch64 mode,
then executes a publication barrier. It sets the mode request followed by the
control request and polls for ON status.

After ON, it restores the original control and mode words, verifies them and
executes a final barrier. This preserves unrelated bits, including the GIC wakeup
disable bit. Restoring whole words requires the exclusive ownership and parked
secondary entry described above.

`POWERED` means the power processing unit reported ON and request cleanup
completed within the deadline. A separate secondary mailbox must prove entry,
exception-level setup, cache coherence, GIC redistributor readiness and eventual
FreeRTOS startup. The controller does not infer those results from power status.

An uncertain callback, invalid initial state, failed verification, backward clock
or expired deadline retains every reservation. A failed write may have taken
effect. The controller records attempted writes and performs no rollback, reset
or automatic retry. Terminal polls perform no callbacks. Successful power-on
also retains resources for the subsequent startup handoff.

## Published firmware corroborates the register sequence

The primary binary reference is the published board
[monitor firmware](https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/monitor.fex).
Its 78,609 bytes have SHA-256
`8af16b86ae8e2eff634a3ebca76a8cdf3c10b3540387573d46c3d785545b2632`.
The image remains outside the repository and is used only as a reference.

The instruction harness executes three isolated helpers at file offsets
`0xc4a0` for entry words, `0xc298` for AArch64 selection, and `0xc314` for the
power sequence. It supplies bounded register models and compares all seven
writes for each target core with native output. It maps only the helper image,
stack and permitted register regions, checks access widths, and caps execution
at 1,000 instructions per helper call.

The experimental
[TF-A A733 port](https://github.com/InfernoEmbedded/arm-trusted-firmware/tree/5bc9605cbfc175276920c3204658c025da3ae279/plat/allwinner/sun60i_a733)
provides readable corroboration. Its build selects the common A55 configuration,
then the NCAT2 power implementation with the NCAT3 register definitions.
The similarly named A53 implementation is not the selected path.
The port contains incomplete power-management functions; it is not evidence
of a complete supported platform. The published binary comparison independently
corroborates the specific helpers used here.

## Reusable evidence covers software sequencing

The changed behavior is secondary-core register programming. The principal
risks are incorrect per-core addresses, premature success and resource reuse
after a partial request. One sanitizer and instruction-reference batch passed:

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-cpu-start.py \
  --firmware ~/.cache/sunstoneos/sources/cpu-startup/monitor.fex \
  --evidence ~/.cache/sunstoneos/evidence/cpu-start-20261016
```

The native fixture covers every secondary target, initial-state refusal,
configuration copying, bounded polls, verification failures and terminal silence.
It injects uncertain access and late completion at every operation in the normal
sequence. It also checks a stuck power request and clock regression. The full
write trace matches the isolated firmware helpers for all seven target cores.

One freestanding AArch64 compile passes with strict alignment. The object uses
1,559 bytes of code/read-only data and requires runtime `memmove`. The largest
reported direct stack frame is 48 bytes. Source hashes, commands, logs, traces
and object identity are recorded in the evidence directory. These are software
checks; no secondary processor executed in this model or on the board.

The next implementation must bind privileged register access, publish a reserved
EL3 entry and mailbox, and coordinate the secondary handoff. It must provide
per-core exception handling, coherence, GIC wakeup/redistributor setup and the
A733 affinity mapping before extending the scheduler to all eight cores.
Physical execution requires independently qualified watchdog recovery first.
