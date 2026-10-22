#!/usr/bin/env python3
"""Count the render worker's per-frame CPU work on the texture path.

Builds checks/gpu-texture-frame.c with the worker and texture sources of
--source (default: this checkout) and runs it under Unicorn. Counts, per
frame and while the worker runs: bytes stored by memory region, cache
maintenance by operation, stage-1 translations, loads from the output
allocation during completion checks, instructions, barriers and store-release
instructions, worker polls, and the worker's tick waits between CPU-only steps
under its burst rule. Client rings, clock, ownership and GPU results are stubs;
nothing touches a device. Runs 1080p profile 4 with 16 batches and
read-back, 720p profile 2 with read-back, and 720p profile 2 presented.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_PC

ROOT = Path(__file__).resolve().parents[1]
FREERTOS = Path.home() / '.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
NEWLIB = Path.home() / '.cache/sunstoneos/host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include'
SOURCES = ['drivers/gpu/pvr_render_service.c', 'drivers/gpu/pvr_render_texture.c', 'drivers/gpu/pvr_render_job.c',
           'drivers/gpu/pvr_render_programs.c', 'drivers/gpu/pvr_render_draw.c', 'runtime/memory.c']
# The C flags of the A733 SMP images that run the render worker
# (cmake/a733-smp-cached.cmake and its siblings), which include
# -mgeneral-regs-only, so no copy loop uses SIMD registers.
FLAGS = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-ffunction-sections', '-fdata-sections', '-O2', '-Wall', '-Wextra']
STACK, INPUT, SHARED, PAYLOAD = 0x4f000000, 0x50000000, 0x5f000000, 0x60000000
SENTINEL = 0x4ffff000
SCENES = {
    'profile4-1080p-readback': dict(width=1920, height=1080, profile=4, present=0, vertices=4, indices=65535, batches=16),
    'profile2-720p-readback': dict(width=1280, height=720, profile=2, present=0, vertices=4, indices=6, batches=0),
    'profile2-720p-present': dict(width=1280, height=720, profile=2, present=1, vertices=4, indices=6, batches=0),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(source, out, clang, lld):
    objects = []
    for name in SOURCES + ['checks/gpu-texture-frame.c']:
        root = ROOT if name.startswith('checks/') else source
        obj = out / (Path(name).stem + '.o')
        subprocess.run([clang, *FLAGS, '-I', str(source / 'include'), '-I', str(FREERTOS / 'include'),
                        '-I', str(FREERTOS / 'portable/GCC/ARM_CR82'), '-isystem', str(NEWLIB),
                        '-c', str(root / name), '-o', str(obj)], check=True)
        objects.append(str(obj))
    elf = out / 'frame.elf'
    subprocess.run([lld, '-static', '--gc-sections', '-e', 'frame_main', '-Ttext=0x40000000', '-o', str(elf), *objects],
                   check=True)
    return elf


def scene_input(scene):
    """Vertices, indices, batch records and texels in the canonical order."""
    corners = [(-1, -1, 0, 1, 0, 0), (1, -1, 0, 1, 1, 0), (-1, 1, 0, 1, 0, 1), (1, 1, 0, 1, 1, 1)]
    data = bytearray(b''.join(struct.pack('<6f', *v) for v in corners[:scene['vertices']]))
    quad = [0, 1, 3, 0, 3, 2]
    data += struct.pack('<%dH' % scene['indices'], *[quad[i % 6] for i in range(scene['indices'])])
    state = 0
    if scene['batches']:
        n = scene['batches']; count = scene['indices'] // n // 3 * 3
        for i in range(n):
            first = i * count; c = count if i < n - 1 else scene['indices'] - first
            data += struct.pack('<4I', first, c, (i % 5) << 6 | 8 | 1, 0)
        state = n | 1 << 5 | 2 << 6 | 2 << 9
    stride = (scene['width'] + 1) & ~1
    row = bytes((x * 7 + 13) & 255 for x in range(stride * 4))
    data += row * scene['height']
    return bytes(data), state


class Frame:
    def __init__(self, elf):
        self.elf = elf
        with elf.open('rb') as f:
            e = ELFFile(f)
            self.symbols = {s.name: s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols() if s.name}
            self.segments = [(p['p_vaddr'], p['p_memsz'], p.data()) for p in e.iter_segments() if p['p_type'] == 'PT_LOAD']
            text = e.get_section_by_name('.text')
            self.text = (text['sh_addr'], text.data())
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        low = min(a for a, _, _ in self.segments) & ~0xfff
        high = (max(a + n for a, n, _ in self.segments) + 0xfff) & ~0xfff
        self.u.mem_map(low, high - low)
        for address, _, data in self.segments:
            self.u.mem_write(address, data)
        self.u.mem_map(STACK, 0x1000000)

    def run(self, scene):
        u = self.u
        data, state = scene_input(scene)
        u.mem_map(INPUT, (len(data) + 0xfff) & ~0xfff)
        u.mem_write(INPUT, data)
        u.mem_map(SHARED, 0x10000)
        u.mem_map(PAYLOAD, 0x8000000)
        config = self.symbols['frame_config']
        u.mem_write(config, struct.pack('<3Q9I', PAYLOAD, SHARED, INPUT, scene['width'], scene['height'],
                    scene['profile'], state, 2, scene['present'], scene['vertices'], scene['indices'], len(data)))
        self.layout = None; self.counting = 0; self.frame = 0; self.stage = 'setup'
        self.frames = {}
        dsb, dmb, stlr = set(), set(), set()
        base, code = self.text
        for i in range(0, len(code), 4):
            w = int.from_bytes(code[i:i + 4], 'little')
            if (w & 0xfffff0ff) == 0xd503309f: dsb.add(base + i)
            elif (w & 0xfffff0ff) == 0xd50330bf: dmb.add(base + i)
            elif (w & 0x3ffffc00) == 0x089ffc00: stlr.add(base + i)
        marks = {self.symbols[n]: n for n in ('frame_mark', 'gpu_model_render', 'aarch64_device_cache',
                                              'aarch64_device_translate')}
        def current():
            return self.frames[self.frame]
        def region(address):
            if SHARED <= address < SHARED + 0x10000: return 'shared'
            if not PAYLOAD <= address < PAYLOAD + 0x8000000: return 'private'
            o = address - PAYLOAD; l = self.layout
            if o < l['output']: return 'programs'
            if o < l['output'] + l['output_allocation']: return 'output'
            if o < l['geometry']: return 'image'
            if o < l['geometry'] + 1703936: return 'geometry'
            if l['depth_bytes'] and l['depth'] <= o < l['depth'] + l['depth_bytes'] + 8192: return 'depth'
            if l['upload'] <= o < l['upload'] + 65536: return 'upload'
            if l['download'] <= o < l['download'] + 65536: return 'download'
            if l['copy'] <= o < l['copy'] + l['output_allocation'] and l['copy'] >= l['output'] + l['output_allocation']: return 'copy'
            return 'other'
        def add(counter, key, n):
            counter[key] = counter.get(key, 0) + n
        def code(uc, address, size, unused):
            if address == SENTINEL: uc.emu_stop(); return
            name = marks.get(address)
            if address in dsb and self.counting: current()['dsb'] += 1
            if address in dmb and self.counting: current()['dmb'] += 1
            if address in stlr and self.counting: current()['stlr'] += 1
            if name is None: return
            x0, x1, x2 = (uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
            if name == 'frame_mark':
                if x0 in (0, 1, 2): self.counting = x0
                elif 16 < x0 < 32:
                    self.frame = x0 - 16; self.stage = 'render'
                    self.frames[self.frame] = dict(stores={}, cache={}, cache_calls=0, translations=0, output_loads=0,
                        instructions=0, read_instructions=0, dsb=0, dmb=0, stlr=0, polls=0, requests=0, read_requests=0,
                        tick_waits=0, gpu_waits=0)
                    if self.layout is None:
                        fields = 'output output_allocation image geometry depth depth_bytes copy upload download total program_bytes payload_bytes'.split()
                        self.layout = dict(zip(fields, struct.unpack('<12I', uc.mem_read(self.symbols['frame_layout'], 48))))
                elif 32 < x0 < 48: self.stage = 'output'
                if x0 == 1: add(current(), 'read_requests' if self.stage == 'output' else 'requests', 1)
                if x0 == 2: current()['polls'] += 1
                if x0 == 3: current()['tick_waits'] += 1
                if x0 == 4: current()['gpu_waits'] += 1
            elif name == 'gpu_model_render':
                l = self.layout; frame = bytes(((x * 3 + x0) & 255) for x in range(4096))
                total = ((scene['width'] + 1) & ~1) * scene['height'] * 4
                start = PAYLOAD + l['output'] + 64
                for at in range(0, total, 4096): uc.mem_write(start + at, frame[:min(4096, total - at)])
                uc.mem_write(SHARED + 1408, struct.pack('<2I', x0, 2 * x0))
            elif name == 'aarch64_device_cache' and self.counting:
                c = current(); c['cache_calls'] += 1
                add(c['cache'], ('clean', 'clean_invalidate', 'invalidate')[x0] + ':' + region(x1), x2)
            elif name == 'aarch64_device_translate' and self.counting:
                current()['translations'] += 1
        def block(uc, address, size, unused):
            if self.counting:
                add(current(), 'read_instructions' if self.stage == 'output' else 'instructions', size // 4)
        def write(uc, access, address, size, value, unused):
            if self.counting and self.layout:
                add(current()['stores'], region(address), size)
        def read(uc, access, address, size, value, unused):
            if self.counting == 2 and self.layout and region(address) == 'output':
                current()['output_loads'] += size
        u.hook_add(UC_HOOK_CODE, code, begin=SENTINEL, end=SENTINEL)
        for at in list(marks) + sorted(dsb | dmb | stlr):
            u.hook_add(UC_HOOK_CODE, code, begin=at, end=at)
        u.hook_add(UC_HOOK_BLOCK, block)
        u.hook_add(UC_HOOK_MEM_WRITE, write, begin=SHARED, end=PAYLOAD + 0x8000000)
        self.read_hook = u.hook_add(UC_HOOK_MEM_READ, read, begin=PAYLOAD, end=PAYLOAD + 0x8000000)
        u.reg_write(UC_ARM64_REG_SP, STACK + 0xff0000)
        u.reg_write(UC_ARM64_REG_LR, SENTINEL)
        u.emu_start(self.symbols['frame_main'], SENTINEL)
        failure = int.from_bytes(u.mem_read(self.symbols['frame_failure'], 4), 'little')
        assert not failure and u.reg_read(UC_ARM64_REG_PC) == SENTINEL, ('driver failed', failure, hex(u.reg_read(UC_ARM64_REG_PC)))
        # The GPU-read areas hold exactly the submitted input, and the output
        # guards survive around the modeled frame.
        l = self.layout; v = scene['vertices'] * 24; i = scene['indices'] * 2; b = scene['batches'] * 16
        stride = (scene['width'] + 1) & ~1; frame = stride * scene['height'] * 4
        assert bytes(u.mem_read(PAYLOAD + l['geometry'], v)) == data[:v]
        assert bytes(u.mem_read(PAYLOAD + l['geometry'] + 65536 * 24, i)) == data[v:v + i]
        assert bytes(u.mem_read(PAYLOAD + l['image'], frame)) == data[v + i + b:]
        guard = struct.pack('<I', 0x36da97c5)
        assert bytes(u.mem_read(PAYLOAD + l['output'], 64)) == guard * 16
        tail = l['output_allocation'] - 64 - frame
        assert bytes(u.mem_read(PAYLOAD + l['output'] + 64 + frame, tail)) == guard * (tail // 4)
        # Depth stays in tile memory: the worker neither stores to nor
        # maintains the depth extent.
        for f in self.frames.values():
            assert 'depth' not in f['stores'] and not any(k.endswith(':depth') for k in f['cache']), f
        return dict(layout=self.layout, input_bytes=len(data), frames=self.frames)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--source', type=Path, default=ROOT, help='Checkout whose worker sources are measured')
    p.add_argument('--clang', default='clang')
    p.add_argument('--lld', default='ld.lld')
    p.add_argument('--scene', choices=sorted(SCENES), action='append')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    source = a.source.resolve()
    elf = build(source, a.output, a.clang, a.lld)
    results = {}
    for name in a.scene or sorted(SCENES):
        results[name] = Frame(elf).run(SCENES[name])
        f = results[name]['frames'][2]
        print(name, json.dumps({k: v for k, v in f.items()}, sort_keys=True), flush=True)
    record = dict(passed=True, physical_access=False, source=str(source), elf_sha256=sha(elf),
                  sources_sha256={n: sha(source / n) for n in SOURCES}, scenes=results, scope=__doc__)
    (a.output / 'frame.json').write_text(json.dumps(record, indent=1, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
