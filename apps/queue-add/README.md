# Vector addition through the GPU queue

This application requires the GPU and adds two vectors of 65 unsigned 32-bit
words through the SunstoneOS application interface. The default build submits
one job. Defining `QUEUE_ADD_JOBS` selects one to eight serial jobs on one
context and program. Each job finishes and passes verification before the next
submission. A parked submission waits up to 500 scheduler ticks, yielding one
tick between attempts while the session remains live. Other submission results
stop the application. The input values exercise unsigned wraparound, and the third
workgroup uses one of its 32 lanes.

Every job rotates the input vectors by one element. The application verifies
all 65 outputs against that job's rotated values, which rejects stale output
from its predecessor. Every job has the same expected checksum, 8,840.

The manifest grants separate input and output regions. The CPU fills the input;
the GPU writes the output. One context holds one program, two leases and at
most one outstanding job.
The shader admits indices below 65. Its two reads span the 520 initialized
input bytes, and its stores span 260 output bytes. Binding zero supplies the
output address. The queue puts binding one's input
address before the four-byte element count in the shader's argument block.

`main.c` includes `program.h`, so the image needs only `main.c` and the manifest's
generated sources. The application kit must generate `queue_add-app.h` from
`manifest.json`. Select the OS GPU service when building the image.

The OpenCL source is `vector_add.cl`. `program.h` contains its compiled PowerVR
Unified Shading Cluster instructions and a `gpu_queue_program` descriptor.
[sources.json](sources.json) records the pinned Mesa revision, compiler and
program hashes, resource metadata and source license. Mesa runs on the host;
the generated instructions run on the GPU. The source and descriptor are
available under the MIT license.

Reproduce the descriptor with `apps/gpu-program/compile.py compile --profile 3
--queue-program`, supplying `--source apps/queue-add/vector_add.cl`, the pinned
Mesa source and build directories, the Ninja executable and an output directory.
The output's `program.h` must match this application's descriptor.

A submission failure records `0x400 | (sequence << 4) | result` in the retained
application fault. The low four bits identify the public queue result; the next
four bits identify the job. This distinguishes a parked device from a malformed
job, a busy slot or a lost service.

The result is 48 bytes of little-endian words:

| Byte offset | Field | Successful eight-job result |
| --- | --- | --- |
| 0 | Completed jobs | 8 |
| 4 | Elements per job | 65 |
| 8 | Last job's checksum | 8,840 |
| 12 | Last completion sequence | 8 |
| 16 | Eight `kick_to_done_us` values | GPU worker's measured durations in microseconds |

The one-job result records one duration and leaves seven durations zero. The
GPU worker measures each duration from its kick to its observed timeline update.
Submit-to-kick timing requires separate coordinator instrumentation.

Build the returning eight-job image with `SUNSTONE_APPLICATION=queue-add`,
`SUNSTONE_A733_OS_GPU=ON`, `SUNSTONE_A733_OS_CPU_RETURN=ON` and
`CMAKE_C_FLAGS=-DQUEUE_ADD_JOBS=8`. The manifest selects the A55 application
cluster.
The coordinator must supply the image's ordinary dependency paths and supervised
session inputs. A board run also requires the generic application's launcher and
result-capture integration, then the coordinator's `tools/dev-run.py` path.
This application adds no launcher or physical launch authority.

`checks/queue-add.py --build DIR` compiles the default and eight-job variants
for the host and AArch64, then runs 54 checks. Its sanitizer fixtures check the
lease, argument and dispatch records, model copied output and inject refusals,
timeout, corruption, stale output and completion faults. Later-job injections
check the exact number of submissions, waits and closes before failure.
Parked cases cover readiness after three ticks, a persistent park, session
revocation, refusal after parking and a deadline across tick-counter wrap.
Compile-time refusals cover job counts zero and nine. `checks/gpu-queue-program.py --build DIR`
checks descriptor reproduction, malformed metadata and code bounds. These host
checks establish application behavior and descriptor format. Physical shader
execution remains unqualified.
