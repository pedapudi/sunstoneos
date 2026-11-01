# Vector addition through the GPU queue

This application requires the GPU and submits one job through the SunstoneOS
application interface. It adds two vectors of 65 unsigned 32-bit words, checks
every result and returns a 16-byte record containing the element count, checksum,
job sequence and a zero reserved word. The expected checksum is 8,840. The input
values exercise unsigned wraparound, and the third workgroup uses one of its
32 lanes.

The manifest grants separate input and output regions. The CPU fills the input;
the GPU writes the output. One context holds one program, two leases and one job.
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

`checks/queue-add.py --build DIR` compiles this application for the host and
AArch64, then runs ten sanitizer cases. Its queue callbacks check the lease,
argument and dispatch records, model copied output and inject refusals, timeout,
corruption and completion faults. `checks/gpu-queue-program.py --build DIR`
checks descriptor reproduction, malformed metadata and code bounds. These host
checks establish application behavior and descriptor format. Physical shader
execution remains unqualified.
