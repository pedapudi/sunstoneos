# Run two exact-sum jobs through the returning GPU caller

Status: **open**. The offline qualification caller, its `--fw-mimus` build variant of `apps/gpu-power-id/build.py` and its host checks are not in the tree: the application-specific GPU session left the OS, and the exact-sum shader and job adapter belong to the mimus checkout ([gpu-mimus-service.md](../gpu-mimus-service.md)). The host results of the earlier caller are in [gpu-exact-sum-caller.jsonl](../evidence/gpu-exact-sum-caller.jsonl). Physical exact-sum execution and return through a returning caller are unqualified.

## Purpose and workload

A functional qualification caller runs the linked exact-sum program through the session API of the returning GPU image, which reuses the firmware context, command ring, timeline and cleanup of the caller that completed eight 32-lane arithmetic jobs on the board. Its corrected shutdown requires the idle proof before OFF.

The workload is two jobs on one context: K = 512 and K = 516, each with 33 rows and 17 channels and four workgroups that cover padded rows and channels. Weights alternate between -128 and 127, and activation rows alternate between 32,767 and -32,767, so the second job produces positive and negative sums outside int32. The caller compares all 1,122 sums with scalar int64 products and checks all 128 lane tags per job.

## Memory and submission

- The returning image keeps its 18 MiB reservation. The caller owns 16 KiB of weights, 128 KiB of activation storage, 128 KiB of output, a 4 KiB job page and 8 KiB of program pages, mapped by 71 GPU mappings of 4 KiB pages. Every CPU page translates to its own physical address with write-back attributes before publication. GPU input and program pages are read-only, and output bypasses the GPU system-level cache.
- The session configuration accepts optional `arena_bytes` and `output_bytes`; zero selects the production 12 MiB and 2 MiB regions. Explicit extents are page aligned and within those maxima, CPU bases stay 2 MiB aligned, open checks overlap with the effective extents, and submit checks the activation extent and output capacity before it writes arguments or advances the sequence.
- The diagnostic owns CPU-writable output. Before each job it writes guards outside the tag and sum ranges and stale tags inside the tag range, verifies that those tags cannot complete the next sequence, and publishes the output. After the kernel's timeline arrives and the kick is accepted, the session acquires the output, and the caller checks tags, sums and guards before another job can modify any input or output. The production read-only output mapping is a separate qualification.
- The caller keeps the context programs, fixed arithmetic allocations and builders, which costs about 360 KiB of retained backing. It adds no allocator, worker, interrupt service or firmware transport.

## Bounds and failure handling

Both jobs share the 200 ms job budget, including a 50 ms cleanup reserve, and another job begins only with at least 10 ms remaining before that reserve. The caller stops at the first missing timeline, tag mismatch, numerical mismatch, damaged guard, transport failure or firmware event, and retains all backing. Context cleanup requires the completed-timeline, reset or never-kicked condition, and shutdown independently requires the idle proof before power-off. No failure starts another job. The kernel, linked USC bytes and session cache sequence are those of the standalone exact-sum program, so the caller makes no throughput or latency claim and does not exercise the engine hook, multicore input publication or concurrent accelerators.

## Open work

1. Provide a qualification caller in the mimus checkout that runs the workload above, with host checks that execute the linked caller against a model of the platform and firmware, covering narrow and wide jobs, a missing tag, a wrong sum, a damaged guard and a missing timeline.
2. Run it once, supervised and RAM-only, after a coordinator accepts the frozen image, the board state and the returning caller's authority requirements. Require the `GPU exact sums` record to report two jobs, 1,122 sums and zero bad tags, sums and guards, context cleanup status 1, the exact-sum COMPLETE line, status zero and a normal CPU return under the 18 MiB RAM disposition contract. Any failure ends the attempt, and a retained powered GPU needs the recovery procedure before another GPU run. The prediction is the record `gpu-exact-sum-caller-physical`.
