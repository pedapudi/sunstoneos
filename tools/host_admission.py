#!/usr/bin/env python3
"""Host pre-flight of the EL3 admission of an OS image, run with the image's own admission code.

A returning image stops at EL3 admission (a733_smp_stop(1, detail)) when its contract or session is refused. This
tool builds, for the host, the admission code of the image under test: apps/os/admission.c (or the cpu-return
patched copy in the build tree), apps/os/session.c, apps/board/admission.c, the storage digest, the image's build
ID and its manifest section, all with the compile definitions of the image's own configured build. It then runs
them on the bytes the runner will send: the session uploaded to A733_BOARD_SESSION_ADDRESS and the contract the
launcher completes (platform/a733-smp-supervised/launch.c: arming time, startup deadline, diagnostic deadline and
expiry from the image's lifetime). A refusal names the failing term as the board's stop detail does
(include/a733_admission_refusal.h, tools/a733-crumbs.py).

No device access. The build directory is the CMake build that produced the image (found from the image's path);
the harness is cached by the image's SHA-256, so a queued copy of the image reuses the harness built at submission.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))
CLANG = Path(os.environ.get('SUNSTONE_HOST_CLANG', '/usr/bin/clang-21'))
COUNTER_HZ = 24_000_000
STARTUP_SECONDS = 1
# The counter at the EL3 check, after the launcher armed the board: the launcher's own work and the jump to EL3.
EL3_DELAY_TICKS = COUNTER_HZ // 10
CONTRACT_OFFSETS = {'magic': 0, 'armed_at': 80, 'startup_deadline': 88, 'diagnostic_deadline': 96, 'expires_at': 104}
STUBS = '#include <stdio.h>\n#include <stdlib.h>\n#define S(n) void n(void) { fprintf(stderr, "unmodelled: " #n "\\n"); exit(4); }\n'


class Unavailable(RuntimeError):
    """The host admission could not be built or run for this image; nothing was decided."""


class AdmissionRefused(ValueError):
    """The image's own admission code refuses this session; the message names the failing term."""

    def __init__(self, message, detail, term, sub):
        super().__init__(message)
        self.detail, self.term, self.sub = detail, term, sub


def load(name, path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command, **kwargs):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    return result


def find_build(image):
    """The CMake build directory and source root of an image built there, or None."""
    for directory in Path(image).resolve().parents:
        cache = directory / 'CMakeCache.txt'
        if cache.is_file():
            home = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache.read_text(), re.M)
            return directory, Path(home.group(1)) if home else ROOT
    return None


def symbol_bytes(elf, name):
    table = elf.get_section_by_name('.symtab')
    found = [s for s in table.iter_symbols() if s.name == name and s['st_shndx'] != 'SHN_UNDEF'] if table else []
    if len(found) != 1:
        return None
    section = elf.get_section(found[0]['st_shndx'])
    offset = int(found[0]['st_value']) - section['sh_addr']
    return section.data()[offset:offset + int(found[0]['st_size'])]


def image_facts(image):
    """Manifest section, contract magic and lifetime of a returning OS image."""
    with Path(image).open('rb') as stream:
        elf = ELFFile(stream)
        section = elf.get_section_by_name('sunstone_manifest')
        profile = symbol_bytes(elf, 'os_contract_profile')
        marker = symbol_bytes(elf, 'os_cpu_return_watchdog')
        if section is None or not section['sh_size'] or profile is None or marker is None:
            raise Unavailable('not a returning OS image with an application manifest')
        import struct
        return {'manifest': section.data(), 'contract_magic': struct.unpack_from('<Q', profile)[0],
                'seconds': struct.unpack_from('<5Q', marker)[4]}


def harness_key(image, sources):
    h = hashlib.sha256(Path(image).read_bytes())
    for path in sources:
        h.update(Path(path).read_bytes())
    return h.hexdigest()[:24]


