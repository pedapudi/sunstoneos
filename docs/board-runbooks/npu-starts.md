# Three NPU starts in one board lifetime

Status: **runbook only**. No set is frozen and nothing here has run. The
coordinator freezes a set from the images and host this runbook names, then
runs it on port 3-3, whose speed bin (code 4, vf0400) admits 852 MHz.

## What the run qualifies

The production NPU startup ([design](../designs/npu-sram-route.md)) routes the
shared SRAM to the NPU and powers domain 4 before any NPU clock. It adopts a
PLL_NPU that an earlier start in the same power lifetime left running, and it
can retune that PLL to 852 MHz ([852 MHz design](../designs/npu-852mhz-plan.md)).
One lifetime runs the finite numerical image three times, each with two
copied jobs and a comparison with the reference:

- **Start 1, 492 MHz, cold.** PLL_NPU is disabled and the boot ROM routes the
  shared SRAM to the CPU. The image enables PLL_NPU, routes the SRAM, runs
  both jobs, stops, routes the SRAM back and returns to the ROM.
- **Start 2, 492 MHz, adopted.** The same image again, without a power
  removal. Its register pass finds PLL_NPU locked at 492 MHz with its output
  gate open, as start 1's local stop left it, and adopts it without a PLL
  write. Domain 4 stays ON and is adopted without a PCK600 write.
- **Start 3, 852 MHz, rate change.** The image built with `--rate-852`. Its
  register pass admits 852 MHz for the board's speed bin, finds PLL_NPU
  locked at 492 MHz and retunes it in the vendor's order. DCDC2 stays at
  800 mV.

Start 3 comes last because it is the only start with a clock change; a
failure there loses the least. A third start keeps start 2 as the adoption
test with no clock change, so a failure in start 3 is attributable to the
rate change alone.

## Predictions

Every start ends with status 0, a normal ROM return and verified DRAM
restoration, and both of its outputs match the reference: the first output's
SHA-256 and every byte of the second output, as in the routing run `nr-npu`
of `board-session-20261025-3/port-3-3`. The outputs of all three starts are
byte-identical. Each start's host record carries `npu_result.timing`, the
image's counter measurements:

| Start | Startup, entry to READY | Each job, publication to completion observation |
| --- | --- | --- |
| 1, 492 MHz, domain 4 ON at entry | 4.6 ms (modelled 4,615 µs) | One worker poll period, under 1 ms |
| 2, 492 MHz, adopted | 4.4 ms (modelled 4,414 µs) | One worker poll period, under 1 ms |
| 3, 852 MHz, rate change | 4.6 ms (modelled 4,615 µs) | One worker poll period, under 1 ms |

