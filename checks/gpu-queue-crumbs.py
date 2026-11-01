#!/usr/bin/env python3
"""Check native GPU job breadcrumb arguments, ordering and disabled builds.

The real queue worker runs in Unicorn. Service time, cache operations, ring
transport and completion queues are modeled; no GPU or board is accessed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import struct
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, environment, hook_addresses, save, skip_crumbs

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compile-commands', type=Path, required=True)
p.add_argument('--lld', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
entry = next(c for c in json.loads(a.compile_commands.read_text())
             if c['file'].endswith('/drivers/gpu/pvr_compute_queue.c'))
original = shlex.split(entry['command'])
base, i = [], 0
while i < len(original):
    token = original[i]
    if token in ('-o', '-c'):
        i += 2
        continue
    if not token.startswith('-DSUNSTONE_CRUMBS'):
        base.append(token)
    i += 1
commands, rows = [], []

def run(command, log):
    commands.append(list(map(str, command)))
    save(a.output / 'commands.json', commands)
    result = subprocess.run(command, cwd=ROOT, env=environment(), capture_output=True)
    log.write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, (command, log)

types = {'pvr_compute_queue', 'pvr_gpu_service', 'pvr_cccb', 'gpu_queue_job', 'gpu_queue_binding',
         'gpu_queue_lease_info', 'pvr_mips_buffer', 'pvr_fw_buffer', 'a733_gpu_queue', 'device_memory_extent'}
for enabled in (False, True):
    directory = a.output / ('enabled' if enabled else 'disabled')
    directory.mkdir()
    obj, asm, stub, elf = (directory / n for n in ('queue.o', 'stubs.S', 'stubs.o', 'queue.elf'))
    run(base + (['-DSUNSTONE_CRUMBS=1'] if enabled else []) +
        ['-Werror', '-c', str(ROOT / 'drivers/gpu/pvr_compute_queue.c'), '-o', str(obj)], directory / 'compile.log')
    undefined = subprocess.check_output(['llvm-nm-21', '-u', str(obj)], text=True)
    names = [line.split()[-1] for line in undefined.splitlines()]
    asm.write_text('.text\n' + ''.join('.global %s\n%s:\n ret\n' % (n, n) for n in names))
    run([base[0], '--target=aarch64-none-elf', '-c', str(asm), '-o', str(stub)], directory / 'stubs.log')
    run([str(a.lld), '-Ttext=0x41000000', '--entry=pvr_compute_queue_poll', str(obj), str(stub),
         '-o', str(elf)], directory / 'link.log')
    image = Image(elf, types)
    sizes, enums = {}, {}
    with elf.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if not name:
                    continue
                name = name.value.decode()
                if die.tag == 'DW_TAG_enumerator':
                    enums[name] = die.attributes['DW_AT_const_value'].value
                if die.tag == 'DW_TAG_structure_type' and name in types and 'DW_AT_byte_size' in die.attributes:
                    sizes[name] = die.attributes['DW_AT_byte_size'].value
    for case in ('success', 'deadline-refused', 'fault-before-kick', 'fault-after-kick',
                 'abort-submitted', 'abort-reserved', 'publication-failed', 'create-ready'):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x80000000, 0x200000)
        mapped = set()
        for address, data in image.parts:
            if data:
                for page in range(address & ~4095, (address + len(data) + 4095) & ~4095, 4096):
                    if page not in mapped:
                        u.mem_map(page, 4096)
                        mapped.add(page)
                u.mem_write(address, data)
        crumbs = []
        skip_crumbs(u, image, crumbs)
        def put(address, value, width=4):
            u.mem_write(address, int(value).to_bytes(width, 'little'))
        def get(address, width=4):
            return int.from_bytes(u.mem_read(address, width), 'little')
        def field(address, typ, name):
            return address + image.layouts[typ][name]
        def setfield(address, typ, name, value, width=4):
            put(field(address, typ, name), value, width)
        q, service, owner, payload, timeline = 0x80010000, 0x80030000, 0x80050000, 0x80080000, 0x80090000
        sequence = 0x12345678
        setfield(q, 'pvr_compute_queue', 'owner', owner, 8)
        setfield(q, 'pvr_compute_queue', 'service', service, 8)
        setfield(q, 'pvr_compute_queue', 'epoch', 1, 8)
        setfield(service, 'pvr_gpu_service', 'epoch', 1, 8)
        setfield(q, 'pvr_compute_queue', 'phase', enums['PVR_COMPUTE_QUEUE_READY'])
        setfield(q, 'pvr_compute_queue', 'slot_state', enums['PVR_COMPUTE_QUEUE_SUBMITTED'])
        setfield(q, 'pvr_compute_queue', 'sequence', sequence - 1)
        setfield(q, 'pvr_compute_queue', 'lease_count', 1)
        setfield(q, 'pvr_compute_queue', 'job_us', 10000, 8)
        lease = field(q, 'pvr_compute_queue', 'lease')
        setfield(lease, 'gpu_queue_lease_info', 'bytes', 4096, 8)
        slot = field(q, 'pvr_compute_queue', 'slot')
        setfield(slot, 'gpu_queue_job', 'sequence', sequence)
        setfield(slot, 'gpu_queue_job', 'bindings', 1, 2)
        groups = field(slot, 'gpu_queue_job', 'groups')
        for index in range(3):
            put(groups + index * 4, 1)
        client = field(q, 'pvr_compute_queue', 'client')
        setfield(client, 'pvr_cccb', 'phase', enums['PVR_CCCB_IDLE'])
        fw = field(q, 'pvr_compute_queue', 'firmware') + 5 * sizes['pvr_mips_buffer']
        setfield(field(fw, 'pvr_mips_buffer', 'storage'), 'pvr_fw_buffer', 'cpu', timeline, 8)
        put(timeline, sequence - 1)
        setfield(field(owner, 'a733_gpu_queue', 'payload_extent'), 'device_memory_extent', 'cpu', payload, 8)
        if case == 'deadline-refused' or case == 'publication-failed':
            setfield(slot, 'gpu_queue_job', 'not_after_us', 1, 8)
        if case == 'abort-reserved':
            setfield(q, 'pvr_compute_queue', 'slot_state', enums['PVR_COMPUTE_QUEUE_RESERVED'])
        if case == 'create-ready':
            setfield(q, 'pvr_compute_queue', 'phase', enums['PVR_COMPUTE_QUEUE_FORMAT'])
            setfield(q, 'pvr_compute_queue', 'create_pending', 1)
            setfield(q, 'pvr_compute_queue', 'sequence', 0)
            setfield(q, 'pvr_compute_queue', 'slot_state', enums['PVR_COMPUTE_QUEUE_CLOSED'])
        state = {'posts': [], 'ring_begin': 0, 'publication_failed': False}
        def ret(value=0):
            u.reg_write(UC_ARM64_REG_X0, value)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
        def hook(machine, address, size, unused):
            name = hooks[address]
            args = [u.reg_read(UC_ARM64_REG_X0 + index) for index in range(4)]
            if name == 'memcpy':
                u.mem_write(args[0], bytes(u.mem_read(args[1], args[2])))
                ret(args[0])
            elif name == 'memset':
                u.mem_write(args[0], bytes([args[1] & 255]) * args[2])
                ret(args[0])
            elif name == 'pvr_gpu_service_now':
                assert crumbs == ([(0x0410, sequence)] if enabled else [])
                put(args[1], 100, 8)
                ret(case != 'fault-before-kick')
            elif name == 'pvr_cccb_begin':
                state['ring_begin'] += 1
                assert crumbs == ([(0x0410, sequence)] if enabled else [])
                setfield(client, 'pvr_cccb', 'phase', enums['PVR_CCCB_WAIT'])
                ret(1)
            elif name == 'pvr_cccb_poll':
                setfield(client, 'pvr_cccb', 'phase', enums['PVR_CCCB_IDLE'])
                put(timeline, sequence)
                ret(enums['PVR_CCCB_OK'])
            elif name == 'xQueueGenericSend':
                record = struct.unpack('<IHHII', bytes(u.mem_read(args[1], 16)))
                assert crumbs == ([(0x0410, sequence), (0x0411, sequence)] if enabled and case != 'create-ready' else [])
                assert get(field(q, 'pvr_compute_queue', 'crumb_pending')) == 0
                state['posts'].append(record)
                if case == 'publication-failed' and not state['publication_failed']:
                    state['publication_failed'] = True
                    ret(0)
                else:
                    ret(1)
            elif name in ('pvr_compute_job_grid_pds', 'pvr_compute_job_grid_cdm', 'pvr_cccb_abort'):
                ret()
            elif name == 'aarch64_device_cache':
                ret(1)
            else:
                raise AssertionError(('Unexpected model boundary', name))
        hooks = {image.address(name): name for name in names}
        hook_addresses(u, hooks, hook)
        def invoke(name):
            u.reg_write(UC_ARM64_REG_SP, 0x801ff000)
            u.reg_write(UC_ARM64_REG_LR, 0x80000000)
            u.reg_write(UC_ARM64_REG_X0, q)
            u.emu_start(image.address(name), 0x80000000, count=100000)
            assert u.reg_read(UC_ARM64_REG_PC) == 0x80000000, (case, 'execution budget')
            return u.reg_read(UC_ARM64_REG_X0)
        if case.startswith('abort-'):
            invoke('pvr_compute_queue_abort')
        else:
            result = invoke('pvr_compute_queue_poll')
            if case == 'success':
                assert result == 1 and not state['posts']
                invoke('pvr_compute_queue_poll')
            elif case in ('fault-before-kick', 'publication-failed'):
                assert result & (1 << 31)
                invoke('pvr_compute_queue_abort')
            elif case == 'fault-after-kick':
                assert not state['posts']
                invoke('pvr_compute_queue_abort')
        invoke('pvr_compute_queue_abort')  # Repeat abort never repeats a boundary.
        expected = [] if case in ('abort-reserved', 'create-ready') else [(0x0410, sequence), (0x0411, sequence)]
        assert crumbs == (expected if enabled else []), (case, enabled, crumbs)
        assert get(field(q, 'pvr_compute_queue', 'crumb_pending')) == 0
        if case not in ('abort-reserved', 'create-ready'):
            expected_result = enums['GPU_QUEUE_OK'] if case == 'success' else enums['GPU_QUEUE_REFUSED'] if case == 'deadline-refused' else enums['GPU_QUEUE_FAULT']
            assert state['posts'][-1][:2] == (sequence, expected_result), (case, state)
        rows.append({'crumbs_enabled': enabled, 'case': case, 'crumbs': crumbs, **state, 'passed': True})
save(a.output / 'results.json', {'cases': rows, 'source_sha256': {
    name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
    for name in ('drivers/gpu/pvr_compute_queue.c', 'include/pvr_compute_queue.h', 'include/sunstone_crumb.h')}})
print('PASS native GPU job breadcrumbs: %d cases; enabled two SVCs per claimed job, disabled zero' % len(rows))
