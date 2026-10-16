# DMA0 startup preserves shared clocks and requires a complete controller handoff

The [DMA platform component](../drivers/dma/a733_dma_platform.c) validates
inherited fabric clocks and prepares DMA0 for the privileged DMA service.
It supports an already enabled, idle controller or a controller whose reset
is already asserted and whose dedicated bus gates are off. The component never
asserts reset. Host model and pinned-source checks pass; both implementation
files compile for AArch64. Physical startup and production binding remain
unqualified.

The [public interface](../include/a733_dma_platform.h) accepts copied startup
metadata, a finite deadline, authenticated resource acquisition, a common
register mutex, a monotonic clock, and ordered register operations. The
[native adapter](../drivers/dma/a733_dma_platform_native.c) restricts writes to
DMA0's dedicated gates, reset release, automatic clock gating, and interrupt
routes. Shared PLLs, fabric clocks, channel state, pins, and voltage are read-only
or outside its address allowlist.

## Lifetime authority precedes every register access

The platform must authenticate a handoff covering all DMA0 channels, remote MCU
users, CPU users, route writers, and FIFO producers. That handoff must establish
that earlier work drained, producers stopped, and interrupt delivery remains
masked. Idle register observations alone do not establish those facts.
The `handoff` field identifies external evidence; assigning a nonzero number
does not create authorization.

The acquisition callback obtains one token from the shared clock registry:

| Authority | Required coverage |
| --- | --- |
| Shared `A733_CLOCK_FABRIC` | AHB, NSI, and MBUS source selectors, divisors, and global gates |
| Shared `A733_CLOCK_OSC` | The selected 24 MHz oscillator remains stable |
| Shared `A733_CLOCK_PERI0`, when selected | Peripheral PLL configuration, modulation, output gates, automatic gates, and divisors remain stable |
| Shared `A733_CLOCK_REF`, when PERI0 is selected | Reference PLL configuration remains stable |
| Exclusive `A733_CLOCK_DMA0` | All controller users, dedicated bus/MBUS leaf gates, reset, automatic gating, and CPU/MCU routes |

The component derives the shared mask from the expected fabric profile before
acquisition. The registry expands PERI0 through REF and OSC; the derived mask
already contains those dependencies. A callback can call
`a733_clock_acquire(registry, config->owner, shared, A733_CLOCK_DMA0,
config->deadline_us, token)` after authenticating the handoff. Registry refusal
must leave the token unchanged. A pending admission must preserve its existing
token identity on subsequent polls.

Every hardware writer must use this registry and the common register mutex.
The mutex serializes updates to shared words; the token protects configuration
between updates. Acquisition callbacks run outside the register mutex.
Neither the shared FABRIC token nor the DMA0 token authorizes changes to
unrelated critical gates.

The object, token, and callback state remain in private privileged storage for
the OS lifetime. The component has no release or teardown operation. Success
keeps the token while DMA runs; failure retains it and prohibits further
component register access. Task abandonment does not release authority.

## Admission verifies the selected parent and programmed rate

Startup metadata specifies the expected source, divisor, enable fields, and
rate for each fabric clock. Hardware must match that metadata under the acquired
token. Only these inherited parent selectors are admitted:

| Clock and register | Selector | Parent rate |
| --- | --- | --- |
| AHB, `0x02002500` | 0 or 3 | 24 MHz or PERI0 600 MHz |
| NSI, `0x02002580` | 0, 2, 3, or 4 | 24 MHz, PERI0 800 MHz, 600 MHz, or 480 MHz |
| MBUS, `0x02002588` | 0, 1, 3, or 4 | 24 MHz, PERI0 600 MHz, 480 MHz, or 400 MHz |

The divisor is the five-bit field plus one. Its range is 1–32. The parent rate
must divide by it without a fractional hertz result and equal the supplied rate.
NSI and MBUS global gates must already be enabled. DDR, display-engine, NPU,
32 kHz, internal-oscillator, and unknown parents are refused. Adding those
parents requires source validation and corresponding authority against their
writers.

The oscillator selector at `0x07090160[15:14]` must be 0 or 3, using the
[existing source-validated oscillator interpretation](gpu-resources.md). PERI0 profiles also
require the source-defined PLL enable, supply, lock, and output flags. Reference
PLL N and M must be equal within the source's multiplier limits, producing
24 MHz. PERI0 N must lie between 53 and 105. Each selected output divisor must
produce its nominal rate. For example, N=100 and divisors 2, 3, and 5 produce
the 1.2 GHz, 800 MHz, and 480 MHz outputs; fixed division produces 600 and 400 MHz.

The component refuses enabled modulation and selected-output automatic gating.
It preserves unselected divider fields. Clock checks establish programmed
settings; physical frequency remains unmeasured. The clock reference labels DMA0's leaf gates
with `dcxo`; that bookkeeping does not establish that DMA transfers run at
24 MHz. The fabric checks establish conservative stability prerequisites. Throughput
and complete fabric power management require separate qualification.

## Cold startup releases an inherited reset without taking over active work

Cold startup requires DMA0 reset asserted and its bus and MBUS leaf gates off.
After observing that state, it waits at least 20 microseconds. It then rechecks
the baseline and performs verified updates in source order:

1. Release DMA0 reset at `0x02002704[16]`.
2. Enable its bus gate at `0x02002704[0]`.
3. Enable its MBUS leaf gate at `0x020025e4[0]`.
4. Require every channel idle, unpaused, with zero mode, interrupt enable,
   interrupt pending, and descriptor-high state.
5. Program DMA automatic gating at `0x04601028` to the source value 4.
6. Remove old CPU/MCU interrupt destinations before adding the requested routes.
7. Recheck channel quietness before reporting ready.

The MBUS source helper ORs `0x40302` into the shared word. Those bits also
represent critical VE decoder, ISP, CSI, and VE gates. This implementation
requires all of them already set and preserves them. Missing bits cause a
retained refusal; startup does not enable unrelated blocks. Other fields survive
read-modify-write under the same mutex, including changes between polls.

CPU and MCU route registers are `0x04601034` and `0x04601038`. Startup compares
their low 16 bits with expected values, preserves upper bits, and permits only
changes within the declared route-change mask. Desired CPU/MCU destinations must
be disjoint. The source's CPU `0x00ff` and MCU `0xff00` partition is a policy
example; it does not transfer MCU ownership to SunstoneOS.

Adoption requires reset released, both leaf gates enabled, the desired routes
already installed, automatic gating equal to 4, and the same quietness checks.
It performs no writes. Both modes require a complete handoff. Neither mode stops
an active channel, acknowledges an inherited interrupt, clears descriptors, or
retries an uncertain operation.

The deadline is an absolute monotonic time in microseconds. It differs from the
DMA controller's capture deadlines in counter ticks. Every callback and register
access has checks before or after completion, including final unlock. A late
successful callback cannot publish readiness. The clock provider must enforce
one monotonic lower bound across authority admission and its internal samples;
checking only samples made by this component cannot detect a hidden regression
inside another callback. All callbacks must themselves be bounded.

A write callback can fail after issuing the write. The component records that
possibility and retains every admitted token. It does not attempt rollback or
further register access after failure. Independent watchdog protection remains
a prerequisite because a stalled peripheral access may prevent software from
checking its deadline.

## Focused evidence covers admission and ordering behavior

The host fixture uses the real shared registry with modeled device registers.
It checks deferred admission, reset timing, concurrent updates to unrelated
fields between locks, route preservation, adoption without writes, and refused
clock profiles. It also checks denied handoff, a live remote channel, uncertain
writes, clock regression, and late final operations. Retained failures reject
later polls without more device operations.

The source comparison decodes observed fabric rates and gate writes from the
same fixture log. It checks the DMA0 device-tree binding, critical-gate mask,
reset/bus/MBUS order, automatic-gating value, and CPU route value. The native
adapter has compile evidence; it has not executed against the physical device.
Existing DMA controller, memory-adapter, FreeRTOS service, audio, and GPU checks
remain applicable and were not repeated.

PERI0 input-divider bit 1 must be zero, selecting division by one. The A733
User Manual V1.00, section 4.1.6.6, defines this field even though the kernel
clock profile omits it. The focused `--pll-input-only` case admits the unchanged
profile and refuses divide-by-two before writes, retaining ownership.
Sanitizers and native compilation pass. Evidence is in
`~/.cache/sunstoneos/evidence/peri0-input-divider-20261016/`;
unchanged platform and service behavior retains its earlier evidence.

Raw evidence is in `~/.cache/sunstoneos/evidence/dma-platform-20261016/`.
The separate `clock-evidence.json` covers registry extension checks;
`evidence.json` records platform inputs, artifacts, commands, and results.
Pinned local sources and their manifest are in
`~/.cache/sunstoneos/sources/dma-platform/`. They come from
[the board kernel revision](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f):

| Source path | SHA-256 |
| --- | --- |
| `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c` | `1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77` |
| `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-rtc.c` | `484ef6c6cbdac224484f22ca57db8063e2f83404bfe18c18e32c12ab1bc0a3ea` |
| `bsp/drivers/clk/sunxi-ng/ccu_mult.h` | `1ecf95b5e4d35807b4b8fa5dca3b454af5b0924991fcee276e730b8e528d7c79` |
| `bsp/drivers/clk/sunxi-ng/ccu_div.h` | `a379fcb476803bc3fda7143f79d18cfcecb4320bc9f04facbea2a5b0d7c1d17e` |
| `bsp/drivers/clk/sunxi-ng/ccu_gate.h` | `02777af3356f168356d478ac8bcd9710acac97cc2a75b5354302c2787f9eac19` |
| `bsp/drivers/dma/sunxi-dma.c` | `d60e6598835ddf16b1f2374f746abd82da507dfefeb16edade9a0f7021b3d3bb` |
| `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` | `1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa` |

Production integration must supply authenticated handoff, the shared clock and
register services, masked interrupt delivery, qualified watchdog recovery, and
protected Device mappings. After readiness, the DMA service may authorize only
channels in `desired_cpu`. Its descriptor, buffer, translation, cache, and FIFO
reservations remain separate requirements. Controller readiness does not establish
buffer ownership, DMA drain, microphone operation, or physical qualification.
