#!/usr/bin/env python3
"""Check that the OS session admission of the storage-write CPU-return image accepts a session for it.

A storage-write image reached the board once and stopped at EL3 admission (a733_smp_stop(1, 1)) because its manifest asked
for a 300 s run in a 60 s supervised session: os_session_decode refuses a run deadline above the session lifetime minus the
return reserve, and no host check ran the decoder. This check links the image's own decoder and validator for the host
(the compiler flags come from the configured build) and runs them on a session built from the failing run's session
(checks/data/storage-write-session.bin, 1072 bytes, the runner's output for the first submission) with the manifest hash
replaced by the hash of the image's manifest section. It requires:
  accepted   the image's manifest: decode, os_session_valid and the board prefix validator all succeed;
  refused    the same manifest with run_us raised to 300 s: decode fails (the board's failure, reproduced);
  refused    the same manifest with the return reserve at the whole session: decode fails.
Usage: storage-write-admission.py --build BUILD_DIR --output DIR, where BUILD_DIR configures and builds the
sunstone-a733-os-storage-write-test target (SUNSTONE_A733_OS_CPU_RETURN, SUNSTONE_A733_OS_SD_WRITE).
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
HASH_OFFSET = 592 + 32                 # SS_SESSION_PREFIX_BYTES plus the offset of manifests_sha256 in the session header
TARGET = 'sunstone-a733-os-storage-write-test'
STUBS = """#include <stdlib.h>
#define S(n) void n(void) { abort(); }
S(a733_clock_acquire) S(a733_clock_release) S(a733_clock_retain) S(a733_pmic_decode) S(a733_pmic_reap) S(a733_pmic_result)
S(a733_pmic_submit) S(a733_pmic_submit_write) S(a733_rail_acquire) S(a733_rail_floor) S(a733_rail_poll) S(a733_rail_release)
S(a733_rail_retain) S(a733_thermal_policy_level) S(a733_thermal_policy_seen) S(board_session_sealed)
"""
MANIFEST_C = """#include "application.h"
__asm__(".section sunstone_manifest,\\"a\\"\\n.balign 8\\n.global __start_sunstone_manifest\\n__start_sunstone_manifest:\\n"
        ".incbin \\"%s\\"\\n.global __stop_sunstone_manifest\\n__stop_sunstone_manifest:\\n.text\\n");
extern const struct ss_manifest __start_sunstone_manifest[];
const struct ss_application ss_application = {__start_sunstone_manifest, 0, 0, 0, 0};
"""


def run(command, **kwargs):
    r = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    assert r.returncode == 0, f'{command[0]}: {r.stdout}{r.stderr}'
    return r.stdout


def section(elf, out):
    run(['llvm-objcopy-21', f'--dump-section=sunstone_manifest={out}', elf, '/dev/null'])
    return out.read_bytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    out = a.output; out.mkdir(parents=True, exist_ok=True)
    flags = (a.build / 'CMakeFiles/sunstone-a733-os-core.dir/flags.make').read_text()
    defs = re.search(r'^C_DEFINES = (.*)$', flags, re.M).group(1).split()
    inc = re.search(r'^C_INCLUDES = (.*)$', flags, re.M).group(1).split()
    assert '-DSUNSTONE_A733_OS_CPU_RETURN=1' in defs and '-DSUNSTONE_A733_OS_SD_WRITE=1' in defs, 'not a storage-write CPU-return build'
    elf = a.build / (TARGET + '.elf')
    manifest = section(elf, out / 'manifest.bin')
    image_dir = a.build / 'os-image' / TARGET
    cc = [harness.CLANG, '-std=gnu11', '-O1', '-g', '-w']
    build_id = next(image_dir.glob('*-build-id.c'))
    (out / 'stubs.c').write_text(STUBS)
    objects = []
    sources = [(a.build / 'os-image/return-admission.c', defs), (ROOT / 'apps/os/session.c', defs),
               (ROOT / 'drivers/storage/storage_digest.c', defs), (build_id, defs),
               (ROOT / 'drivers/clock/a733_cpu_clock.c', [d for d in defs if d != '-DSUNSTONE_CRUMBS=1']),
               (out / 'stubs.c', []), (ROOT / 'checks/storage-write-admission.c', defs)]
    for source, d in sources:
        o = out / (source.stem + '.o')
        run([*cc, *d, *inc, '-c', source, '-o', o])
        objects.append(o)

    def admit(name, raw_manifest):
        mpath = out / (name + '.manifest')
        mpath.write_bytes(raw_manifest)
        (out / (name + '.c')).write_text(MANIFEST_C % mpath)
        run([*cc, *defs, *inc, '-I' + str(ROOT / 'apps/os'), '-c', out / (name + '.c'), '-o', out / (name + '.mo')])
        binary = out / name
        run([harness.CLANG, *objects, out / (name + '.mo'), '-o', binary])
        session = bytearray((ROOT / 'checks/data/storage-write-session.bin').read_bytes())
        session[HASH_OFFSET:HASH_OFFSET + 32] = hashlib.sha256(raw_manifest).digest()
        spath = out / (name + '.session')
        spath.write_bytes(session)
        return run([binary, spath]).strip()

    # run_deadline_us and return_reserve_us are the 13th and 14th little-endian fields after the 8-byte name block; find them
    # from the decoder rather than a literal offset.
    spec = __import__('importlib.util').util.spec_from_file_location('sunstone_manifest', ROOT / 'tools/sunstone-manifest.py')
    decoder = __import__('importlib.util').util.module_from_spec(spec); spec.loader.exec_module(decoder)
    parsed = decoder.unpack(manifest)[0]
    assert parsed['run_deadline_us'] <= 55_000_000 and parsed['return_reserve_us'] == 5_000_000, parsed
    head = decoder.HEAD
    fields = list(head.unpack_from(manifest))
    names = ['magic', 'version', 'size', 'name', 'required', 'optional', 'cluster', 'workers', 'priority', 'reserved0', 'stack', 'heap',
             'run', 'reserve', 'window', 'output', 'first', 'count', 'regions', 'reserved1']
    assert len(fields) == len(names)

    def mutated(**changes):
        values = dict(zip(names, fields)); values.update(changes)
        raw = bytearray(manifest); head.pack_into(raw, 0, *[values[n] for n in names])
        return bytes(raw)

    accepted = admit('accepted', manifest)
    assert accepted == 'decode=1 valid=1 board=1', f'the image\'s own manifest is refused: {accepted}'
    long_run = admit('long-run', mutated(run=300_000_000))
    assert long_run.startswith('decode=0'), f'a 300 s run is admitted: {long_run}'
    big_reserve = admit('big-reserve', mutated(reserve=60_000_000))
    assert big_reserve.startswith('decode=0'), f'a return reserve of the whole session is admitted: {big_reserve}'
    print(f"PASS storage-write admission: manifest run_us {parsed['run_deadline_us']} accepted ({accepted}); "
          f"300 s run refused ({long_run}); whole-session reserve refused ({big_reserve})")


if __name__ == '__main__':
    main()
