#!/usr/bin/env python3
"""Construct one finite Baseline IDR with six nonconstant I_PCM macroblocks.

The separately decoded FFmpeg output must match the original planar samples.
No hardware is accessed. Outputs belong in an external evidence directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


class Bits:
    def __init__(self): self.bits = []
    def u(self, value, count):
        assert 0 <= value < 1 << count
        self.bits.extend((value >> bit) & 1 for bit in reversed(range(count)))
    def ue(self, value):
        code = value + 1
        self.u(0, code.bit_length() - 1)
        self.u(code, code.bit_length())
    def se(self, value): self.ue(2 * abs(value) - (value > 0))
    def align(self):
        while len(self.bits) % 8: self.u(0, 1)
    def finish(self):
        self.u(1, 1); self.align()
        return bytes(sum(self.bits[i+j] << (7-j) for j in range(8))
                     for i in range(0, len(self.bits), 8))


def nal(kind, rbsp):
    output = bytearray([kind]); zeros = 0
    for byte in rbsp:
        if zeros == 2 and byte <= 3: output.append(3); zeros = 0
        output.append(byte); zeros = zeros + 1 if byte == 0 else 0
    return bytes(output)


def construct():
    s = Bits()
    for value in (66, 0xc0, 10): s.u(value, 8)
    for value in (0, 0, 2, 1): s.ue(value)  # SPS, frame_num, POC type, references.
    s.u(0, 1); s.ue(2); s.ue(1); s.u(1, 1); s.u(1, 1)
    s.u(0, 1); s.u(0, 1)
    p = Bits(); p.ue(0); p.ue(0); p.u(0, 1); p.u(0, 1)
    for value in (0, 0, 0): p.ue(value)
    p.u(0, 1); p.u(0, 2)
    for value in (0, 0, 0): p.se(value)
    p.u(1, 1); p.u(0, 1); p.u(0, 1)
    planes = [bytes(16 + ((x * 7 + y * 11 + channel * 43) % 220)
                    for y in range(height) for x in range(width))
              for channel, (width, height) in enumerate(((48, 32), (24, 16), (24, 16)))]
    b = Bits(); b.ue(0); b.ue(2); b.ue(0); b.u(0, 4); b.ue(0)
    b.u(0, 1); b.u(0, 1); b.se(0); b.ue(1)
    header_bits = 8 + len(b.bits)
    for my in range(2):
        for mx in range(3):
            b.ue(25); b.align()
            for plane, stride, size in zip(planes, (48, 24, 24), (16, 8, 8)):
                for row in range(size):
                    for col in range(size): b.u(plane[(my*size+row)*stride+mx*size+col], 8)
    slice_nal = nal(0x65, b.finish())
    stream = b''.join(b'\0\0\0\1' + n for n in (nal(0x67, s.finish()), nal(0x68, p.finish()), slice_nal))
    return stream, slice_nal, b''.join(planes), header_bits


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--ffmpeg', required=True, type=Path)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    stream, slice_nal, pixels, header_bits = construct()
    for name, data in [('input.h264', stream), ('slice.bin', slice_nal), ('expected.yuv', pixels)]:
        (a.output/name).write_bytes(data)
    command = [str(a.ffmpeg), '-hide_banner', '-loglevel', 'error', '-i', str(a.output/'input.h264'),
               '-frames:v', '1', '-pix_fmt', 'yuv420p', '-f', 'rawvideo', str(a.output/'decoded.yuv')]
    (a.output/'commands.json').write_text(json.dumps([command], indent=2)+'\n')
    result = subprocess.run(command, capture_output=True)
    (a.output/'ffmpeg.log').write_bytes(result.stdout+result.stderr)
    assert result.returncode == 0 and (a.output/'decoded.yuv').read_bytes() == pixels
    record = {'profile': 'Baseline, progressive 48x32 8-bit 4:2:0, single IDR I slice, six I_PCM macroblocks',
              'header_bits': header_bits, 'width': 48, 'height': 32, 'samples': 2304,
              'files': {n: {'bytes': (a.output/n).stat().st_size,
                            'sha256': hashlib.sha256((a.output/n).read_bytes()).hexdigest()}
                        for n in ('input.h264', 'slice.bin', 'expected.yuv', 'decoded.yuv')},
              'ffmpeg_sha256': hashlib.sha256(a.ffmpeg.read_bytes()).hexdigest(), 'passed': True}
    (a.output/'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print('PASS: independent CPU decoder matches all nonconstant I_PCM samples.')


if __name__ == '__main__': main()
