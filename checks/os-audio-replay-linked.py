#!/usr/bin/env python3
"""Execute the recorded-input application from its full native OS ELF.

The linked manifest supplies format and regions. Grant publication, liveness,
counter, wait/progress and finish are modeled at their public boundaries. The
application and replay implementation execute their linked instructions. This
does not run boot, MMU setup, devices or FreeRTOS scheduling.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
from elftools.elf.elffile import ELFFile
from harness import Image, hook_addresses
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_LR

CHECK_INPUTS = ('apps/os/capture.c', 'checks/os-audio-replay-application.c')



def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    image = Image(a.elf, ['ss_grant', 'ss_span'])
    with a.elf.open('rb') as stream:
        elf = ELFFile(stream)
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        manifest = elf.get_section_by_name('sunstone_manifest').data()
    assert len(manifest) == 552 and struct.unpack_from('<II', manifest, 32) == (0, 0)
    assert struct.unpack_from('<IHHII', manifest, 88) == (16000, 1, 16, 1, 320)
    source, capacity, kind, access = struct.unpack_from('<QQII', manifest, 104)
    assert capacity == 684 and kind == 1 and access & 1
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    pages = set()

    def allocate(address, size):
        for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
            if page not in pages:
                uc.mem_map(page, 4096)
                pages.add(page)

    for address, data, size in parts:
        allocate(address, size)
        uc.mem_write(address, data)
    allocate(source, capacity)
    pcm = bytes((17 * i + 7) & 255 for i in range(640))
    uc.mem_write(source, b'\xa5' * 44 + pcm)
    # Scratch and stack are outside the complete admitted device-memory pool.
    grant = 0x7f000000
    allocate(grant, 0x10000)
    uc.reg_write(UC_ARM64_REG_SP, grant + 0xf000)
    layout = image.layouts['ss_grant']
    uc.mem_write(grant + layout['region_count'], struct.pack('<I', 1))
    uc.mem_write(grant + layout['regions'], struct.pack('<QQ', source, capacity))
    uc.reg_write(UC_ARM64_REG_X0, grant)
    counter = 0
    waits = 0
    result = None
    addresses = {image.address(n): n for n in
                 ('ss_grant', 'ss_live', 'a733_smp_counter', 'ss_usb_wait', 'ss_progress', 'ss_finish', 'ss_fail')}

    def boundary(machine, address, size, unused):
        nonlocal counter, waits, result
        name = addresses[address]
        if name == 'ss_finish':
            size = machine.reg_read(UC_ARM64_REG_X1)
            assert size == 8
            result = bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X0), size))
            machine.emu_stop()
            return
        assert name != 'ss_fail', hex(machine.reg_read(UC_ARM64_REG_X0))
        value = 0
        if name == 'ss_grant':
            value = grant
        elif name == 'ss_live':
            value = 1
        elif name == 'a733_smp_counter':
            value = counter
        elif name == 'ss_usb_wait':
            counter += 24000
            waits += 1
        machine.reg_write(UC_ARM64_REG_X0, value)
        machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))

    hook_addresses(uc, addresses, boundary)
    uc.emu_start(image.address('audio_replay_main'), 0, count=4000000)
    assert result is not None and struct.unpack('<II', result) == (320, zlib.adler32(pcm))
    assert waits == 20 and bytes(uc.mem_read(source, capacity)) == b'\xa5' * 44 + pcm
    (a.output / 'result.bin').write_bytes(result)
    record = {'passed': True, 'physical_access': False, 'frames': 320, 'paced_waits_ms': waits,
              'devices_required': 0, 'devices_optional': 0, 'mimus_dependency': False,
              'image_sha256': hashlib.sha256(a.elf.read_bytes()).hexdigest(),
              'modeled_boundaries': list(addresses.values()), 'boot_and_scheduler_executed': False}
    (a.output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS full native OS linked replay: 320 frames, 20 ms pacing, exact checksum, no devices or mimus')


if __name__ == '__main__':
    main()
