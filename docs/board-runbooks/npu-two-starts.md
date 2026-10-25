# Two NPU starts in one board lifetime

Status: **runbook only**. No set is frozen and nothing here has run. The
coordinator freezes a set from the image and host this runbook names, then
runs it on port 3-3.

## What the run qualifies

The production NPU startup ([design](../designs/npu-sram-route.md)) routes the
shared SRAM to the NPU, powers domain 4 before any NPU clock, and adopts a
PLL_NPU that an earlier start in the same power lifetime left running. One
lifetime runs the finite numerical image twice, each time with two copied
jobs and a comparison with the reference:

- **Start 1** begins from a cold board: PLL_NPU disabled, the shared SRAM
  routed to the CPU by the boot ROM. The image enables PLL_NPU, routes the
  SRAM, runs both jobs, stops, routes the SRAM back and returns to the ROM.
- **Start 2** runs the same image again without a power removal. Its
  register pass finds PLL_NPU locked at the profile with its output gate
  open, as start 1's local stop left it, and adopts it without a PLL write.
  Domain 4 stays ON and is adopted without a PCK600 write.

**Prediction.** Both starts end with status 0, a normal ROM return and
verified DRAM restoration, and all four outputs match the reference: in each
start the first output's SHA-256 and every byte of the second output, as in
the routing run `nr-npu` of `board-session-20261025-3/port-3-3`. The state
read between the starts shows PLL_NPU locked with its output gate open.
Start 2 reaching a normal return then shows the adoption, because the
register pass admits an enabled PLL only through that path. The state read
after start 2 shows the same PLL word as the one before it.

The routing variant matched the reference in `nr-npu`, so a mismatch in
start 1 points at what the production sequence changed: domain 4 powered
before the PLL and the local clocks. A start 2 that refuses at the register
pass shows a difference between the stopped state and the adoption
predicate; the image then parks at EL3, and the board needs a power removal.

## Inputs to freeze

- The finite numerical image built by `apps/npu-finite/build.py` from the
  revision that merges `work/npu-production-1025`, with its default profile.
  Both starts use the same image file and the same host
  (`apps/npu-finite/host.py` from that revision). Each start needs its own
  root-review package with a new epoch, generation and lease.
- The read bundle `npu-clock-words` (bundle 9 of `board-bundles-20261025-6`,
  33 read-only FEL words, no NPU-window access) with two prediction files:
  the cold predictions of that set, and the stopped-state predictions in the
  table below.
- The DCDC2 snapshot image `npu-supply-snapshot-20261024`, the survey build
  and the cache-off helper that the routing run used
  (`board-bundles-20261025-7/runbook.md`, steps 2 to 4).

## Order

Every step runs once, through `bundle-run.py`, and only after the step before
it passed. The steps follow the routing run's set
(`board-bundles-20261025-7/runbook.md`), whose commands apply unchanged apart
from the record names.

| Step | Record | Action | Pass |
| ---: | --- | --- | --- |
| 0 | — | The user removes and restores the board's power; open a new session directory `S` for port 3-3 | The A733 ROM answers on a new FEL instance |
| 1 | `ts-ddr` | Cold DDR start (`tools/run-ddr.py`, as `s7-ddr`) | 12,288 MiB, stage 5 |
| 2 | `ts-read-0` | Bundle 9 with the cold predictions | Every checked word matches: PLL_NPU disabled, `0x03000008` bit 1 set |
| 3 | `ts-irq97`, `ts-cache-off-1`, `ts-survey-1`, `ts-dcdc2-1`, `ts-cache-off-2`, `ts-survey-2` | IRQ97 capture, cache-off and survey, DCDC2 snapshot, cache-off and survey | As in the routing run; DCDC2 enabled at 800 mV |
| 4 | — | Root finalization of package 1, warm-return chain up to `ts-dcdc2-1` | `session`, `prepare` and `validate` succeed |
| 5 | `ts-npu-1` | Start 1: `host.py run` with package 1 | Exit 0: both outputs match |
| 6 | `ts-read-1` | Bundle 9 with the stopped-state predictions | Every checked word matches |
| 7 | `ts-cache-off-3`, `ts-survey-3`, `ts-dcdc2-2`, `ts-cache-off-4`, `ts-survey-4` | Cache-off and survey, DCDC2 snapshot, cache-off and survey | As in step 3 |
| 8 | — | Root finalization of package 2; warm-return chain extended by `ts-npu-1` and `ts-dcdc2-2` | `session`, `prepare` and `validate` succeed |
| 9 | `ts-npu-2` | Start 2: `host.py run` with package 2 | Exit 0: both outputs match |
| 10 | `ts-read-2` | Bundle 9 with the stopped-state predictions | Every checked word matches |

The FEL reads of steps 2, 6 and 10 carry no hang risk beyond the existing CCU
reads; the two starts carry the risk of the run, so they come after them in
each half of the lifetime.

## Stopped-state predictions

Words of bundle 9 that local stop sets; the cold prediction of every other
word is unchanged.

| Address | Word | Prediction | Why |
| --- | --- | --- | --- |
| `0x020022a0` | PLL_NPU | `& 0xfeffffff` = `0xf8305100` | Enabled, LDO, lock enable, LOCK and output gate set at the 492 MHz factors; bit 24 recorded |
| `0x02003920` | PLL_NPU gate control | `0x00010001` | Software gate enable and No-Auto, written by start 1's enable table |
| `0x020039a0` | PLL_NPU gate status | `0x00010000` | Effective gate on |
| `0x02002b00` | NPU module clock | `& 0x8700001f` = `0x00000000` | Module gate closed by local stop, selector PLL_NPU, divider 1 |
| `0x02002b04` | NPU bus and resets | `0x00080000` | Bus gate closed, core, AXI and AHB resets asserted, SRAM reset released |
| `0x07064000`, `0x07064008` | Domain 4 policy, status | `& 0xf` = `0x8` | ON, never powered off |
| `0x03000008` | Shared SRAM routing | `& 0x2` = `0x2` | Set again at local stop and at EL3 before the ROM return |

## Stop conditions and recovery

- Stop at the first nonzero exit of any step; do not retry. Keep the port
  lock and the claim until the user's power removal.
- A failure inside the routing window parks the CPU at EL3 with bit 1 clear.
  Nothing returns to the ROM from there; the power removal's cold ROM entry
  sets bit 1 before it uses the SRAM.
- A prediction mismatch in a read step is a finding and stops the sequence
  before the next start.
- Both starts depend on the watchdog staying disabled: the host refuses to
  launch and the monitor refuses entry when `0x02050014` bit 0 is set.
- After step 10 the board keeps PLL_NPU, domain 4, DCDC2 and the SRAM
  powered; a later start in the same lifetime adopts them again.
