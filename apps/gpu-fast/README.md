# Measure GPU queue latency and throughput

The returning benchmark uses the existing vector-add shader with element count
zero. Firmware executes the ordinary compute and fence commands; every shader
lane exits before input or output access. The zero-job input opens and closes
the same context as a control.

Select `SUNSTONE_APPLICATION=gpu-fast`, `SUNSTONE_A733_OS_GPU=ON` and
`SUNSTONE_GPU_FAST_TIMING=ON` through the
cached configuration helper. The target is `sunstone-a733-os-gpu-fast`.

`prepare.py --jobs 0|10000 --cache writeback|uncached --output DIRECTORY` writes
the 16-byte input. Submit through the board
queue, with `--kind cpu-return`, `--input DIRECTORY/input.bin`,
an admitted GPU template and this checkout's
`apps/gpu-fast/host.py` as `--decoder`, with `--decode-application`.
Run the zero-job control before the measured input.

The measured input warms 64 jobs, records 10,000 serial round trips and submission
wall times, then keeps four jobs queued across another 10,000 completions.
The result contains counter-tick distributions and pipeline elapsed time.
`host.py` reports microseconds and jobs per second. Submission wall time includes
preemption and measurement overhead; it bounds CPU time from above.
IRQ entry to wait return also includes output collection and bounds the time
until the waiting task runs. A second distribution ends immediately after the
GPU notification returns, before collection. It contains only waits that block.
A zero IRQ timestamp identifies a fence acquired
by slow event handling and does not enter that distribution.

The benchmark owns all optional timing. Disabled production builds omit the
IRQ and wake timestamp fields and counter reads. The cache option fixes the payload map
at context creation; application buffer mappings and cache policy remain fixed.