def build_harness(image, build=None, cache=None):
    """Compile the image's admission code into a host executable; returns its path. Cached by image digest."""
    image = Path(image)
    cache = Path(cache) if cache else CACHE / 'preflight'
    located = find_build(image) if build is None else build
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    if not located:
        # An image copied out of its build (a queued submission) finds the harness built when it was submitted.
        for candidate in sorted(cache.glob(digest[:24] + '-*/host_admission'), key=lambda p: p.stat().st_mtime, reverse=True) if cache.is_dir() else []:
            return candidate
        raise Unavailable('the image is not under its CMake build directory and no harness was built at submission')
    build_dir, source = located
    target = image.stem
    output = build_dir / 'os-image' / ('' if target == 'sunstone-a733-os' else target)
    flags = build_dir / 'CMakeFiles/sunstone-a733-os-core.dir/flags.make'
    build_id = output / (target + '-build-id.c')
    if not flags.is_file() or not build_id.is_file():
        raise Unavailable('the build directory has no os-image build ID or core flags for %s' % target)
    text = flags.read_text()
    defines = re.search(r'^C_DEFINES = (.*)$', text, re.M).group(1).split()
    includes = re.search(r'^C_INCLUDES = (.*)$', text, re.M).group(1).split()
    returning = '-DSUNSTONE_A733_OS_CPU_RETURN=1' in defines
    admission = build_dir / 'os-image/return-admission.c' if returning else source / 'apps/os/admission.c'
    units = [(admission, defines), (source / 'apps/os/session.c', defines),
             (source / 'drivers/storage/storage_digest.c', defines), (build_id, defines),
             (ROOT / 'tools/host_admission.c', defines)]
    if '-DSUNSTONE_A733_OS_CPU_CLOCK=1' in defines:
        units.append((source / 'drivers/clock/a733_cpu_clock.c', [d for d in defines if d != '-DSUNSTONE_CRUMBS=1']))
    for path, _ in units:
        if not path.is_file():
            raise Unavailable('missing admission source %s' % path)
    facts = image_facts(image)
    key = harness_key(image, [p for p, _ in units] + [ROOT / 'tools/host_admission_io.c'])
    out = cache / (digest[:24] + '-' + key[:8])
    binary = out / 'host_admission'
    if binary.is_file():
        return binary
    out.mkdir(parents=True, exist_ok=True)
    cc = [CLANG, '-std=gnu11', '-O1', '-g', '-w', '-no-pie']
    objects = []
    for path, d in units:
        obj = out / (path.stem + '.o')
        done = run([*cc, *d, *includes, '-I' + str(source / 'apps/os'), '-I' + str(source / 'tools'), '-c', path, '-o', obj])
        if done.returncode:
            raise Unavailable('cannot compile %s with the image definitions: %s' % (path.name, done.stderr.strip()[:400]))
        objects.append(obj)
    (out / 'manifest.bin').write_bytes(facts['manifest'])
    (out / 'manifest.c').write_text(
        '#include "application.h"\n__asm__(".section sunstone_manifest,\\"a\\"\\n.balign 8\\n.global __start_sunstone_manifest\\n'
        '__start_sunstone_manifest:\\n.incbin \\"%s\\"\\n.global __stop_sunstone_manifest\\n__stop_sunstone_manifest:\\n.text\\n");\n'
        'extern const struct ss_manifest __start_sunstone_manifest[];\n'
        'const struct ss_application ss_application = {__start_sunstone_manifest, 0, 0, 0, 0};\n' % (out / 'manifest.bin'))
    done = run([*cc, *defines, *includes, '-I' + str(source / 'apps/os'), '-c', out / 'manifest.c', '-o', out / 'manifest.o'])
    if done.returncode:
        raise Unavailable('cannot compile the manifest section: %s' % done.stderr.strip()[:400])
    objects.append(out / 'manifest.o')
    done = run([*cc, '-c', ROOT / 'tools/host_admission_io.c', '-o', out / 'io.o'])
    if done.returncode:
        raise Unavailable('cannot compile host_admission_io.c: ' + done.stderr.strip()[:400])
    objects.append(out / 'io.o')
    stubs = []
    for attempt in range(3):
        extra = []
        if stubs:
            (out / 'stubs.c').write_text(STUBS + ''.join('S(%s)\n' % n for n in stubs))
            done = run([*cc, '-c', out / 'stubs.c', '-o', out / 'stubs.o'])
            if done.returncode:
                raise Unavailable('cannot compile stubs: ' + done.stderr.strip()[:400])
            extra = [out / 'stubs.o']
        done = run([CLANG, '-no-pie', *objects, *extra, '-o', binary])
        if not done.returncode:
            return binary
        missing = sorted(set(re.findall(r'undefined (?:symbol|reference to)[: ]+[`\']?([A-Za-z_][A-Za-z_0-9]*)', done.stderr)))
        if not missing or set(missing) <= set(stubs):
            break
        stubs = sorted(set(stubs) | set(missing))
    raise Unavailable('cannot link the admission code: %s' % done.stderr.strip()[:400])


