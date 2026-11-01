#!/usr/bin/env python3
"""Execute application region initialization from linked GPU images.

The native helper's validation and aligned zero stores execute in Unicorn.
Cache publication is a modeled boundary: the fixture observes the exact
published span and can refuse it. No board or GPU code executes. Additional
QEMU-layout ELFs reuse the native image's identity object with data fixtures
for multiple registrations, the record cap and malformed section lengths.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from elftools.elf.elffile import ELFFile
from harness import Image, hook_addresses
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
SECTION = 'sunstone_region_initializers'
HELPER = 'os_application_initialize_regions'
DEADLINE = 1000000000
STACK = 0x80000000
SENTINEL = STACK
LAYOUTS = ['ss_application', 'ss_manifest', 'ss_region', 'ss_region_initializer']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Model:
    def __init__(self, image):
        self.image = image
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(0x40000000, 0x08000000)
        self.uc.mem_map(STACK, 0x100000)
        for address, data in image.parts:
            if data:
                self.uc.mem_write(address, data)
        self.begin = image.address('__start_' + SECTION)
        self.end = image.address('__stop_' + SECTION)
        self.application = image.address('ss_application')
        self.manifest = self.get(self.application + image.layouts['ss_application']['manifest'])
        self.writes, self.publications, self.guards = [], [], []

    def get(self, address, size=8):
        return int.from_bytes(self.uc.mem_read(address, size), 'little')

    def put(self, address, value, size=8):
        self.uc.mem_write(address, int(value).to_bytes(size, 'little'))

    def field(self, base, kind, name):
        return base + self.image.layouts[kind][name]

    def record(self, index=0):
        return self.begin + index * 16

    def region_count(self):
        return self.get(self.field(self.manifest, 'ss_manifest', 'region_count'), 1)

    def region(self, index):
        return self.field(self.manifest, 'ss_manifest', 'regions') + index * 56

    def region_value(self, index, field):
        return self.get(self.field(self.region(index), 'ss_region', field), 4 if field in ('kind', 'access') else 8)

    def set_region(self, index, field, value):
        self.put(self.field(self.region(index), 'ss_region', field), value, 4 if field in ('kind', 'access') else 8)

    def set_record(self, field, value, index=0):
        self.put(self.field(self.record(index), 'ss_region_initializer', field), value,
                 4 if field in ('region', 'reserved') else 8)

    def watch(self, address, prefix, clear=False):
        before = bytes([0xa5]) * 64 + bytes([0x3c]) * prefix + bytes([0x5a]) * 64
        self.uc.mem_write(address - 64, before)
        after = before[:64] + (bytes(prefix) if clear else before[64:64 + prefix]) + before[64 + prefix:]
        self.guards.append((address - 64, after))

    def run(self, ranges=(), expected=1, fail_publication=0):
        def store(cpu, access, address, size, value, unused):
            if not STACK <= address < STACK + 0x100000:
                self.writes.append((address, size, value))

        def publish(cpu, address, size, unused):
            values = tuple(cpu.reg_read(UC_ARM64_REG_X0 + i) for i in range(3))
            self.publications.append(values)
            assert bytes(cpu.mem_read(values[0], values[1])) == bytes(values[1]), 'publication preceded zero stores'
            cpu.reg_write(UC_ARM64_REG_X0, int(len(self.publications) != fail_publication))
            cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))

        self.uc.hook_add(UC_HOOK_MEM_WRITE, store)
        hook_addresses(self.uc, [self.image.address('a733_cpu_coherence_publish')], publish)
        self.uc.reg_write(UC_ARM64_REG_X0, DEADLINE)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff000)
        self.uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        self.uc.emu_start(self.image.address(HELPER), SENTINEL, count=2000000)
        assert self.uc.reg_read(UC_ARM64_REG_PC) == SENTINEL, 'helper exhausted the instruction budget'
        result = self.uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        assert result == expected, (result, expected)
        expected_writes = [(address + offset, 8, 0) for address, size in ranges for offset in range(0, size, 8)]
        assert self.writes == expected_writes, (self.writes[:3], len(self.writes), len(expected_writes))
        assert self.publications == [(address, size, DEADLINE) for address, size in ranges], self.publications
        for address, after in self.guards:
            assert bytes(self.uc.mem_read(address, len(after))) == after, 'prefix or adjacent guard differs'
        return {'returned': result, 'aligned_zero_stores': len(self.writes),
                'publications': [[hex(address), size, deadline] for address, size, deadline in self.publications]}


def fixture_image(args, count, extra_byte=False, bounds=None):
    out = args.output.parent / ('records-' + str(count) + ('-extra-byte' if extra_byte else '') + ('-' + bounds if bounds else ''))
    out.mkdir(parents=True, exist_ok=True)
    records = ',\n'.join('{%d, 0, %d}' % (i, 32 + i * 8) for i in range(count))
    regions = ',\n'.join('{.address=0x%x, .bytes=4096, .kind=SS_REGION_WORK, .access=SS_ACCESS_READ|SS_ACCESS_GPU_WRITE}'
                          % (0x44800000 + i * 0x200000) for i in range(min(count, 8)))
    source = '''#include "sunstone_app.h"
const struct ss_manifest fixture_manifest = {.region_count=%d, .regions={%s}};
/* The helper consumes only the real binding's first, manifest-pointer field. */
const struct { const struct ss_manifest *manifest; } ss_application = {&fixture_manifest};
const struct ss_region_initializer initializers[%d]
 __attribute__((section("sunstone_region_initializers"), aligned(8), used)) = {%s};
void _start(void); void _start(void) { }
int a733_cpu_coherence_publish(uint64_t address, uint64_t bytes, uint64_t deadline);
int a733_cpu_coherence_publish(uint64_t address, uint64_t bytes, uint64_t deadline)
{ (void)address; (void)bytes; (void)deadline; return 1; }
''' % (min(count, 8), regions, count, records)
    if extra_byte:
        source += 'const unsigned char tail __attribute__((section("sunstone_region_initializers"), aligned(1), used)) = 1;\n'
    (out / 'fixture.c').write_text(source)
    script = ROOT / 'platform/qemu-smp/link.ld'
    if bounds:
        text = script.read_text()
        if bounds == 'unaligned':
            text = text.replace('__start_sunstone_region_initializers = .;', '__start_sunstone_region_initializers = . + 1;')
            text = text.replace('__stop_sunstone_region_initializers = .;', '__stop_sunstone_region_initializers = . + 1;')
        else:
            raise AssertionError(bounds)
        script = out / 'link.ld'
        script.write_text(text)
    commands = [[str(args.cc), '--target=aarch64-none-elf', '-std=c11', '-g', '-O2', '-ffreestanding',
                 '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'include'), '-c', str(out / 'fixture.c'),
                 '-o', str(out / 'fixture.o')],
                [str(args.ld), '--gc-sections', '-u', HELPER, '-T', str(script),
                 str(out / 'fixture.o'), str(args.identity_object), '-o', str(out / 'fixture.elf')]]
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        (out / ('compile.log' if '-c' in command else 'link.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    fixture = Image(out / 'fixture.elf', LAYOUTS)
    # A standalone helper object can omit unused private binding types from
    # DWARF. Its layout comes from the same native image, not a fixture guess.
    for name, fields in args.native_layouts.items():
        if name in fixture.layouts:
            assert fixture.layouts[name] == fields, name
        else:
            fixture.layouts[name] = fields
    return fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True, help='native GPU image with one application initializer')
    parser.add_argument('--empty-image', type=Path, required=True, help='queue-add GPU image without registrations')
    parser.add_argument('--identity-object', type=Path, help='identity.c.obj from the native --image build')
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--ld', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', help='run only a named case; repeat to resume selected cases')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.identity_object is None:
        args.identity_object = args.image.parent / 'CMakeFiles/sunstone-a733-os-core.dir/apps/os/identity.c.obj'
    assert args.identity_object.is_file(), args.identity_object
    image = Image(args.image, LAYOUTS)
    empty_image = Image(args.empty_image, LAYOUTS)
    args.native_layouts = image.layouts
    model = Model(image)
    assert model.end - model.begin == 16, 'the native application must register exactly one initializer'
    index = model.get(model.record(), 4)
    address = model.region_value(index, 'address')
    prefix = model.get(model.record() + 8)
    assert model.get(model.record() + 4, 4) == 0 and prefix > 0 and prefix % 8 == 0
    cases = []

    def record(name, machine, ranges=(), expected=1, fail_publication=0):
        if args.case and name not in args.case:
            return
        cases.append({'case': name, **machine.run(ranges, expected, fail_publication)})

    model.watch(address, prefix, True)
    record('native-registration-exact-prefix', model, [(address, prefix)])
    for moved, length in [(0x45000000, 32), (0x45200000, 72), (0x46fff000, 4096)]:
        model = Model(image)
        model.set_region(index, 'address', moved)
        model.set_region(index, 'bytes', 4096)
        model.set_record('zero_prefix_bytes', length)
        model.watch(address, prefix)
        model.watch(moved, length, True)
        record('relocated-prefix-%d' % length, model, [(moved, length)])
    model = Model(image)
    model.watch(address, prefix, True)
    record('publication-failure-observed', model, [(address, prefix)], 0, 1)

    changes = [
        ('region-index-out-of-bounds', lambda m: m.set_record('region', m.region_count())),
        ('reserved-word', lambda m: m.set_record('reserved', 1)),
        ('zero-prefix', lambda m: m.set_record('zero_prefix_bytes', 0)),
        ('unaligned-prefix', lambda m: m.set_record('zero_prefix_bytes', 7)),
        ('prefix-exceeds-region', lambda m: m.set_record('zero_prefix_bytes', m.region_value(index, 'bytes') + 8)),
        ('cpu-write-permission', lambda m: m.set_region(index, 'access', 11)),
        ('missing-cpu-read', lambda m: m.set_region(index, 'access', 8)),
        ('missing-gpu-write', lambda m: m.set_region(index, 'access', 1)),
        ('input-region-kind', lambda m: m.set_region(index, 'kind', 1)),
        ('output-region-kind', lambda m: m.set_region(index, 'kind', 3)),
        ('unaligned-address', lambda m: m.set_region(index, 'address', address + 4)),
        ('address-before-window', lambda m: m.set_region(index, 'address', 0x43fffff8)),
        ('address-after-window', lambda m: m.set_region(index, 'address', 0x47000000)),
        ('region-crosses-window-end', lambda m: m.set_region(index, 'address', 0x46fffff8)),
        ('address-overflow', lambda m: m.set_region(index, 'address', 0xfffffffffffffff8)),
        ('region-size-overflow', lambda m: m.set_region(index, 'bytes', 0xffffffffffffffff)),
        ('zero-region-size', lambda m: m.set_region(index, 'bytes', 0)),
        ('manifest-region-count-overflow', lambda m: m.put(m.field(m.manifest, 'ss_manifest', 'region_count'), 9, 1)),
        ('null-manifest', lambda m: m.put(m.application + m.image.layouts['ss_application']['manifest'], 0)),
    ]
    for name, change in changes:
        model = Model(image)
        model.watch(address, prefix)
        change(model)
        record(name + '-before-writes', model, expected=0)

    model = Model(empty_image)
    assert model.begin == model.end, 'the empty application unexpectedly registers an initializer'
    watched = []
    for i in range(model.region_count()):
        if model.region_value(i, 'access') & 8:
            target = model.region_value(i, 'address')
            model.watch(target, 128)
            watched.append(target)
    assert watched, 'empty-registration image must still have a GPU-write region'
    record('no-registration-leaves-gpu-output-untouched', model)

    multi = fixture_image(args, 2)
    def multi_model(clear=False):
        m = Model(multi)
        ranges = [(m.region_value(i, 'address'), 32 + 8 * i) for i in range(2)]
        for target, length in ranges:
            m.watch(target, length, clear)
        return m, ranges
    model, ranges = multi_model(True)
    record('two-registrations-exact-prefixes', model, ranges)
    for name, change in [
        ('later-reserved', lambda m: m.set_record('reserved', 1, 1)),
        ('later-bad-index', lambda m: m.set_record('region', 2, 1)),
        ('later-cpu-write', lambda m: m.set_region(1, 'access', 11)),
        ('later-unaligned-prefix', lambda m: m.set_record('zero_prefix_bytes', 7, 1)),
        ('duplicate-region', lambda m: m.set_record('region', 0, 1)),
    ]:
        model, ranges = multi_model()
        change(model)
        record(name + '-refused-before-any-write', model, expected=0)
    model, ranges = multi_model(True)
    record('second-publication-failure-observed', model, ranges, 0, 2)
    for count in (8, 9):
        model = Model(fixture_image(args, count))
        ranges = [(model.region_value(i, 'address'), 32 + 8 * i) for i in range(min(count, 8))]
        for target, length in ranges:
            model.watch(target, length, count == 8)
        record('record-cap-%d' % count, model, ranges if count == 8 else (), int(count == 8))
    model = Model(fixture_image(args, 2, True))
    for i in range(2):
        model.watch(model.region_value(i, 'address'), 32 + 8 * i)
    assert (model.end - model.begin) % 16
    record('partial-registration-record-refused-before-writes', model, expected=0)

    for bounds in ('unaligned',):
        model = Model(fixture_image(args, 2, bounds=bounds))
        for i in range(2):
            model.watch(model.region_value(i, 'address'), 32 + 8 * i)
        assert model.begin & 7
        record(bounds + '-registration-bounds-refused-before-writes', model, expected=0)
    if args.case:
        assert set(args.case) == {case['case'] for case in cases}, 'unknown selected case'

    with args.image.open('rb') as stream:
        native = ELFFile(stream).get_section_by_name(SECTION)
        assert native is not None and native['sh_flags'] & 2 and not native['sh_flags'] & 1
        assert native['sh_addralign'] >= 8
    result = {'schema': 'sunstone-os-region-initialization-v1', 'physical_access': False,
              'images': {str(path): sha(path) for path in (args.image, args.empty_image)},
              'identity_object': {'path': str(args.identity_object), 'sha256': sha(args.identity_object)},
              'cases': cases, 'scope': __doc__}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS native application region initialization:', len(cases), 'cases')


if __name__ == '__main__':
    main()