The model runs the PMIC at 400 kHz with each step at its wire time. Board
snapshot steps took 146% of their wire time ([power domains](../power-domains.md#measurements)),
which could add up to about 1 ms to each startup. Both boards read domain 4
ON at boot, so start 1 follows the modelled ON case. The service worker polls
for completion once per 1 ms tick, so a job shorter than a tick reads as the
time to the next poll at both rates; the measurement can bound the job time
but cannot show the 492/852 = 0.58 compute ratio.

The state read after start 1 shows PLL_NPU locked at 492 MHz with its output
gate open. Start 2 reaching a normal return then shows the adoption, because
the register pass admits an enabled PLL only through adoption or a rate
change, and a rate change needs the other profile. The read after start 2
shows the same PLL word as the one before it. The read after start 3 shows
the 852 MHz word.

If start 1 mismatches, the routing variant's match in `nr-npu` points at what
the production sequence changed: domain 4 powered first and the event-driven
waits. A start 2 that refuses at the register pass shows a difference between
the stopped state and the adoption predicate; a start 3 that refuses there
shows a bin or rate-change predicate mismatch. In both cases the image parks
at EL3, and the board needs a power removal.

## Inputs to freeze

- The finite numerical image built by `apps/npu-finite/build.py` from the
  revision that merges `work/npu-production-1025`, default profile, for
  starts 1 and 2; the same revision built with `--rate-852` for start 3. Each
  start needs its own root-review package with a new epoch, generation and
  lease; the package of start 3 names the user's approval of NPU clock
  changes and the 852 MHz image.
- The host `apps/npu-finite/host.py` from that revision, which reports
  `timing`.
- The read bundle `npu-clock-words` (bundle 9 of `board-bundles-20261025-6`,
  33 read-only FEL words, no NPU-window access) with three prediction files:
  the cold predictions of that set, and the stopped-state predictions below
  at 492 and at 852 MHz.
- The DCDC2 snapshot image `npu-supply-snapshot-20261024`, the survey build
  and the cache-off helper that the routing run used
  (`board-bundles-20261025-7/runbook.md`, steps 2 to 4).

## Order

Every step runs once, through `bundle-run.py`, and only after the step before
it passed. The steps follow the routing run's set
(`board-bundles-20261025-7/runbook.md`), whose commands apply unchanged apart
from the record names and the image of start 3.

| Step | Record | Action | Pass |
| ---: | --- | --- | --- |
| 0 | — | The user removes and restores the board's power; open a new session directory `S` for port 3-3 | The A733 ROM answers on a new FEL instance |
| 1 | `ts-ddr` | Cold DDR start (`tools/run-ddr.py`, as `s7-ddr`) | 12,288 MiB, stage 5 |
| 2 | `ts-read-0` | Bundle 9 with the cold predictions | Every checked word matches: PLL_NPU disabled, `0x03000008` bit 1 set |
| 3 | `ts-irq97`, `ts-cache-off-1`, `ts-survey-1`, `ts-dcdc2-1`, `ts-cache-off-2`, `ts-survey-2` | IRQ97 capture, cache-off and survey, DCDC2 snapshot, cache-off and survey | As in the routing run; DCDC2 enabled at 800 mV |
| 4 | — | Root finalization of package 1, warm-return chain up to `ts-dcdc2-1` | `session`, `prepare` and `validate` succeed |
| 5 | `ts-npu-1` | Start 1: `host.py run` with package 1 | Exit 0: both outputs match; `timing` present |
| 6 | `ts-read-1` | Bundle 9 with the 492 MHz stopped-state predictions | Every checked word matches |
| 7 | `ts-cache-off-3`, `ts-survey-3`, `ts-dcdc2-2`, `ts-cache-off-4`, `ts-survey-4` | Cache-off and survey, DCDC2 snapshot, cache-off and survey | As in step 3 |
| 8 | — | Root finalization of package 2; warm-return chain extended by `ts-npu-1` and `ts-dcdc2-2` | `session`, `prepare` and `validate` succeed |
| 9 | `ts-npu-2` | Start 2: `host.py run` with package 2 | Exit 0: both outputs match; `timing` present |
| 10 | `ts-read-2` | Bundle 9 with the 492 MHz stopped-state predictions | Every checked word matches |
| 11 | `ts-cache-off-5`, `ts-survey-5`, `ts-dcdc2-3`, `ts-cache-off-6`, `ts-survey-6` | Cache-off and survey, DCDC2 snapshot, cache-off and survey | As in step 3 |
| 12 | — | Root finalization of package 3 with the 852 MHz image; warm-return chain extended by `ts-npu-2` and `ts-dcdc2-3` | `session`, `prepare` and `validate` succeed |
| 13 | `ts-npu-3` | Start 3: `host.py run` with package 3 | Exit 0: both outputs match; `timing` present |
| 14 | `ts-read-3` | Bundle 9 with the 852 MHz stopped-state predictions | Every checked word matches |

The FEL reads carry no hang risk beyond the existing CCU reads; each start
comes after the reads and snapshots of its part of the lifetime.

## Stopped-state predictions

Words of bundle 9 that local stop sets; the cold prediction of every other
word is unchanged.

| Address | Word | Prediction | Why |
| --- | --- | --- | --- |
| `0x020022a0` | PLL_NPU | `& 0xfeffffff` = `0xf8305100` after starts 1 and 2; `0xf8104600` after start 3 | Enabled, LDO, lock enable, LOCK and output gate set at the 492 or 852 MHz factors; bit 24 recorded |
| `0x02003920` | PLL_NPU gate control | `0x00010001` | Software gate enable and No-Auto, written by start 1's enable table |
| `0x020039a0` | PLL_NPU gate status | `0x00010000` | Effective gate on |
| `0x02002b00` | NPU module clock | `& 0x8700001f` = `0x00000000` | Module gate closed by local stop, selector PLL_NPU, divider 1 |
| `0x02002b04` | NPU bus and resets | `0x00080000` | Bus gate closed, core, AXI and AHB resets asserted, SRAM reset released |
| `0x07064000`, `0x07064008` | Domain 4 policy, status | `& 0xf` = `0x8` | ON, never powered off |
| `0x03000008` | Shared SRAM routing | `& 0x2` = `0x2` | Set again at local stop and at EL3 before the ROM return |
| `0x0300624c` | SID speed-bin word | `0x00000496` | Code 4, column vf0400, which admits 852 MHz |

## Stop conditions and recovery

- Stop at the first nonzero exit of any step; do not retry. Keep the port
  lock and the claim until the user's power removal.
- A failure inside the routing window parks the CPU at EL3 with bit 1 clear.
  Nothing returns to the ROM from there; the power removal's cold ROM entry
  sets bit 1 before it uses the SRAM.
- A prediction mismatch in a read step is a finding and stops the sequence
  before the next start.
- Every start depends on the watchdog staying disabled: the host refuses to
  launch and the monitor refuses entry when `0x02050014` bit 0 is set.
- After step 14 the board keeps PLL_NPU at 852 MHz, domain 4, DCDC2 and the
  SRAM powered; a later start adopts 852 MHz or retunes it to 492 MHz.
