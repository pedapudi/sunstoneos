# Plan: run the NPU at 852 MHz at the inherited 800 mV

Status: **plan**. Nothing here is built. The change needs the coordinator's
approval and its own performance design, reviewed under section 4 of
[performance architecture](../performance-architecture.md), before any code.
A board run of it also needs the user's approval for that clock change, as
every clock or voltage change does
([CPU clocks](../cpu-clock.md#a-session-must-grant-each-change)). This plan
is rank 6 of the [vendor action plan](../vendor-firmware/action-plan.md) and
follows rank 2, the [SRAM routing in every NPU startup](npu-sram-route.md).

## What the vendor table allows

Every SunstoneOS NPU image runs PLL_NPU at 492 MHz: N=82, P0=4, M1=1, factor
word `0x00305100` (`drivers/npu/a733_npu_startup.c`; [npu-pll.md](../npu-pll.md)).
DCDC2, the NPU supply, stays at the 800 mV that boot firmware leaves; the
power-domain owner verifies it and nothing writes it.

The board kernel's NPU operating-point table (`sun60iw2p1-cpu-vf.dtsi`
lines 2069–2091, linux-orangepi `2ac08e8c`, SHA-256 `d5535630…`) lists
852 MHz with a voltage per speed-bin column:

| Columns | 852 MHz voltage |
| --- | --- |
| vf0000, vf0200, vf0201, vf0400, vf0500, vf0501, in both the 24 MHz and the 26 MHz oscillator columns; 26 MHz vf0204 | 800 mV |
| vf0401 | 820 mV |
| vf0202, vf0203; 24 MHz vf0204 | 880 mV |

Port 3-3's SID speed-bin word reads `0x496`, code 4, column vf0400, which
lists 800 mV in both oscillator columns. Port 3-1's bin is not recorded.

The vendor driver itself selects 1,008 MHz and never changes DCDC2
([NPU backsolve](../reference-backsolve-npu.md#npu-driver-probe), step 11).
Its own table lists 1,008 MHz at 900 mV or more, so that setting runs outside
the table, and SunstoneOS does not copy it. Raising DCDC2 is out of scope:
boot0 programs it in its DRAM supply setup, and it also supplies `vdd-edp`.

## Planned change

1. **A second PLL profile.** 852 MHz = 24 MHz × 71 ÷ 2: N=71 and P0=2, factor
   word `0x00104600`, a 1,704 MHz oscillator inside the board source's
   N range of 53–105. The dedicated design confirms the oscillator range
   and the P0 encoding against manual section 4.1.6.56 before use.
2. **Admission by speed bin and grant.** The session names the profile. The
   startup admits 852 MHz only when the SID speed-bin code maps to a column
   that lists 800 mV, and with a reviewed approval record bound to one
   board and one image, as the CPU clock grants do.
3. **The rules that name the factor word.** The allowlist adapter accepts
   only `0x00305100` as a factor write
   (`drivers/npu/a733_npu_platform_native.c`); the register pass, the LOCK
   check and the adoption predicate compare with that word. Each takes the
   selected profile's word. Adoption admits a PLL only at the selected
   profile, so an image at the other rate refuses a PLL that an earlier image
   left running, until a reviewed rate-change step exists.
4. **Checks.** `checks/npu-pll.py` compares the second profile with the
   pinned sources; the host model and the linked-instruction check run the
   sequencer at both profiles, including the cross-profile adoption refusal.

No voltage, supply, domain or routing step changes.

## Measurement and prediction

Before and after, on the same board, image sources and model, with the
[two-start runbook](../board-runbooks/npu-two-starts.md)'s numerical image:

- both outputs byte-identical to the 492 MHz run;
- the job time, from the finite image's start and end counters, falls toward
  492/852 = 0.58 of its 492 MHz value, less where memory bounds the work;
- the thermal profile of the finite image reports no new thermal stop.

Port 3-3 runs it first. Port 3-1 runs it only after the read-only row of
action plan rank 4 records its speed bin.
