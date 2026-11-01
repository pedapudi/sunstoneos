#!/usr/bin/env python3
"""Execute the diagnostic stream constructor and check its complete owned extent.

Uses an accepted native compile database for dependency paths. This builds only
a host instruction fixture, without a board image or physical admission.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'checks'))
from harness import Image

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-database', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--linker', type=Path, default=Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21')
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    entries = json.loads(args.compile_database.read_text())
    entry = next(x for x in entries if x['file'].endswith('/drivers/npu/npu_convolution.c'))
    old_root = entry['file'].removesuffix('/drivers/npu/npu_convolution.c')
    command = [x.replace(old_root, str(ROOT)) for x in shlex.split(entry['command'])]
    source = ROOT/'checks/npu-active-cancel-layout.c'
    obj = args.evidence/'fixture.o'
    command[command.index('-c')+1] = str(source)
    command[command.index('-o')+1] = str(obj)
    command += ['-DSUNSTONE_NPU_ACTIVE_CANCEL=1']
    result = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True)
    (args.evidence/'compile.log').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stderr
    objects = [obj]
    commands = [command]
    for relative in ('platform/a733/npu_native.c','drivers/npu/a733_npu_memory.c',
                     'drivers/npu/a733_npu_mmu.c','runtime/counter_clock.c','runtime/memory.c'):
        selected = command.copy()
        target = args.evidence/(Path(relative).stem+'.o')
        selected[selected.index('-c')+1] = str(ROOT/relative)
        selected[selected.index('-o')+1] = str(target)
        result = subprocess.run(selected, cwd=entry['directory'], capture_output=True, text=True)
        (args.evidence/(target.stem+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode == 0, result.stderr
        objects.append(target)
        commands.append(selected)
    image_path = args.evidence/'fixture.elf'
    link = [str(args.linker), '-m', 'aarch64elf', '--gc-sections',
            '--entry=npu_active_cancel_stream', '-Ttext=0x40000000',
            '--undefined=npu_active_cancel_native', *map(str,objects), '-o', str(image_path)]
    result = subprocess.run(link, capture_output=True, text=True)
    (args.evidence/'link.log').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stderr
    image = Image(image_path, layout_names=('a733_npu_buffer','a733_npu_native','a733_npu_memory','a733_npu_ops'))
    fields = image.layouts['a733_npu_buffer']
    def machine():
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x40000000, 0x04000000)
        u.mem_map(0x200000, 4096)  # Linker's ELF header segment.
        u.mem_map(0x03600000, 4096)
        for address, data in image.parts:
            if data: u.mem_write(address, data)
        return u
    u = machine()
    read = lambda address, size: bytes(u.mem_read(address, size))
    geometry = [int.from_bytes(read(image.address('npu_active_cancel_geometry')+8*i, 8), 'little') for i in range(5)]
    assert geometry == [7, 0x189000, 0x184000, 262160, 24], geometry
    slots = int.from_bytes(read(image.address('npu_active_cancel_slots'), 8), 'little')
    extents = []
    for index in range(7):
        raw = read(slots+index*24, 24)
        offset, size, device, role = (int.from_bytes(raw[a:b], 'little') for a,b in ((0,8),(8,16),(16,20),(20,24)))
        assert offset%4096 == size%4096 == device%4096 == 0
        assert offset+size <= geometry[2]
        assert device >= 0x480000 or device+size <= 0x400000
        assert all(offset+size <= a or b <= offset for a,b in extents)
        extents.append((offset, offset+size))
        if index == 6: assert (offset,size,device,role) == (0x104000,524288,0xe010a000,2)
    assert geometry[2]+20480 == geometry[1]
    # The native selected pool is 2 MiB. Parent alignment cannot consume more
    # than a page here; other allocation capacity is reported for integration.
    assert geometry[1] <= 2*1024*1024
    cases = []
    for name, cpu, size, role, accepted in (
        ('owned-stream',0x42000000,524288,2,1),
        ('short-allocation',0x42000000,524280,2,0),
        ('misaligned-cpu',0x42000004,524288,2,0),
        ('wrong-role',0x42000000,524288,0,0),
    ):
        u = machine()
        config, sentinel = 0x43000000, 0x43fff000
        u.mem_write(0x42000000, bytes([0xa5])*524288)
        for field, value in dict(cpu=cpu, physical=0x42000000, bytes=size, device=0xe010a000, role=role).items():
            u.mem_write(config+fields[field], value.to_bytes(4 if field in ('device','role') else 8, 'little'))
        writes = []
        def written(uc, access, address, width, value, unused):
            if 0x42000000 <= address < 0x42080000: writes.append((address,width))
        u.hook_add(UC_HOOK_MEM_WRITE, written)
        u.reg_write(UC_ARM64_REG_X0, config)
        u.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
        u.reg_write(UC_ARM64_REG_LR, sentinel)
        u.emu_start(image.address('npu_active_cancel_stream'), sentinel, count=5000000)
        assert u.reg_read(UC_ARM64_REG_PC) == sentinel
        assert u.reg_read(UC_ARM64_REG_X0) == accepted
        raw = bytes(u.mem_read(0x42000000,524288))
        if accepted:
            wait = bytes.fromhex('4006003800000000')
            ending = bytes.fromhex('010e0108400000000000001000000000')
            assert raw[:262144] == wait*32768
            assert raw[262144:262160] == ending
            assert raw[262160:] == bytes.fromhex('0000001000000000')*((524288-262160)//8)
            assert all(0x42000000 <= a and a+n <= 0x42080000 for a,n in writes)
            assert 524280 <= len(raw) and len(raw)//4096 == 128
        else:
            assert not writes and raw == bytes([0xa5])*524288
        cases.append(dict(name=name, passed=True, writes=len(writes)))
    for name, bad_page in (('native-mappings-and-frontend-MMIO',None),
                           ('last-diagnostic-page-translation-refused',127)):
        u = machine()
        native, parent, physical, output, stop = 0x43000000,0x42000000,0x52000000,0x43fd0000,0x43fff000
        translations, reads, barriers = [], [], []
        def get(address, width=8): return int.from_bytes(u.mem_read(address,width),'little')
        def put(address,value,width=8): u.mem_write(address,int(value).to_bytes(width,'little'))
        def ret(value):
            u.reg_write(UC_ARM64_REG_X0,value)
            u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
        def execute(cpu,address,size,unused):
            if address == image.address('aarch64_device_cache_stride'):
                ret(64);return
            if address == image.address('aarch64_device_translate'):
                cpu_address, write, destination = [u.reg_read(UC_ARM64_REG_X0+i) for i in range(3)]
                translations.append((cpu_address,write))
                pa = cpu_address if cpu_address == 0x03600000 else cpu_address+physical-parent
                if bad_page is not None and cpu_address == parent+0x104000+4096*bad_page: pa += 4096
                put(destination,pa);put(destination+8,0 if cpu_address==0x03600000 else 255,4);put(destination+12,3,4)
                ret(1);return
            word = get(address,4)
            if word & 0xffffffe0 in (0xd53be000,0xd53be020):
                u.reg_write(UC_ARM64_REG_X0+(word&31),24000000 if word&0xffffffe0==0xd53be000 else 24)
                u.reg_write(UC_ARM64_REG_PC,address+4)
            if word & 0xfffff0ff == 0xd50330bf: barriers.append(word)
        def mmio(cpu,access,address,size,value,unused):
            assert size == 4
            reads.append(address)
        u.hook_add(UC_HOOK_CODE,execute)
        u.hook_add(UC_HOOK_MEM_READ,mmio,begin=0x03600000,end=0x03600fff)
        def call(address,*values):
            u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,stop)
            for i,value in enumerate(values): u.reg_write(UC_ARM64_REG_X0+i,value)
            u.emu_start(address,stop,count=5000000)
            assert u.reg_read(UC_ARM64_REG_PC)==stop
            return u.reg_read(UC_ARM64_REG_X0)
        accepted = call(image.address('npu_active_cancel_native'),native,parent,physical,0x03600000)
        assert accepted == int(bad_page is None)
        assert not reads  # Native construction accesses no NPU registers.
        assert set(a for a,w in translations if parent+0x104000 <= a < parent+0x184000) == set(range(parent+0x104000,parent+0x184000,4096))
        if accepted:
            arena,pa_arena = parent+0x184000,physical+0x184000
            root = get(arena+4*0xe0,4)
            assert root&0xf == 1
            table = arena+(root&~4095)-pa_arena
            for page in range(128):
                assert get(table+4*(0x10a+page),4) == physical+0x104000+4096*page|3
            operations = get(image.address('npu_active_cancel_native_operations'))
            read32 = get(operations+image.layouts['a733_npu_ops']['read'])
            for offset,value in ((0x660,0x12345678),(0x664,0xe010a008)):
                put(0x03600000+offset,value,4)
                for _ in range(2):
                    assert call(read32,native,offset,output)==1
                    assert get(output,4)==value
            assert reads == [0x03600660]*2+[0x03600664]*2
            assert len(barriers)>=4
        cases.append(dict(name=name,passed=True,translation_pages=len(translations),frontend_reads=reads))
    report = dict(passed=True, physical_access=False, board_image=False, geometry=geometry,
                  parent_bytes=geometry[1], selected_pool_bytes=2097152,
                  remaining_pool_bytes=2097152-geometry[1], cases=cases,
                  commands=commands+[link], elf_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),
                  scope='Actual diagnostic constructor, native adapter, memory admission, MMU builder and frontend MMIO. CPU translation and cache geometry modeled. Pool co-allocation and service recovery need their native fixtures.')
    (args.evidence/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS active cancellation layout, finite stream and rejected constructors')

if __name__ == '__main__': main()
