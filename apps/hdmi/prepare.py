#!/usr/bin/env python3
"""Package a supervised HDMI image and supplied physical-admission records offline."""
import argparse
import hashlib
import json
import io
from pathlib import Path
import struct
import subprocess
from elftools.elf.elffile import ELFFile
ROOT = Path(__file__).resolve().parents[2]
CONTRACT_MAGIC = 0x4153373348444331
SESSION_MAGIC = 0x4153373348444131
REQUEST_MAGIC = 0x4153373353554c33

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def image_data(image):
    raw = image if isinstance(image, bytes) else image.read_bytes()
    with io.BytesIO(raw) as f:
        elf = ELFFile(f)
        symbols = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        parts = [(int(p['p_vaddr']), int(p['p_memsz']), bytes(p.data()))
                 for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
    def read(address, size):
        for base, _, data in parts:
            if base <= address and address + size <= base + len(data):
                return data[address-base:address-base+size]
        raise ValueError('Required image symbol has no initialized bytes')
    build_address = symbols['a733_smp_build_id']
    build = read(build_address, 32)
    size, = struct.unpack('<Q', read(symbols['a733_hdmi_session_bytes'], 8))
    if not 0x41000000 <= build_address <= 0x411fffe0 or not 232 <= size <= 4096:
        raise ValueError('Image layout is outside the HDMI application ABI')
    return parts, build_address, build, size

def validate(image, session, admission):
    image_bytes = image.read_bytes()
    admission_bytes = admission.read_bytes()
    raw = session.read_bytes()
    parts, address, build, size = image_data(image_bytes)
    if len(raw) != size:
        raise ValueError('Session must contain exactly the linked native structure')
    magic, version, count, epoch, generation, lease = struct.unpack_from('<6Q', raw)
    supervisor, owner = struct.unpack_from('<2Q', raw, 208)
    if (magic, version, count) != (SESSION_MAGIC, 1, size) or not all((epoch, generation, lease, supervisor, owner)):
        raise ValueError('Invalid supplied session identity')
    if raw[48:80] != build or raw[80:112] != hashlib.sha256(admission_bytes).digest():
        raise ValueError('Session does not bind this image and admission record')
    record = json.loads(admission_bytes)
    if record.get('schema') != 'a733-hdmi-supervised-admission-v1' or record.get('image_sha256') != hashlib.sha256(image_bytes).hexdigest():
        raise ValueError('Admission record does not name this image')
    if any(record.get(k) != v for k, v in dict(epoch=epoch, generation=generation, lease=lease, supervisor=supervisor).items()):
        raise ValueError('Admission and session identities differ')
    snapshots = {'image.elf': image_bytes, 'admission.json': admission_bytes}
    for key, offset in (('analog', 112), ('power', 144), ('supervision', 176)):
        path = (admission.parent / record[key]).resolve()
        data = path.read_bytes()
        if hashlib.sha256(data).digest() != raw[offset:offset+32]:
            raise ValueError(f'{key} record differs from admitted session')
        snapshots[key+'-record'] = data
    return parts, address, build, raw, epoch, generation, lease, supervisor, snapshots

def request_bytes(address, build, raw, epoch, generation, lease, supervisor):
    session_hash = hashlib.sha256(raw).digest()
    request = struct.pack('<8Q', REQUEST_MAGIC, 3, 128, epoch, generation,
                          generation ^ (2**64-1), lease, address) + build + session_hash
    contract = struct.pack('<6Q', 0, 3, 224, epoch, generation, generation ^ (2**64-1)) + build
    contract += struct.pack('<14Q', 0, 0, 0, 0, lease, 0, 0, 0, 0, 0, 0, 0, supervisor, 0) + session_hash
    assert len(request) == 128 and len(contract) == 224
    return request, contract + bytes(4096-len(contract))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--session', type=Path, required=True)
    p.add_argument('--admission', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--lld', type=Path, required=True)
    p.add_argument('--objcopy', type=Path, required=True)
    a = p.parse_args()
    parts, address, build, raw, epoch, generation, lease, supervisor, snapshots = validate(a.image, a.session, a.admission)
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    paths = {'launch.c': ROOT/'platform/a733-smp-supervised/launch.c',
             'launch.S': ROOT/'platform/a733-smp-observe/launch.S',
             'launch.ld': ROOT/'platform/a733-smp-observe/launch.ld',
             'boot.h': ROOT/'platform/a733-smp-cached/boot.h'}
    for name, source in paths.items(): (out/name).write_bytes(source.read_bytes())
    commands = []
    def run(command):
        command = list(map(str, command)); commands.append(command)
        r = subprocess.run(command, capture_output=True, text=True)
        with (out/'build.log').open('a') as f: f.write(r.stdout+r.stderr)
        (out/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
        r.check_returncode()
    flags = ['--target=arm-none-eabi', '-march=armv8-a', '-marm', '-mfloat-abi=soft',
             '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
             '-fno-asynchronous-unwind-tables', '-Os', '-g', '-Wall', '-Wextra', '-Werror',
             f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL', '-I'+str(out)]
    for name in ('launch.c', 'launch.S'):
        run([a.clang.resolve(), *flags, '-MD', '-MF', out/(name+'.d'), '-c', out/name, '-o', out/(name+'.o')])
    run([a.lld.absolute(), '-T', out/'launch.ld', out/'launch.c.o', out/'launch.S.o', '-o', out/'launch.elf'])
    run([a.objcopy.resolve(), '-O', 'binary', out/'launch.elf', out/'launch.bin'])
    if not 0 < (out/'launch.bin').stat().st_size <= 0xc00:
        raise ValueError('Launcher exceeds its SRAM reservation')
    payloads = []
    reservations = []
    for i, (base, size, data) in enumerate(parts):
        reservations.append(dict(address=base, bytes=size))
        if data:
            name = f'segment-{i}.bin'; (out/name).write_bytes(data)
            payloads.append(dict(name=name, address=base))
    request, contract = request_bytes(address, build, raw, epoch, generation, lease, supervisor)
    fixed = [('session.bin', raw+bytes(4096-len(raw)), 0x41f00000),
             ('request.bin', request, 0x52c00), ('contract.bin', contract, 0x42004000),
             ('result-zero.bin', bytes(16384), 0x42000000), ('report-zero.bin', bytes(128), 0x52d00),
             ('launch.bin', (out/'launch.bin').read_bytes(), 0x52000)]
    for name, data, base in fixed:
        (out/name).write_bytes(data); payloads.append(dict(name=name, address=base))
    for name, data in snapshots.items(): (out/name).write_bytes(data)
    record = dict(schema='a733-hdmi-supervised-bundle-v1', physical_access=False,
                  build_id=build.hex(), image_sha256=hashlib.sha256(snapshots['image.elf']).hexdigest(), session_sha256=hashlib.sha256(raw).hexdigest(),
                  epoch=epoch, generation=generation, lease=lease, supervisor=supervisor,
                  payloads=payloads, reservations=reservations,
                  files={f.name:digest(f) for f in out.iterdir() if f.is_file()},
                  tools={str(t.resolve()):digest(t.resolve()) for t in (a.clang,a.lld,a.objcopy)})
    (out/'prepared.json').write_text(json.dumps(record, indent=2)+'\n')
    print('Prepared supervised HDMI bundle; physical launch requires coordinator approval and board ownership.')
if __name__ == '__main__': main()
