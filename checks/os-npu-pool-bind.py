#!/usr/bin/env python3
"""The packaging step sets the session's NPU pool from the pool symbols of the image.

Links small images with the pool symbols of the legacy pool (8 MiB at 0x47400000) and of a pool sized from the declaration
(22 MiB ending at 0x47000000), and checks apps/os/host.py bind_npu_pool: the record's pool_start and pool_bytes follow the
symbols whatever the template said, nothing else in the record changes, an image without a pool leaves the services alone,
a partial or misaligned pool is refused, and apps/os/prepare.py puts the bound record into the session it writes.
"""
import argparse, importlib.util, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded)
    return loaded


host = module('pool_bind_host', ROOT / 'apps/os/host.py')
rule = module('pool_bind_rule', ROOT / 'apps/os/cpu-return-host.py')
(out / 'start.c').write_text('void start(void) {}\n')
subprocess.run(['clang-21', '--target=aarch64-none-elf', '-c', str(out / 'start.c'), '-o', str(out / 'start.o')], check=True)


def link(name, symbols):
    options = ['--defsym=%s=%#x' % item for item in symbols.items()]
    subprocess.run([str(harness.LLD), '--entry=start', *options, str(out / 'start.o'), '-o', str(out / name)], check=True)
    return (out / name).read_bytes()


template = bytearray(host.SERVICE_BYTES[host.NPU])
struct.pack_into('<QQ', template, host.NPU_POOL_OFFSET, 0x47400000, 0x800000)   # what every earlier template said
template[100] = 0x5a                                                              # a byte elsewhere in the record
services = {host.NPU: bytes(template), host.THERMAL: b'thermal'}
cases = []
for label, symbols in (('legacy 8 MiB', {'__os_npu_pool_start': 0x47400000, '__os_npu_pool_end': 0x47c00000}),
                       ('sized 22 MiB', {'__os_npu_pool_start': 0x45a00000, '__os_npu_pool_end': 0x47000000})):
    image = link(label.split()[0] + '.elf', symbols)
    bound = host.bind_npu_pool(image, services)
    start, size = struct.unpack_from('<QQ', bound[host.NPU], host.NPU_POOL_OFFSET)
    assert (start, start + size) == (symbols['__os_npu_pool_start'], symbols['__os_npu_pool_end']), label
    assert rule.npu_pool_extent_valid(start, start + size), label
    assert bound[host.NPU][:host.NPU_POOL_OFFSET] == services[host.NPU][:host.NPU_POOL_OFFSET]
    assert bound[host.NPU][host.NPU_POOL_OFFSET + 16:] == services[host.NPU][host.NPU_POOL_OFFSET + 16:]
    assert bound[host.THERMAL] == services[host.THERMAL] and services[host.NPU] == bytes(template)
    cases.append(label)
# No pool symbols: unchanged. Only one of them: refused. Not whole blocks, or above the RAM EL0 reaches: refused.
plain = link('plain.elf', {})
assert host.bind_npu_pool(plain, services) == services
for label, symbols in (('partial', {'__os_npu_pool_start': 0x47400000}),
                       ('unaligned', {'__os_npu_pool_start': 0x47401000, '__os_npu_pool_end': 0x47c01000}),
                       ('above the reachable RAM', {'__os_npu_pool_start': 0x48000000, '__os_npu_pool_end': 0x48800000})):
    image = link(label.replace(' ', '-') + '.elf', symbols)
    try:
        host.bind_npu_pool(image, services)
    except ValueError:
        cases.append('refused: ' + label)
    else:
        raise AssertionError(label + ' was accepted')
print('PASS the session pool follows the image symbols:', ', '.join(cases))
