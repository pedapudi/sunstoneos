# Measurements: exact mimus integer matrix products on the GPU

These measurements check the [GPU matrix-product component](../gpu-mimus.md)
against the budgets in its [design](gpu-mimus-compute.md). They compare the
integration base (`audit/integration`, commit `d40ca4b`) with the commit that
last changed this document. All values come from host builds, host checks and
the time model; no board or GPU was used, and no USC instruction was executed.

## Method

- **Base.** The base commit was exported with `git archive`, configured with
  the shared configuration script and `-DSUNSTONE_GPU_COMPUTE_APPLICATION=ON`,
  and built with `-j 6` under `~/.cache/sunstoneos/build/gpu-mimus-compute/`.
  The seven targets that compile `drivers/gpu/pvr_compute_job.c` were built
  at both commits: the GPU, compute, free-list, IRQ, memory and power service
  models and the GPU render application. `llvm-size -A` gave section sizes.
- **Unchanged job bytes.** At both commits, `checks/pvr-compute-package.py`
  compared the native PDS and CDM bytes of compiled packages with the pinned
  Mesa generators for profiles two and three, and `checks/pvr-compute-service.py`
  ran its first three emulator cases. At the change,
  `checks/pvr-compute-job.py` compared the fixed job with Mesa's reference;
  its reference build now receives the project's include directory, without
  which it stopped at a missing header at both commits.
- **New checks.** `checks/gpu-mimus-program.py`, `checks/gpu-mimus-layers.py`
  and `checks/gpu-mimus-readout.py` ran as listed in
  [gpu-mimus.md](../gpu-mimus.md#checks), with mimus at commit `3e05158`.
  `checks/gpu-mimus-time-model.py` ran twice: with the design's assumptions,
  and with the call list, instruction groups per MAC and read-out counts from
  those checks.

The base has no GPU path for mimus, so its rows below are the CPU profile in
[mimus.md](../mimus.md) or the design's stated baseline.

## Results against the design's budgets

| Metric | Baseline | Target | Result | Within budget |
| --- | --- | --- | --- | --- |
| Output bytes differing from mimus's CPU path | No GPU path | 0 | 0 of 12,923,134 bench outputs; 0 in every other case | Yes |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | 0, 0, 0: the argument record holds the model and arena addresses, and the read-out reads the output region | Yes |
| Jobs per recording | Not possible | 204 | 204 | Yes |
| USC instruction groups per MAC | None | At most 1.75 | 3.39: 217 groups per 64 MACs | **No** |
| Program code bytes | None | At most 4,000 | 7,624, in two 4 KiB program pages | **No** |
| GPU compute, all channels | 14.894 s on the A55 | 0.574 s | 1.111 s in the time model with the measured count | **No** |
| DRAM bytes read per MAC, per lane | None | 0.75 | 0.75 | Yes |
| Lines maintained per recording, GPU only | None | 597,601 cleaned; 988,060 cleaned and invalidated | 597,397 cleaned; 988,060 cleaned and invalidated | Yes |
| `DSB SY` per job | None | 6 | 6 | Yes |
| Tag words per recording, GPU only | None | 815,008 | 815,008 | Yes |
| Read-out instructions per recording | Inside the CPU kernel | At most 43.3 M | 37.6 M (with bias in every layer) | Yes |
| Compares per job; translations; hashes | Native resource admission and a CPU mapping check | At most 12; none; none | 9; none; none | Yes |
| Ring and job-word bytes per job | 368-byte bound | At most 300 | 276: 184 ring bytes, 92 job-word bytes | Yes |
| Linear stage, best channel split, 494 MHz | 14.894 s | 0.629 s | 0.997 s, 80.6% of MACs on the GPU | **No** |
| Linear stage, best channel split, 2,002 MHz | 14.894 s | 0.396 s | 0.535 s, 45.5% of MACs on the GPU | **No** |
| Linear stage, GPU only, 2,002 MHz | 14.894 s | 0.612 s | 1.149 s | **No** |
| GPU leaf entries | 4 KiB only | 17 | 18: the program takes two 4 KiB pages | **No** |

The cleaned-line count is 204 lines below the design's, because the model
places every call's codes on a 64-byte boundary and the design counts one
extra line per job for an unaligned start.

## The instruction-count miss

The compiler's listing of the inner loop holds, per 4 K steps of one lane:

| Operation | Groups |
| --- | --- |
| Multiply-adds (`imadd32`) | 64 |
| Signed extractions (`ibfe`) | 32 |
| Selects that the compiler adds after each extraction for a zero field width | 32 |
| Register moves | 42 |
| 64-bit address adds, loads and load waits (`add64_32`, `ld`, `wdf`) | 36 |
| Loop control | 11 |

The design's 1.75 assumed one group per extraction and no moves. The pinned
compiler translates every signed field extraction, including those written as
two shifts, as `ibfe` followed by a select on the field width, even when the
width is the constant 8 or 16. Four rewrites were tried and rejected:
extraction with `mul_hi` and pointers incremented per load stream both added
moves, a separate per-row product sum added multiply-adds, and a 64-bit
accumulator did not compile within two minutes. Keeping the 64-bit base of
each load stream in a register cut each address computation from two groups
to one; the loop above is the shortest found.

With the measured counts, the time model's best placement keeps more channels
on the CPU: at 2,002 MHz the linear stage takes 0.535 s against 0.396 s, and
it no longer fits the 0.4435 s pipeline goal. Two changes
would reduce the count, and each needs board evidence first. Loads of 16
bytes would take four words per instruction once the board shows their
alignment rule. Shift amounts passed at run time would reduce the extraction
to one shift where the field is the top of its word; a first attempt put the
shift amounts in scratch memory, which the profile forbids.

## Left-out changes, with measured constants

At 2,002 MHz on the A76 pair, the best channel split saves 0.33% with a
second job in flight, 0.02% with the GPU share's read-out on both cores and
0.00% with a larger CPU-channel share on core 7. Each stays below the
design's 2% rule.

## Images

| Target | Base `.text` | Change `.text` |
| --- | --- | --- |
| GPU service model | 148,480 | 149,248 |
| Compute service model | 184,440 | 185,464 |
| Free-list service model | 151,504 | 152,528 |
| IRQ model | 147,712 | 148,736 |
| Memory model | 153,776 | 154,800 |
| Power service model | 153,680 | 154,704 |
| GPU render application | 290,272 | 290,968 |

`pvr_compute_job.o` grows by 876 bytes because its profile-three PDS and CDM
encoders, fence program and firmware command became out-of-line functions
that the mimus session shares; with section alignment the images grow by 696
to 1,024 bytes. `.rodata` is unchanged apart from 64 fewer bytes in the
render application and 16 more in the compute model. The compiled-package
bytes equal Mesa's for profiles two and three, the fixed job's 267,368 bytes
equal both Mesa's reference and the earlier evidence, and the compute
service's three emulator cases pass at both commits.

## Evidence

Raw outputs are under `~/.cache/sunstoneos/build/gpu-mimus-compute/evidence/`:
`baseline/` (base images, time model and package check), `program/`,
`layers/` (with `calls.txt`), `readout/`, `package/`, `service/`,
`jobcheck/job/`, `time-model-design.jsonl` and `time-model-measured.jsonl`.