def completed_contract(contract, seconds, magic, armed_at=COUNTER_HZ * 3600):
    """The contract after the launcher: the runner's bytes with the launcher's time fields and magic."""
    import struct
    raw = bytearray(contract)
    for name in ('armed_at', 'startup_deadline', 'diagnostic_deadline', 'expires_at', 'magic'):
        if struct.unpack_from('<Q', raw, CONTRACT_OFFSETS[name])[0]:
            raise ValueError('the runner\'s contract has a nonzero %s before the launcher' % name)
    struct.pack_into('<Q', raw, CONTRACT_OFFSETS['armed_at'], armed_at)
    struct.pack_into('<Q', raw, CONTRACT_OFFSETS['startup_deadline'], armed_at + STARTUP_SECONDS * COUNTER_HZ)
    struct.pack_into('<Q', raw, CONTRACT_OFFSETS['diagnostic_deadline'], armed_at + seconds * COUNTER_HZ)
    struct.pack_into('<Q', raw, CONTRACT_OFFSETS['expires_at'], armed_at + seconds * COUNTER_HZ)
    struct.pack_into('<Q', raw, CONTRACT_OFFSETS['magic'], magic)
    return bytes(raw), armed_at + EL3_DELAY_TICKS


def evaluate(binary, session, completed, now):
    """Run the harness on a session and a completed contract; {'detail', 'decode', 'stop'}."""
    with tempfile.TemporaryDirectory(prefix='preflight-') as scratch:
        scratch = Path(scratch)
        (scratch / 'session.bin').write_bytes(bytes(session))
        (scratch / 'contract.bin').write_bytes(bytes(completed))
        done = run([binary, scratch / 'session.bin', scratch / 'contract.bin', str(now)], timeout=60)
    if done.returncode:
        raise Unavailable('the host admission code stopped (%d): %s' % (done.returncode, (done.stderr or done.stdout).strip()[:300]))
    found = re.fullmatch(r'refusal=0x([0-9a-f]+) decode=(\d+) stop=0x([0-9a-f]+)\s*', done.stdout)
    if not found:
        raise Unavailable('unexpected host admission output: ' + done.stdout.strip()[:200])
    return {'detail': int(found[1], 16), 'decode': int(found[2]), 'stop': int(found[3], 16)}


def explain(detail, image):
    """Message for a refusal: the named term and, for the lifetime terms, the manifest numbers."""
    crumbs = load('host_admission_crumbs', ROOT / 'tools/a733-crumbs.py')
    named = crumbs.refusal_detail(detail)
    text = 'EL3 admission would stop the board with a733_smp_stop(1, %#x): %s' % (detail, named['meaning'] if named else 'unnamed')
    if named and named.get('sub') in ('return_reserve', 'run_deadline'):
        lifetime = load('host_admission_lifetime', ROOT / 'tools/session_lifetime.py')
        facts = image_facts(image)
        sunstone_manifest = load('host_admission_manifest', ROOT / 'tools/sunstone-manifest.py')
        parsed = sunstone_manifest.unpack(facts['manifest'])[0]
        try:
            lifetime.check(parsed['run_deadline_us'], parsed['return_reserve_us'], facts['seconds'])
        except lifetime.LifetimeError as error:
            text += '; ' + str(error)
    return text, named


def preflight(image, session, contract, seconds=None, build=None, cache=None):
    """Run the image's admission on the session and the launcher-completed contract.

    Returns {'admitted': True, ...}; raises AdmissionRefused naming the term, or Unavailable when the host
    admission cannot be built (the caller decides whether that blocks)."""
    facts = image_facts(image)
    seconds = facts['seconds'] if seconds is None else seconds
    binary = build_harness(image, build, cache)
    completed, now = completed_contract(contract, seconds, facts['contract_magic'])
    result = evaluate(binary, session, completed, now)
    detail, decode, stop = result['detail'], result['decode'], result['stop']
    if detail != stop:
        raise Unavailable('the board stop %#x differs from the admission refusal %#x' % (stop, detail))
    report = {'admitted': detail == 0, 'detail': detail, 'decode_subterm': decode, 'lifetime_seconds': seconds,
              'harness': str(binary)}
    if detail:
        message, named = explain(detail, image)
        raise AdmissionRefused(message, detail, named['term'] if named else None, named.get('sub') if named else None)
    return report


def main(argv=None):
    import argparse
    parser = argparse.ArgumentParser(description='Run the image\'s EL3 admission on a session and contract (no device)')
    parser.add_argument('image', type=Path)
    parser.add_argument('session', type=Path)
    parser.add_argument('contract', type=Path, help='the runner\'s contract.bin, before the launcher completes it')
    parser.add_argument('--seconds', type=int)
    parser.add_argument('--build', type=Path, help='the CMake build directory of the image, when it is not under it')
    args = parser.parse_args(argv)
    try:
        located = None
        if args.build:
            home = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', (args.build / 'CMakeCache.txt').read_text(), re.M)
            located = (args.build.resolve(), Path(home.group(1)))
        print(json.dumps(preflight(args.image, args.session.read_bytes(), args.contract.read_bytes(), args.seconds, build=located), indent=2))
    except AdmissionRefused as error:
        print('refused: %s' % error, file=sys.stderr)
        return 1
    except Unavailable as error:
        print('unavailable: %s' % error, file=sys.stderr)
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main())
