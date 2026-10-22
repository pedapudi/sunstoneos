# Headset voice-recognition measurements

This record compares the integration base (commit `87da531`) with the
implementation of [bluetooth-voice-recognition.md](bluetooth-voice-recognition.md)
(commit `c8de9d1`) against that design's budgets. Every measured metric is
within its budget in the design. The owner copies each PCM byte once, and the
design gives a measured reason for each increase over the base.

## Method

Both commits were exported with `git archive` and built in separate
directories with the same toolchain and options: the Bluetooth image with
recognition (`SUNSTONE_BLUETOOTH_APPLICATION`, `SUNSTONE_BLUETOOTH_MIMUS`),
the same with mSBC, the implementation with call control on, and the OS
headset image. Raw outputs are under
`~/.cache/sunstoneos/build/bluetooth-voice-recognition-measure/`.

- **Sizes.** `llvm-size -A` on the AArch64 objects. Code is `.text*` plus
  `.rodata*`; static data is `.data*` plus `.bss*`.
- **Instructions, ordering instructions and locks.** The design's Unicorn
  harness, rebuilt against each exported tree with the image's flags (`-O2`,
  `-mgeneral-regs-only`). The harness runs the real `bt_hfp_ag.c` input path
  on one complete AT line, and the real `headset.c` owner step on one 60-byte
  SCO packet. Capstone decodes every executed instruction to count `DMB`,
  `DSB`, load-acquire, store-release and exclusive instructions, and the trace
  records every function entered.
- **Whole segment.** A loop in the same harness re-arms one 60-byte CVSD
  packet and runs the owner step until it publishes a 48,000-byte segment. A
  memory-write hook counts the bytes stored, and a code hook counts the
  instructions executed, each by function.
- **Polls and host checks.** The `bluetooth-headset.py` host model, with
  the repeated recognition profile, CVSD and mSBC, cases `button`, `start`,
  `activations` and `bac-idle`. Each case prints its latencies in owner
  polls. Every case except `speech` passed in the repeated CVSD composition,
  with call control off and on, and in the mSBC composition; the default
  cases passed in the single-capture composition. Both repeated compositions
  check each taken segment's digest against the slot's bytes. The CVSD
  `early` case ends activations after zero and an odd number of bytes, so
  publication hashes a remainder shorter than one 64-byte block.
  `os-image-headset.py` passed against the OS headset build.
- **Wake-ups.** The wait calls in each consumer loop: `vTaskDelay(1)` wakes
  1,000 times per second; a notification wait with a 100 ms or 250 ms timeout
  wakes at most 10 or 4 times per second plus once per notification.

## Results

OFF and ON are the call-control settings; OFF is the default. The base has
no call-control option and always builds call control in.

