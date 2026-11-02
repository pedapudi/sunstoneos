#!/usr/bin/env python3
"""Execute the linked scheduler tick with ready lists of different lengths.

Only the core identity and SGI publication are substituted. Real list traversal,
affinity checks, yield requests and tick advancement execute in Unicorn. Counts
describe instruction-model work; they are not physical timing measurements.
"""
import argparse
import hashlib
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_PSTATE, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1

from harness import Image, hook_addresses

CHECK_INPUTS = ['patches/freertos-smp-tasks.patch', 'include/FreeRTOSConfig.h',
                'platform/a733-smp-cached/FreeRTOSConfig.h']
SCRATCH = 0x90000000
STOP = SCRATCH + 0x3ff000
CORES = 8


def run(image, segments, sizes, priorities, waiting):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.reg_write(UC_ARM64_REG_PSTATE, 5)
    ranges = []
    for address, length in sorted(segments):
        start, end = address & ~4095, (address + length + 4095) & ~4095
        if ranges and start <= ranges[-1][1]:
            ranges[-1][1] = max(ranges[-1][1], end)
        else:
            ranges.append([start, end])
    for start, end in ranges:
        assert end <= SCRATCH or start >= SCRATCH + 0x400000
        cpu.mem_map(start, end - start)
    cpu.mem_map(SCRATCH, 0x400000)
    for address, data in image.parts:
        if data:
            cpu.mem_write(address, data)
    cpu.reg_write(UC_ARM64_REG_SP, STOP - 16)
    cpu.reg_write(UC_ARM64_REG_LR, STOP)

    def put(address, value, width=8):
        cpu.mem_write(address, int(value).to_bytes(width, 'little', signed=value < 0))

    def get(address, width=8):
        return int.from_bytes(cpu.mem_read(address, width), 'little')

    tcb, item, ready = (image.layouts[n] for n in ('tskTaskControlBlock', 'xLIST_ITEM', 'xLIST'))
    lists = [[] for _ in range(8)]
    owners = set()
    current = image.address('pxCurrentTCBs')

    def task(index, priority, affinity, state):
        address = SCRATCH + index * 1024
        put(address + tcb['uxPriority'], priority)
        put(address + tcb['uxCoreAffinityMask'], affinity)
        put(address + tcb['xTaskRunState'], state)
        link = address + tcb['xStateListItem']
        put(link + item['pvOwner'], address)
        owners.add(link + item['pvOwner'])
        lists[priority].append(link)
        return address

    for core, priority in enumerate(priorities):
        put(current + core * 8, task(core, priority, 1 << core, core))
    for index, (priority, affinity) in enumerate(waiting, CORES):
        task(index, priority, affinity, -1)
    for priority, links in enumerate(lists):
        address = image.address('pxReadyTasksLists') + priority * sizes['xLIST']
        sentinel = address + ready['xListEnd']
        put(address + ready['uxNumberOfItems'], len(links))
        put(address + ready['pxIndex'], sentinel)
        put(sentinel + item['xItemValue'], 0xffffffff, 4)
        ring = [sentinel, *links]
        for index, link in enumerate(ring):
            put(link + item['pxNext'], ring[(index + 1) % len(ring)])
            put(link + item['pxPrevious'], ring[index - 1])
    put(image.address('uxSchedulerSuspended'), 0)
    put(image.address('xTickCount'), 100, 4)
    put(image.address('xNextTaskUnblockTime'), 0xffffffff, 4)
    put(image.address('xYieldPendings'), 0, CORES * 8)
    visited, instructions, sgis = [], [0], []

    def code(uc, address, size, unused):
        instructions[0] += 1

    def load(uc, access, address, size, value, unused):
        if address in owners:
            visited.append(address)

    def substitute(uc, address, size, unused):
        if address == image.address('vInterruptCore'):
            assert uc.reg_read(UC_ARM64_REG_X0) == 0
            sgis.append(uc.reg_read(UC_ARM64_REG_X1))
        else:
            uc.reg_write(UC_ARM64_REG_X0, 0)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, code)
    cpu.hook_add(UC_HOOK_MEM_READ, load)
    hook_addresses(cpu, [image.address('ucPortGetCoreID'), image.address('vInterruptCore')], substitute)
    cpu.emu_start(image.address('xTaskIncrementTick'), STOP, count=500000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == STOP, 'tick exceeded instruction bound'
    assert get(image.address('xTickCount'), 4) == 101
    mask = sum(bool(get(image.address('xYieldPendings') + core * 8)) << core for core in range(CORES))
    assert sorted(sgis) == [core for core in range(1, CORES) if mask & (1 << core)]
    assert bool(cpu.reg_read(UC_ARM64_REG_X0)) == bool(mask & 1)
    return {'visits': len(visited), 'instructions': instructions[0], 'yield_mask': mask}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true', help='check the uncapped scheduler')
    args = parser.parse_args()
    image = Image(args.image, {'tskTaskControlBlock', 'xLIST_ITEM', 'xLIST'})
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        segments = [(int(p['p_vaddr']), int(p['p_memsz'])) for p in elf.iter_segments()
                    if p['p_type'] == 'PT_LOAD' and p['p_memsz']]
        sizes = {}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if die.tag == 'DW_TAG_structure_type' and name and name.value == b'xLIST':
                    sizes['xLIST'] = int(die.attributes['DW_AT_byte_size'].value)
    cases = {
        'running-only': ([2] * CORES, [], 0),
        'affinity-excluded': ([2] * CORES, [(2, 2)], 2),
        'exact-limit': ([2] * CORES, [(2, 2)] * 8, 2),
        'past-limit': ([2] * CORES, [(2, 2)] * 8 + [(2, 1)], 3 if args.baseline else 255),
        'long-list': ([2] * CORES, [(2, 2)] * 1015 + [(2, 1)], 3 if args.baseline else 255),
        'several-priorities': ([2] * 4 + [3] * 4, [(2, 2)] * 20 + [(3, 128)] * 20,
                               130 if args.baseline else 255),
        'priority-narrowing': ([2] * 4 + [3] * 4, [(2, 2)] * 20, 2 if args.baseline else 15),
        'negative-long-list': ([2] * 4 + [3] * 4, [(2, 128)] * 20, 0 if args.baseline else 15),
        'last-affinity': ([2] * CORES, [(2, 2)] * 7 + [(2, 128)], 130),
        'idle-only': ([0] * CORES, [], 0),
    }
    results = {}
    for name, (priorities, waiting, expected) in cases.items():
        result = run(image, segments, sizes, priorities, waiting)
        assert result['yield_mask'] == expected, (name, result, expected)
        if not args.baseline:
            assert result['visits'] <= 16 * len(set(priorities) - {0}), (name, result)
        results[name] = result
    if not args.baseline:
        assert results['long-list']['visits'] == results['past-limit']['visits'] == 0
        assert results['long-list']['instructions'] == results['past-limit']['instructions']
        assert results['exact-limit']['visits'] == 16
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
                                      'baseline': args.baseline, 'cases': results}, indent=2) + '\n')
    print('PASS linked tick: affinity, exact scan limit, conservative fallback and bounded long lists')


if __name__ == '__main__':
    main()
