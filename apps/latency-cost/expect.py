#!/usr/bin/env python3
"""Write the complete native result expectation for the existing board runner."""
import argparse
import json


def expectation(enabled):
    words = [0] * (7784 // 4)
    full = {'min': 0, 'max': 0xffffffff}
    positive = {'min': 1, 'max': 0xffffffff}
    def u64(offset, low, high=0):
        words[offset // 4:offset // 4 + 2] = [low, high]
    u64(0, 0x4f535431, 0x53534c43)
    u64(8, 24000000)
    words[4:8] = [int(enabled), 48, 32, 255]
    u64(32, full)
    for c in range(8):
        base = 40 + c * 776
        for i in range(48):
            u64(base + i * 8, positive)
            u64(base + 384 + i * 8, {'min': 64, 'max': 0xffffffff})
        u64(base + 768, full, {'min': 0, 'max': 0xffff})
    for i in range(96):
        u64(6248 + i * 8, positive)
        u64(7016 + i * 8, {'min': 0, 'max': 3})
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--enabled', choices=('on', 'off'), required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    with open(args.output, 'x') as stream:
        json.dump(expectation(args.enabled == 'on'), stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