| Metric | Base | Implementation | Budget | Within |
| --- | --- | --- | --- | --- |
| `bt_hfp_ag.c` code, OFF | 15,066 B | 11,572 B | ≤ 13,000 B | yes |
| `bt_hfp_ag.c` code, ON | 15,066 B | 15,800 B | ≤ 15,800 B | yes |
| `apps/bluetooth/headset.c` code; static data, OFF | 11,879 B; 440,320 B | 12,807 B; 440,392 B | ≤ 13,400 B; ≤ 440,480 B | yes |
| `apps/bluetooth/headset.c` code; static data, OFF with mSBC | 13,139 B; 440,320 B | 13,663 B; 440,456 B | no code row; ≤ 440,480 B | yes |
| `apps/bluetooth/headset.c` static data, ON | 440,320 B | 440,648 B | ≤ 440,700 B | yes |
| `msbc.c` code | 1,012 B | 1,036 B | ≤ 1,076 B | yes |
| `apps/os/headset.c` code; static data | 2,069 B; 548,192 B | 1,861 B; 500,184 B | ≤ 2,100 B; ≤ 500,192 B | yes |
| `recognition.cc` static data | 438,985 B | 390,985 B | 48,000 B smaller | yes |
| `recognition.cc` code, CVSD; mSBC | 5,527 B; 5,575 B | 6,003 B; 6,051 B (476 B more) | ≤ 500 B more | yes |
| `bt_sdp_server.c` code | 10,333 B | 10,333 B | unchanged | yes |
| Existing AT lines, ON | see below | largest increase 1.91% (`AT+NREC`) | ≤ 2% each | yes |
| Existing AT lines, OFF | see below | every line fewer instructions than the base | ≤ 2% each | yes |
| `AT+BVRA=1`; `AT+BVRA=0` | none | 808 and 803 OFF; 1,240 and 1,236 ON | ≤ 1,400 | yes |
| Activation start | none | 117 CVSD; 128 mSBC; 146 mSBC with a 37-byte frame skip | ≤ 400 | yes |
| Repeated-capture step, packet between activations | 171 CVSD; 174 mSBC | 183; 188 | ≤ 188; ≤ 191 | yes |
| Admission and recognition per poll | none | 66 in idle; 64 between activations | ≤ 75 | yes |
| Recognition step, packet between activations | none | 247 CVSD; 252 mSBC | ≤ 250; ≤ 255 | yes |
| Recognition step, no packet between activations | none | 164 | no row | not budgeted |
| Owner step, packet in a segment | 422 CVSD; 36,811 mSBC; unhashed | at most 2,993 CVSD; at most 40,931 mSBC; hashed | ≤ 3,000; ≤ 46,500 | yes |
| Owner step, publishing packet | 1,684,087 CVSD; 1,715,924 mSBC | 6,025; 41,542 | ≤ 8,000; ≤ 52,000 | yes |
| Owner instructions per 48,000 B CVSD segment | 2,044,008 | 2,090,169 (+2.3%) | ≤ 2,100,000 | yes |
| Owner `memcpy` bytes per 48,000 B CVSD segment | 48,000 | 48,000 | each byte once, plus fewer than 64 at publication | yes |
| Consumer work per 48,000 B segment | 1,763,367 (47 chunks × 1,699 plus SHA-256) | 372 | ≤ 600 | yes |
| Consumer PCM copies per segment, before WAV conversion | 96,000 B (slot to chunk, chunk to buffer) | 0 | no PCM copy | yes |
| Ordering instructions, CVSD packet in a segment | 5 `LDAR` | 1 `LDAR` | no row | yes |
| Ordering instructions, publication | 5 `LDAR`, 1 `STLR` | 1 `LDAR`, 2 `STLR` | release head, then release word | yes |
| Ordering instructions, activation start | none | 1 `LDAXR`/`STLXR` pair, 1 `STLR` | one compare-and-exchange | yes |
| Ordering instructions, consumer take and ack | 5 `LDAR` per 1 KiB chunk (235 per segment) | 6 `LDAR`, 1 `STLR` per segment | no row | yes |
| Lock acquisitions on every measured path | 0 | 0 | no row | yes |
| Owner wake-ups per second | 1,000 | 1,000 | 1,000 until the UART interrupt | yes |
| Consumer wake-ups while waiting | 1,000 per second (`vTaskDelay(1)`) | at most 10 per second (recognizer); 4 (OS client); plus 1 per notification | ≤ 10; ≤ 4 | yes |
| `AT+BVRA=1` line to committed on | none | 4 polls, CVSD and mSBC | ≤ 4 | yes |
| Commit to synchronous-connection setup accepted | none | 4 polls; 13 after a codec list change while idle | ≤ 4; ≤ 13 | yes |
| Owner START to committed on | none | 5 polls | the host model's bound of 5 | yes |
| Link open to first PCM byte in a slot | none | 1 poll | the poll holding the packet | yes |
| Publication to `+BVRA: 0` queued | none | 1 poll | first poll with the serial stream free | yes |

AT lines in instructions, base and implementation, with call control on:
`AT+BRSF` 1,292 and 1,300; `AT+BAC` 1,205 and 1,208; `AT+CIND=?` 2,441 and
2,444; `AT+CIND?` 1,427 and 1,430; `AT+CMER` 1,567 and 1,570; `AT+BIA` 1,723
and 1,726; `AT+VGS` 1,190 and 1,190; `AT+VGM` 1,173 and 1,176; `AT+BCS`
1,104 and 1,107; `AT+CMEE` 1,273 and 1,291; `AT+CLCC` 1,163 and 1,181;
`AT+COPS?` 1,246 and 1,264; `AT+XAPL` (`ERROR`) 2,321 and 2,355; `AT+NREC`
(`ERROR`) 1,781 and 1,815. With call control off, the implementation runs
each line in 420 to 882 fewer instructions than the base with call control.

## Findings

**Hashing whole blocks removes the staging copy.** `storage_digest_add`
compresses a 64-byte block in place only when its staging block is empty.
When the owner hashed each 60-byte CVSD packet as it arrived, every call
began with a partly filled staging block, and the owner copied all 48,000
bytes of a segment into it: 96,000 `memcpy` bytes and 2,180,634 instructions
per segment. The owner now hashes only whole blocks while a segment records
and hashes the rest of the even-length prefix, fewer than 64 bytes, at
publication. A 48,000-byte segment is 750 whole blocks, so publication hashes
nothing more, and the owner stores 48,000 `memcpy` bytes per segment, as the
base did. Across the system, bytes copied before the WAV conversion fall from
144,000 to 48,000 per segment, because the consumers no longer copy.

**The owner spends 2.3% more instructions per CVSD segment than the base.**
Counted by function over one segment, SHA-256 compression (1,675,481) and
the packet `memcpy` (125,294) cost the same as in the base. Of the 46,161
extra instructions, about 20,200 are the 750 one-block calls into
`storage_digest_add`, which replace one call over the finished segment, and
about 25,900 are the per-packet slot, ring and record checks of the capture
step. The design's budget row covers both. Owner and consumer together fall
from about 3.81 million to 2.09 million instructions per segment, because the
consumer no longer hashes the segment again.

**`recognition.cc` code grows by 476 bytes.** The repeated recognizer gains
the loop that requests each activation, borrows and acknowledges the segment
and waits on a task notification; the finite mode-1 copy loop stays beside
it. The design's budget row allows 500 bytes.
