# Preserve uncertain supply observations before electrical admission

Status: implemented and host-checked. Physical voltage and fitted radio wiring
remain unresolved. The image awaits a qualified launch wrapper.

The runtime PMIC decoder follows the pinned AXP8191 bootloader table. Its
DCDC8 and DCDC9 selectors disagree with the pinned Linux table from code 88
onward. Code 100 represents 1.8 V in U-Boot and 3.1 V in Linux. Both sources
identify the same selector register and seven-bit mask. Chip ID 3 and revision
0 do not establish which voltage table matches the fitted part.

## Decode only the range on which the references agree

For DCDC8 and DCDC9, retain the complete selector and enable bytes. Publish
`decoded=0` and `programmed_uv=0` for codes 88–127. Codes 0–87 retain their
existing voltage interpretation. Other rails retain their current profiles.
This changes no rail setting, voltage point, clock, lease or I2C sequence.
Consumers that require a decoded voltage must refuse these uncertain codes.

The decoder runs once per completed snapshot. Its budget remains zero MMIO,
locks, barriers, allocations, sleeps and extra copies. A source-comparison
check will enumerate all 256 selector bytes for both rails, execute the actual
U-Boot getter, parse the Linux ranges and compare their common range against
the native decoder. A composed provider case will verify that an uncertain
selector still publishes complete raw observations and can be reaped.

## Observe the missing supplies through the existing provider

A finite diagnostic will use the production runtime provider to observe DCDC1,
DCDC8, DCDC9, BLDO5, CLDO1 and CLDO3 sequentially under one stable rail lease.
Each snapshot retains the existing six-read sequence: page, chip, revision,
enable, selector and page. The provider performs no PMIC data writes. BLDO5's
selector at register 0x2e fills the gap in the saved second-board capture.

The diagnostic has a 1.5-second client deadline, within the returning monitor's
two-second budget. It starts after task entry and counter initialization;
whole-image startup and reporting time remain unmeasured. Each snapshot has
six byte reads and one copied result;
there is at most one outstanding request. Request reap precedes release of the
client rail lease. The provider retains its platform clock, pin and controller
ownership. A failed transfer retains the provider's existing resources and
reports its stage, phase and last status without reset or retry.

The diagnostic needs a frozen image, host checks and the coordinator's accepted
watchdog/breadcrumb launch path before execution. Its observations establish
programmed bytes and reference agreement only. Supply admission additionally
needs an authoritative transfer function for the fitted PMIC and evidence of
the fitted rail route. Readbacks cannot establish either electrical fact.

## Validation and integration boundaries

Host validation covers complete raw results, reserved and disputed selectors,
disabled outputs, request reap and retained faults. The OS-only,
hello-compute and external mimus baseline builds cover shared decoder linkage.
No board is accessed by this stream. The coordinator must approve and execute
any physical proposal after the board runner is qualified.

This read-only change requires one feature reviewer. Voltage or power-domain
changes require a separate design and three-reviewer panel before coding.
Thermal load transitions and E902 suspend/resume depend on processor lifecycle
evidence and shared resource ownership; this diagnostic supplies no such proof.
