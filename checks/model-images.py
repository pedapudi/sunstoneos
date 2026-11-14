#!/usr/bin/env python3
"""Link every host model image that shares workloads with the OS images, once per tree state.

The latency, partitioned latency, latency control and latency cost models and the CPU reliability model are bare-metal AArch64
images that the OS images do not contain: they link the same workload sources (apps/latency, apps/latency-cost,
apps/cpu-runtime-qualification) against their own platform code, so a change to a kit or SDK function that the workloads call
(include/sunstone_app.h, apps/os/application.c) can break their link while every OS image builds. The gate builds each image
from its recipe in tools/gate_images.py and this check confirms that the build left a valid AArch64 ELF. The check covers the
sources of those images through the build graph (checks/check-index.py), so a change to any file one of them compiles selects it.
Usage: model-images.py --output DIR --elf PATH [--elf PATH ...]
"""
import argparse
from pathlib import Path
import struct

GATE_RUNS = [['--output', '{output}',
              '--elf', '{elf:latency-models}',
              '--elf', '{build:latency-models}/latency-partitioned-writes-model.elf',
              '--elf', '{build:latency-models}/latency-partitioned-loads-model.elf',
              '--elf', '{build:latency-models}/latency-partitioned-pmu-model.elf',
              '--elf', '{elf:latency-cost-model}',
              '--elf', '{elf:cpu-reliability-model}']]
EM_AARCH64 = 183


def verify(path):
    data = path.read_bytes()[:20]
    if len(data) < 20 or data[:4] != b'\x7fELF' or data[4] != 2:
        raise SystemExit('%s: not a 64-bit ELF' % path)
    machine = struct.unpack_from('<H', data, 18)[0]
    if machine != EM_AARCH64:
        raise SystemExit('%s: machine %d, not AArch64' % (path, machine))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--elf', type=Path, action='append', required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for path in args.elf:
        verify(path)
    (args.output / 'images.txt').write_text(''.join(str(path) + '\n' for path in args.elf))
    print('PASS: %d model images linked' % len(args.elf))


if __name__ == '__main__':
    main()
