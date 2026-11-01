#!/usr/bin/env python3
"""Write the CPU control's complete word expectation for the existing board runner."""
import argparse
import json

if not __debug__:
    raise SystemExit('checks require assertions')

RANGE = {'min': 0, 'max': 0xffffffff}
BUDGET_US = (50, 25, 25, 25, 25, 25, 100, 250, 250, 1000, 1000)


def expectation(dma=False):
    # workload.h pins the aggregate, phase and acquisition offsets.
    words = [0] * (57728 // 4)

    def u64(offset, low, high=0):
        words[offset // 4:offset // 4 + 2] = [low, high]

    u64(0, 0x54454e31, 0x53534c41)
    u64(8, 24000000)
    u64(16, {'min': 1, 'max': 10000})
    words[6:10] = [255, 0, 0, 1]  # cores, native, no modeled DMA, CPU0 timer.
    if dma:
        words[8] = {'min': 2, 'max': 0xffffffff}
    for core in range(8):
        traffic = 40 + core * 32
        u64(traffic, {'min': 2, 'max': 0xffffffff})
        for offset in (traffic + 8, traffic + 16, traffic + 24):
            u64(offset, RANGE, RANGE)
        for metric, budget in enumerate(BUDGET_US):
            base = 320 + core * 640 + metric * 56
            if metric >= 8 and not (dma and metric == 8 and core == 0) or (metric == 0 and core):
                continue  # An unexercised path has no samples or timestamps.
            count = 64 if metric == 6 else 128 if metric == 7 else {'min': 2 if metric == 8 else 1, 'max': 0xffffffff}
            u64(base, count)
            u64(base + 8, {'min': 0, 'max': budget * 24})
            u64(base + 16, RANGE, RANGE)
            u64(base + 24, RANGE)
            # First-over-budget context stays zero for an accepted run.
    for phase in range(3):
        for core in range(8):
            for metric, budget in enumerate(BUDGET_US):
                base = 5440 + (phase * 8 + core) * 616 + metric * 56
                if metric >= 8 and not (dma and metric == 8 and core == 0) or (metric == 0 and core):
                    continue
                if metric in (6, 7):
                    count = 64 if phase == 1 and metric == 6 else 128 if phase == 2 and metric == 7 else 0
                    if not count:
                        continue
                else:
                    count = RANGE
                u64(base, count)
                # Quantile bounds can exceed the exact budget by a bin width.
                for offset in (8, 16):
                    u64(base + offset, {'min': 0, 'max': budget * 26})
                u64(base + 24, {'min': 0, 'max': budget * 24})
                u64(base + 32, RANGE, RANGE)
                u64(base + 40, RANGE)
    for core in range(8):
        for lock in range(2):
            u64(20224 + core * 16 + lock * 8, {'min': 1, 'max': 0xffffffff})
    for phase in range(3):
        for core in range(8):
            for lock in range(2):
                u64(20352 + (phase * 8 + core) * 16 + lock * 8, RANGE)
        u64(20736 + phase * 16, RANGE, RANGE)
        u64(20744 + phase * 16, RANGE, RANGE)
    # Initialization starts at counter zero by definition.
    u64(20736, 0)
    for core in range(8):
        u64(20784 + core * 8, RANGE)
        for index in range(192):
            base = 20848 + (core * 192 + index) * 24
            u64(base, RANGE, RANGE)
            u64(base + 8, RANGE, RANGE)
            words[(base + 16) // 4] = 6 if index < 64 else 7
            words[(base + 20) // 4] = 1 if index < 64 else 2
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--dma', action='store_true')
    args = parser.parse_args()
    with open(args.output, 'x') as stream:
        json.dump(expectation(args.dma), stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
