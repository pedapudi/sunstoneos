#!/usr/bin/env python3
"""Execute the exact-sum caller's AArch64 instructions with modeled GPU results.

Reuses the firmware-job platform model. Parses the caller's client commands,
GPU mappings and argument record, then computes integer sums on the host.
USC execution, device cache effects, power transitions and ROM return are not
physical evidence. Cases cover successful narrow/wide jobs, a missing tag,
a wrong sum, a damaged guard, and a missing timeline. No device access.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('gpu_job_check', Path(__file__).with_name('check.py'))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def gpu(u, state, case, image, kick, put):
    context, end, mask = struct.unpack_from('<III', kick, 8)
    assert context == base.JOB_ADDRESS[0] and mask == 1023
    state['kicks'] = state.get('kicks', 0) + 1
    sequence = state['kicks']
    maps = [struct.unpack('<QQQII', bytes(u.mem_read(image.address('job_maps') + 32 * i, 32)))
            for i in range(159)]

    def translate(address, size, write=False):
        data = []
        while size:
            matches = [m for m in maps if m[0] <= address < m[0] + m[2]]
            assert len(matches) == 1, hex(address)
            va, pa, length, shift, flags = matches[0]
            assert shift == 12 and not (write and flags & 1)
            n = min(size, va + length - address)
            data.append((pa + address - va, n))
            address += n
            size -= n
        return data

    def read(address, size):
        return b''.join(bytes(u.mem_read(pa, n)) for pa, n in translate(address, size))

    def write(address, value):
        offset = 0
        for pa, n in translate(address, len(value), True):
            u.mem_write(pa, value[offset:offset+n])
            offset += n

    cursor = int.from_bytes(u.mem_read(base.JOB_OBJECTS[2] + 4, 4), 'little')
    while cursor != end:
        kind, size = struct.unpack('<II', bytes(u.mem_read(base.JOB_OBJECTS[3] + cursor, 8)))
        payload = bytes(u.mem_read(base.JOB_OBJECTS[3] + cursor + 40, size))
        state['client_commands'].append(kind - base.MAGIC)
        if kind == base.MAGIC | 0x8000 | 205:
            assert size == 96
            cdm = struct.unpack_from('<Q', payload, 40)[0]
            assert cdm == 0xda001e0000 + 288
            weights, codes, shape, rows, strides, seq = struct.unpack('<QQIIII', read(cdm - 32, 32))
            k, channels = shape & 65535, shape >> 16
            ws, xs = strides & 65535, strides >> 16
            assert (k, channels, rows, seq) == (512 if sequence == 1 else 516, 17, 33, sequence)
            assert (ws, xs) == (k, 2*k)
            assert any(op == 0 and address == image.address('mimus_weights') and length == 16384
                       for op, address, length, _ in state['cache'])
            width = 8 if k > 512 else 4
            output = 0x46c00000
            for r in range(rows):
                x = struct.unpack('<' + 'h'*k, read(codes + r*xs, 2*k))
                for n in range(channels):
                    w = struct.unpack('<' + 'b'*k, read(weights + n*ws, k))
                    value = sum(a*b for a, b in zip(x, w))
                    if case == 'sum' and r == n == 0: value += 1
                    write(output + 65536 + (r*channels+n)*width, value.to_bytes(width, 'little', signed=True))
            for i in range(128):
                if case != 'tag' or i != 17:
                    write(output + 65536 - 128*4 + i*4, seq.to_bytes(4, 'little'))
            if case == 'guard': write(output + 65536 + rows*channels*width, bytes(4))
        elif kind == base.MAGIC | 213:
            address, value = struct.unpack('<II', payload)
            assert address == base.JOB_ADDRESS[5] and value == sequence
            if case != 'timeline': state['pending_timeline'] = [value, 3]
        else:
            raise AssertionError(hex(kind))
        cursor = (cursor + 40 + size) & mask
    put(base.JOB_OBJECTS[2] + 4, cursor)
    return False


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reuse', type=Path, help='classify preserved captures from this exact image without replay')
    p.add_argument('--case', choices=['good', 'tag', 'sum', 'guard', 'timeline'], action='append')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    image = base.harness.Image(a.image, base.NAMES)
    base.fws_job_gpu = gpu
    digest = base.hashlib.sha256(a.image.read_bytes()).hexdigest()
    if a.reuse:
        assert json.loads((a.reuse / 'results.json').read_text())['image_sha256'] == digest
    results = []
    for case in a.case or ['good', 'tag', 'sum', 'guard', 'timeline']:
        state = json.loads((a.reuse / (case + '.json')).read_text()) if a.reuse else \
            base.run_fws(image, case, stop=True, unchecked=True, job=True)
        (a.output / (case + '.json')).write_text(json.dumps(state, indent=2) + '\n')
        text = ''.join(state['messages'])
        assert 'exception' not in state and state.get('kicks') == (2 if case == 'good' else 1), (state['failure'], text[-3000:])
        assert not state.get('refused'), state.get('refused')
        calls = state['calls']
        order = [calls.index(name) for name in ('a733_gpu_platform_off_begin',
                 'a733_gpu_platform_off_poll', 'a733_gpu_platform_supply_end', 'bb_power_park')]
        assert order == sorted(order) and len(set(order)) == 4, order
        if case == 'good':
            assert state['exit_status'] == 0 and state['failure'] is None, text[-3000:]
            assert 'GPU exact sums jobs=2 sums=1122 bad_tags=0 bad_sums=0 bad_guards=0' in text
            assert 'GPU FWJ cleanup status=0x1 ' in text
        else:
            assert state['exit_status'] is None and state['failure'] == \
                'GPU exact sums: not complete, GPU OFF and provider parked', text[-3000:]
            assert f'GPU FWJ job complete=0 outcome={2 if case == "timeline" else 3}' in text
            if case != 'timeline':
                assert f'bad_{"tags" if case == "tag" else "sums" if case == "sum" else "guards"}=1' in text
            assert ('GPU FWJ cleanup status=0x1 ' in text) == (case != 'timeline')
        results.append({**state, 'cycles': 1})
    (a.output / 'results.json').write_text(json.dumps({'scope': __doc__, 'image_sha256': digest,
        'physical_access': False, 'variant': 'fw-mimus', 'cases': results}, indent=2)+'\n')
    print('PASS exact-sum native caller:', ', '.join(x['case'] for x in results))


if __name__ == '__main__':
    main()
