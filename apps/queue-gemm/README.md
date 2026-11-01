# Tiled FP32 GEMM through GPU local memory

This returning GPU application runs four 13-by-11-by-19 matrix products with
signed, exact integer-valued FP32 inputs. Each 32-lane workgroup cooperatively
uses 384 bytes of on-chip local memory and reaches both barriers on every depth
tile. It verifies every result, changed-input sequence, and surrounding guard.
Physical execution is unqualified.

The manifest supplies separate input and output leases. The shader reads the
input pointer followed by M, N, and K from its argument block. Input stores
row-major A followed by B. Binding zero points 64 bytes into the output lease.
The application checks sixteen words before and after the output against their
values before context creation.

`program.h` contains the compiled USC instructions and immutable local-memory
descriptor. `sources.json` records source, compiler, adapter, and binary hashes,
resource metadata, license, and execution profile. Source and compiled program
are available under MIT. Mesa runs on the host; generated instructions run on
the GPU. The [design](../../docs/designs/gpu-compute-local-memory.md) records the
resource and barrier contract.

Reproduce the compiler output using the pinned Mesa source and build, explicit
Ninja executable, and an external build directory:

```sh
python3 apps/gpu-program/compile.py compile --profile 4 --queue-program \
  --source apps/queue-gemm/tiled_gemm.cl --mesa "$MESA_SOURCE" \
  --build "$MESA_BUILD" --ninja "$NINJA" --output "$PROGRAM_BUILD"
python3 apps/gpu-program/compile.py compile --profile 3 --queue-program \
  --source apps/queue-gemm/direct_gemm.cl --mesa "$MESA_SOURCE" \
  --build "$MESA_BUILD" --ninja "$NINJA" --output "$CONTROL_BUILD"
python3 checks/gpu-local.py --build "$HOST_CHECK_BUILD" \
  --producer "$PROGRAM_BUILD" --control-producer "$CONTROL_BUILD" \
  --mesa-source "$MESA_SOURCE" \
  --mesa-build "$MESA_BUILD" --ld "$LLD"
```

The direct control uses the same inputs, outputs, six workgroups, four jobs, CPU reference,
guards, and result contract. Each lane reads its own operands from global memory.
Its descriptor has zero local words and zero barriers.

`LLD` must name the configured AArch64 linker as `ld.lld`. The host check executes
the unchanged OpenCL shader on synchronized threads, checks real compiler
metadata and Mesa command bytes, and exercises application faults. It does not
execute USC instructions.

Configure through `~/.cache/sunstoneos/bin/cfg.sh` with
`SUNSTONE_APPLICATION=queue-gemm`, `SUNSTONE_A733_OS=ON`,
`SUNSTONE_A733_OS_GPU=ON`, `SUNSTONE_A733_OS_CPU_RETURN=ON`,
`SUNSTONE_A733_OS_CPU_CLOCK=ON`, and `A733_A76_ADMITTED=ON` with the existing
supervised dependencies. Build target `sunstone-a733-os-queue-gemm`.
The coordinator must qualify independent FP32 arithmetic first. Then build and
run the direct control with `SUNSTONE_GPU_GEMM_CONTROL=ON`. Build and run the
local-memory case with that option `OFF` after the direct control passes.
Both use the ordinary application launcher and `tools/dev-run.py`.

`host.py` decodes the 48-byte result. Its twelve unsigned little-endian words
are completed jobs (4), rows (13), columns (11), depth (19), last checksum,
sequence (4), local bytes (384 for local memory or 0 for control), checked guard words (32), and four measured
kick-to-timeline durations. Each duration must be below the 200,000-microsecond
job budget. Per-element comparisons occur before the application publishes
the result. Failure codes 5, 6, 7, and 8 identify completion, numerical,
close, and guard failures. Submission failures retain the existing public
queue result in `0x400 | (sequence << 4) | result`.
