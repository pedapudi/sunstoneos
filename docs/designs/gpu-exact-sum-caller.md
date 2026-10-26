# Run two exact-sum jobs through the returning GPU caller

Status: implemented for offline preparation. Physical exact-sum execution and
return remain unqualified. The firmware context, command ring, timeline and
cleanup reuse the caller that completed eight 32-lane arithmetic jobs on the
board. Its corrected shutdown requires the existing idle proof before OFF.

The `--fw-mimus` variant of `apps/gpu-power-id/build.py` runs the linked program
from `drivers/gpu/pvr_compute_mimus_usc.c` through the existing session API.
It submits two jobs on one context: K = 512 and K = 516, each with 33 rows and
17 channels. The four workgroups per job cover padded rows and channels.
Weights alternate between channels with −128 and 127. Activation rows alternate
between 32,767 and −32,767. The second job produces both positive and negative
sums outside int32. The caller compares all 1,122 sums against scalar int64
products and checks all 128 lane tags per job.

## Memory and submission

The returning image retains its 18 MiB reservation. The caller owns 16 KiB of
weights, 128 KiB of activation storage, 128 KiB of output, a 4 KiB job page and
8 KiB of program pages. Their 71 GPU mappings use 4 KiB pages. Every CPU page
must translate to its own physical address with write-back attributes before
publication. GPU input and program pages are read-only; output bypasses the
GPU system-level cache, as in the qualified arithmetic caller.

The session configuration accepts optional `arena_bytes` and `output_bytes`.
Zero selects the production 12 MiB and 2 MiB regions. Explicit extents must be
page aligned and within those maxima. CPU bases remain 2 MiB aligned. Open
checks overlap using the effective extents; submit checks activation extent
and output capacity before writing arguments or advancing the sequence.
The compiled submit path makes ten comparisons before its first store, within
the existing limit of twelve. The additional output-capacity comparison adds no
cache operation or device access to a production job.

The diagnostic owns CPU-writable output. Before each job, it writes guards
outside the tag and sum ranges and stale tags inside the tag range. It verifies
that those tags cannot complete the next sequence, then publishes the output.
After the kernel's timeline arrives and its kick is accepted, the session
acquires the output. The caller checks tags, sums and guards before another
job can modify any input or output. The production OS's read-only output
mapping remains a separate qualification requirement.

The caller retains the existing context programs, fixed arithmetic allocations
and builders. This costs approximately 360 KiB of additional retained backing;
removing that backing would require another context-construction path. The
exact-sum caller introduces no allocator, worker, interrupt service or firmware
transport. It reuses the existing MMUCACHE command before its first kick and
the existing context cleanup after completion.

## Bounds and failure handling

Both jobs share the existing 200 ms job budget, including a 50 ms cleanup
reserve. Another job begins only with at least 10 ms remaining before that
reserve. The caller stops at the first missing timeline, tag mismatch, numerical
mismatch, damaged guard, transport failure or firmware event. It retains all
backing. Context cleanup requires the existing completed-timeline, reset or
never-kicked condition; shutdown independently requires the idle proof before
power-off. No failure starts another job.

The kernel, linked USC bytes and existing session cache sequence are unchanged.
This is a functional qualification caller. It makes no throughput or latency
claim for recognition and does not exercise the engine hook, multicore input
publication or concurrent accelerators.

## Focused checks and physical acceptance

`checks/gpu-mimus-layers.py` covers the smaller-extent refusals and the default
session's parity with mimus `9c57286b7`. Refused jobs preserve job bytes,
sequence and state. Its GPU model now decodes pointer constants low word first,
matching the encoder used by the qualified arithmetic caller.

`apps/gpu-power-id/mimus-check.py` executes the linked AArch64 caller with the
existing platform and firmware model. Its GPU substitute reads the actual
client commands, mappings and argument records, then writes scalar integer
results. Cases cover successful narrow and wide jobs, a missing tag, a wrong
sum, a damaged guard and a missing timeline. Those cases establish caller
behavior with modeled hardware effects; they do not execute USC instructions.

Build and check offline:

```sh
PY=~/.cache/sunstoneos/venv/bin/python
$PY apps/gpu-power-id/build.py --fw-mimus --output IMAGE
$PY apps/gpu-power-id/mimus-check.py --image IMAGE/image.elf --output CHECK
```

A coordinator may propose one supervised RAM-only run after accepting the
frozen image, current board state and the returning caller's existing authority
requirements. Require the `GPU exact sums` record to report two jobs, 1,122
sums and zero bad tags, sums and guards. Also require context cleanup status 1,
the exact-sum COMPLETE line, status zero and normal CPU return under the existing
18 MiB RAM disposition contract. Any failure ends the attempt. A retained powered GPU
requires the existing recovery procedure before another GPU run.


The existing bundle producer admits `fw-mimus` only with the exact image identity
and all five caller cases. It copies the specialized host wrapper together with
the returning-kernel runner. The wrapper verifies the binary digest before
launch and validates the numerical, cleanup and shutdown records before the
runner publishes success. Duplicate or malformed result lines fail admission.
Report rejection preserves the runner's existing retained-failure policy and
records application rejection separately from transport failure. This adds no
DRAM backup or restoration operation.

`apps/gpu-power-id/mimus-host-check.py` uses the preserved native-model log for
parser and bundle checks. It executes the actual returning runner entry with
modeled transport for the default caller, accepted output and rejected output.
Rejection must leave completion false and transport failure false, and must
reach the existing retention and quarantine handlers.
