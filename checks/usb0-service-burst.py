#!/usr/bin/env python3
"""Build and run the host model of the shared USB0 service loop.

The model compiles apps/board/usb0_service.h with modeled TinyUSB vendor
streams, a polled controller and time (checks/usb0-service-burst.c). Before
the shared loop, every application serviced at most one packet per direction
per 1 ms tick. No hardware is accessed."""
import argparse, hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tinyusb', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='clang')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    model = args.output / 'usb0-service-burst'
    subprocess.run([args.compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(ROOT / 'drivers/usb'),
                    '-I' + str(args.tinyusb / 'src'), str(ROOT / 'checks/usb0-service-burst.c'),
                    '-o', str(model)], check=True)
    cases = {row['case']: row for row in map(json.loads, subprocess.run(
        [str(model)], check=True, capture_output=True, text=True).stdout.splitlines())}
    budget_us = 200 + 10  # one pass may start just before the budget expires

    big = cases['response-64k']
    assert big['transferred'] == 65536 and big['packets'] == 128 and not big['stopped']
    assert big['ticks'] <= 16 and big['max_tick_packets'] >= 8, big  # previously 128 ticks
    request = cases['request-2064']
    assert request['received'] == 2064 and request['packets'] == 5 and request['ticks'] <= 2, request
    frame = cases['response-1160']
    assert frame['transferred'] == 1160 and frame['ticks'] == 1, frame  # previously 3 ticks
    slow = cases['slow-host']
    assert slow['transferred'] == 4096 and slow['ticks'] <= 9, slow
    idle = cases['idle']
    assert idle['max_tick_passes'] == 1 and idle['passes'] == idle['ticks'], idle
    abandoned = cases['abandoned-read']
    assert abandoned['last_tick_passes'] == 1 and abandoned['transferred'] == 0, abandoned
    for row in cases.values():
        assert row['max_tick_busy_us'] <= budget_us and row['max_tick_passes'] <= 64, row
    result = dict(passed=True, physical_access=False,
                  model_sha256=hashlib.sha256((ROOT / 'checks/usb0-service-burst.c').read_bytes()).hexdigest(),
                  header_sha256=hashlib.sha256((ROOT / 'apps/board/usb0_service.h').read_bytes()).hexdigest(),
                  cases=list(cases.values()),
                  limit='Modeled TinyUSB stream, controller events and host turnaround; physical throughput is unmeasured.')
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    for row in cases.values():
        print(f"{row['case']}: {row['ticks']} ticks, {row['packets']} packets, "
              f"max {row['max_tick_packets']} packets and {row['max_tick_busy_us']} us per tick")
    print('PASS: shared USB0 service bursts are bounded and move several packets per tick')


if __name__ == '__main__':
    main()
