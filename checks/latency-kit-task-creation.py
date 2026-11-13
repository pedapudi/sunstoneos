#!/usr/bin/env python3
"""Check that the board latency application creates its tasks through the kit.

The application's init hook creates the latency tasks. A direct FreeRTOS
creation there has no address space to take, and the OS fails the session with
detail 0x7a1 (include/os_space.h). apps/latency/model.c is the QEMU model's
forwarder and is the one file allowed to call FreeRTOS directly."""
import argparse
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/latency']
DIRECT = re.compile(r'\bxTaskCreate\w*\s*\(')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    found = []
    for source in sorted((ROOT / 'apps/latency').glob('*.c')):
        if source.name == 'model.c':
            continue
        for number, line in enumerate(source.read_text().splitlines(), 1):
            if DIRECT.search(line):
                found.append(f'{source.relative_to(ROOT)}:{number}: {line.strip()}')
    if found:
        raise SystemExit('direct FreeRTOS task creation in the board latency application:\n' + '\n'.join(found))
    print('PASS latency kit task creation: every board latency task is created through ss_task_create_static_affinity_set')


if __name__ == '__main__':
    main()
