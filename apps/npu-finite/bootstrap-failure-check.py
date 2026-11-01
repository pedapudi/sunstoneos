#!/usr/bin/env python3
"""Execute bootstrap refusal branches in the linked image; no device access."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    image = args.native / 'image.elf'
    layouts = ('a733_npu_reserved', 'a733_npu_startup', 'a733_npu_startup_config',
               'device_memory_pool', 'device_memory_extent', 'device_memory_allocation',
               'a733_counter_clock', 'npu_runtime')
    im = harness.Image(image, layout_names=layouts)
    spec = importlib.util.spec_from_file_location('bootstrap_crumbs', ROOT / 'tools/a733-crumbs.py')
    crumbs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(crumbs)
    with image.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag != 'DW_TAG_structure_type':
                    continue
                fields = {d.attributes['DW_AT_name'].value.decode(): d.attributes['DW_AT_data_member_location'].value
                          for d in die.iter_children() if d.tag == 'DW_TAG_member'
                          and 'DW_AT_name' in d.attributes and 'DW_AT_data_member_location' in d.attributes}
                if {'used', 'startup', 'pmic_task', 'client'} <= fields.keys():
                    app_fields = fields
    rows = []
    runtime = 'npu_runtime_startup_poll' in im.symbols
    cases = (('unused-application', 0, 0), ('invalid-parent', 1, 1),
             ('counter-failure', 1, 1), ('deadline', 1, 0),
             ('startup-retained', 1, 4), ('activation-failure', 1, 4))
    for name, used, expected in cases:
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x40000000, 0x04000000)
        for address, data in im.parts:
            if data:
                u.mem_write(address, data)
        app = im.address('app')
        def put(address, value, size=8):
            u.mem_write(address, value.to_bytes(size, 'little'))
        put(app + app_fields['used'], used, 4)
        startup = app + app_fields['startup']
        put(startup + im.layouts['a733_npu_startup']['config'] + im.layouts['a733_npu_startup_config']['deadline_us'], 100)
        put(startup + im.layouts['a733_npu_startup']['stage'], 4, 4)
        if name not in ('unused-application', 'invalid-parent'):
            if runtime:
                rt, fields = app + app_fields['runtime'], im.layouts['npu_runtime']
                put(rt + fields['prepared'], 1, 4)
                put(rt + fields['parent_cpu'], im.address('__npu_runtime_parent_start'))
                put(rt + fields['parent_bytes'], 4096)
            else:
                reserved, fields = app + app_fields['reserved'], im.layouts['a733_npu_reserved']
                pool = 0x43000000
                put(reserved + fields['admitted'], 1, 4)
                put(reserved + fields['pool'], pool)
                put(app + app_fields['pool'], pool)
                put(pool + im.layouts['device_memory_pool']['initialized'], 1, 4)
                # This guard checks consistency of an already admitted parent;
                # zero-valued lease/allocation fields match both private copies.
                extent_bytes = im.layouts['device_memory_extent']['bytes']
                for extent in (reserved + fields['extent'], app + app_fields['extent']):
                    put(extent + extent_bytes, 1085440)  # baseline reserved bytes
                slot = pool + im.layouts['device_memory_pool']['allocations']
                put(slot + im.layouts['device_memory_allocation']['state'], 2, 4)
        state = {'failures': [], 'suspended': False, 'polls': 0, 'activations': 0}

        def returned(value):
            u.reg_write(UC_ARM64_REG_X0, value)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

        def code(cpu, address, size, unused):
            word = int.from_bytes(u.mem_read(address, 4), 'little')
            if word & 0xffe0001f == 0xd4000001:
                assert word >> 5 & 0xffff == 0x5c0e
                state['failures'].append([u.reg_read(UC_ARM64_REG_X0 + i) for i in range(3)])
                u.reg_write(UC_ARM64_REG_PC, address + 4)
            elif address == im.address('vTaskSuspend'):
                state['suspended'] = True
                u.emu_stop()
            elif address == im.address('a733_counter_clock_now'):
                if name == 'counter-failure':
                    put(app + app_fields['clock'] + im.layouts['a733_counter_clock']['failed'], 1, 4)
                    returned(0)
                else:
                    put(u.reg_read(UC_ARM64_REG_X0 + 1), 100 if name == 'deadline' else 50)
                    returned(1)
            elif address == im.address('npu_runtime_startup_poll' if runtime else 'npu_convolution_startup_poll'):
                state['polls'] += 1
                returned(3 if name == 'startup-retained' else 0)
            elif address == im.address('npu_service_activate'):
                state['activations'] += 1
                returned(4)

        u.hook_add(UC_HOOK_CODE, code)
        u.reg_write(UC_ARM64_REG_SP, 0x43ffe000)
        u.reg_write(UC_ARM64_REG_LR, 0x43fff000)
        u.reg_write(UC_ARM64_REG_X0, im.address('app'))
        u.emu_start(im.address('bootstrap'), 0, count=200000)
        assert state['suspended'] and state['failures'], state
        site, value, caller = state['failures'][0]
        source = crumbs.symbolize(image, caller - 4 if site >> 31 else caller)
        row = dict(case=name, first_site=hex(site), value=value, caller_source=source,
                   failure_records=state['failures'])
        rows.append(row)
        (args.evidence / 'result.json').write_text(json.dumps(rows, indent=2) + '\n')
        # The old image records only application_fail(4). A diagnostic must
        # identify this predicate before that common exit records its reason.
        assert value == expected and not site >> 31, row
        assert source and 'bootstrap' in source, row
        assert state['polls'] == int(name in ('startup-retained', 'activation-failure')), state
        assert state['activations'] == int(name == 'activation-failure'), state
    assert len({row['first_site'] for row in rows}) == len(rows), rows
    print('PASS linked bootstrap failures: six distinct predicates')


if __name__ == '__main__':
    main()
