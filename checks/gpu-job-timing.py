#!/usr/bin/env python3
"""Run the diagnostic producer (apps/os/gpu-timing.c) on the real FreeRTOS kernel under the host shim.

The counter is the shim's A733 counter, driven by the fixture; the request queue, the
worker task, the blocking receive and the interrupt that wakes it are the kernel's.
Device-facing callees are modeled at their boundary. The optional compile_commands.json
argument is accepted for compatibility and is not needed.
"""
import argparse
import pathlib
import subprocess
import host_rtos
from harness import environment

parser = argparse.ArgumentParser()
parser.add_argument("compile_commands", type=pathlib.Path, nargs="?")
parser.add_argument("--output", type=pathlib.Path, required=True)
parser.add_argument("--nsi", action="store_true", help="exercise optional NSI diagnostic callbacks")
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
defines = ["SUNSTONE_GPU_TIMING=1"] + (["SUNSTONE_GPU_NSI_PMU=1"] if args.nsi else [])
wraps = ["gpu_queue_submit", "gpu_queue_wait", "pvr_cccb_begin", "pvr_cccb_poll", "pvr_ccb_submit",
         "aarch64_device_cache", "xQueueReceive"]
link = ["-Wl,--wrap=" + w for w in wraps]
args.output.mkdir(parents=True, exist_ok=True)


def run(source, name):
    exe = host_rtos.build(args.output, ["checks/gpu-job-timing.c", "checks/gpu-job-timing-callees.c", source], name, defines=defines, link=link)
    return subprocess.run([str(exe)], capture_output=True, text=True, env=environment())


done = run("apps/os/gpu-timing.c", "gpu-job-timing-host")
assert done.returncode == 0, done.stderr
# Negative control: a timeout that overwrote the paired blocked-IRQ ticks must fail the check.
control = host_rtos.mutated(args.output, "apps/os/gpu-timing.c",
                            "    uint64_t begin=track?ticks():0;\n    if(track){receive_blocked=0;receive_block_tick=0;}",
                            "    uint64_t begin=track?ticks():0;\n    if(track){receive_blocked=0;receive_block_tick=0;if(active)active->public.ticks[GPU_TIMING_WORKER_BLOCK]=0;}",
                            "gpu-timing-lost-pairing.c")
assert run(control, "control-lost-pairing").returncode != 0, "control passed"
print("GPU job timing producer, lifetime, cache filter, IRQ classification and capacity passed")
