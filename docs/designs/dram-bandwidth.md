# Sustained DRAM read bandwidth

Status: implemented as a returning measurement application. The board
result is recorded in `docs/evidence/memory-dma.jsonl` once the run completes.

The application in `apps/dram-bandwidth/` measures how many bytes per second a
set of cores reads from DRAM when the data does not fit in any cache. Speech
and language models stream their weights from DRAM once per chunk of audio or
text, so this rate bounds how fast a model with weights larger than the caches
can run when it computes little per weight byte.

## Method

Eight worker tasks, one pinned to each core, fill a 40 MiB work region with a
fixed 64-bit pattern, each core writing its own 5 MiB slice. They then run
timed read passes for five core sets in turn: one Cortex-A55 core (core 0),
one Cortex-A76 core (core 6), both A76 cores (6 and 7), the six A55 cores (0 to
5) and all eight cores.

- In a pass each participating core reads one slice with 128-byte vector loads
  and no software prefetch. The slice a core reads advances by one on every
  pass, so a core never reads the slice its own caches received in the pass
  before. Each A55 core has a 128 KiB private L2 cache and each A76 core a 256 KiB
  one (read from the cache ID registers on the board); all cores share a 1 MiB L3.
  Against 5 MiB read per core, at most a few percent of a pass can hit in cache.
- The cores start together at a barrier. A pass lasts from the first start to the
  last finish. The counter runs at 24 MHz, so a pass of a few milliseconds has an
  error below 0.01 percent.
- Two warm-up passes precede eight timed passes for each core set. The result
  records every timed pass, the XOR digest of the data each pass read, the
  clock records before and after the run and the five thermal sensor readings
  before and after.
- The host decoder recomputes every slice digest from the fill pattern and
  refuses a pass whose digest differs. It converts ticks to gigabytes per second
  (10^9 bytes per second).

## Memory configuration

The DRAM training records of both boards and the boot0 analysis give the
following configuration. Each item is a record or a source fact, not a
reading from the controller on the board.

- Type: LPDDR5 (boot0 parameter `dram_type` 8), 12,288 MiB, reported by boot0
  and stored in both training records.
- Channels: two (`dram_tpr13` bit 6). An LPDDR5 channel is 16 bits wide, so the
  data bus is 32 bits wide.
- Operating point: boot0 trains P-state 0 at 2040 MHz and the OS runs on it
  ([DRAM fast boot](dram-fast-boot.md)).
- Pin rate: with double data rate on the 2040 MHz clock the interface moves
  4080 megatransfers per second on 32 bits, which is 16.32 GB/s. The
  relation between the stated frequency and the data rate is an assumption.

## Predicted sustained read bandwidth

Sequential reads of an idle LPDDR5 interface reach 70 to 85 percent of the pin
rate when enough requests are outstanding, because refresh, bank turnaround and
the command bus consume the rest. That puts the ceiling at 11.4 to 13.9 GB/s.
How many requests a core keeps outstanding limits what one core reaches: with
about 12 line fills in flight at a loaded latency of 120 to 150 ns, a core
reaches at most 5 to 6.4 GB/s. The A76 core issues deeper prefetch streams and
has a larger window than the A55 core.

| Core set | Predicted GB/s (range) | Limit assumed |
| --- | --- | --- |
| One A55 core | 5 (3 to 6.5) | outstanding line fills |
| One A76 core | 9 (6 to 12) | outstanding line fills and prefetch depth |
| Two A76 cores | 12 (9 to 13.5) | approaches the interface ceiling |
| Six A55 cores | 12 (9 to 13.5) | interface ceiling |
| All eight cores | 12 (9.5 to 13.5) | interface ceiling |

A result of all eight cores below 9 GB/s would show a limit below the DRAM pins,
for example the 600 MHz memory-bus clock, and would change the plan for any
workload that streams more than that rate.

## Result

The board record, the measured table and the comparison with this prediction
are in `docs/evidence/memory-dma.jsonl` and the status file
`docs/status/memory-dma.md`.
