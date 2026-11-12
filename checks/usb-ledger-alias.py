#!/usr/bin/env python3
"""Execute the ledger-backed USB constructor and its EL0 payload alias.

The instruction model follows docs/designs/unified-memory.md and the USB
payload permissions in docs/designs/usb-host-classes.md. Architectural AT/PAR,
TLBs and cache completion are modeled; no hardware qualification is claimed.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness

GATE_RUNS = [['--image', '{elf:os-usb-ledger}', '--output', '{output}']]
CHECK_INPUTS = ['checks/ledger-nc.py', 'checks/cpu-table-walks.py', 'apps/os/usb-memory.c',
                'apps/os/usb-memory.h', 'platform/a733-smp-cached/memory.c',
                'include/a733_usb_memory.h', 'arch/aarch64/device_memory.c']

PAGE, BLOCK = 4096, 0x200000
USB_ALIAS, DATA_OFFSET, DATA_BYTES = 0x7a000000, 0x40000, 0x400000
ALIAS_PAGES = (DATA_OFFSET + DATA_BYTES + BLOCK - 1) // BLOCK
PAYLOAD = range(USB_ALIAS + DATA_OFFSET, USB_ALIAS + DATA_OFFSET + DATA_BYTES, PAGE)
ADDRESS = 0x0000fffffffff000
LEAF_FLAGS = (3 << 53) | (1 << 10) | (3 << 8) | (3 << 6) | 8 | 3


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ledger = load('usb_alias_ledger', ROOT / 'checks/ledger-nc.py')
walks = load('usb_alias_boot', ROOT / 'checks/cpu-table-walks.py')
# The reusable boot model runs the linked OS on the same maximal CPU as the
# application-side models. It needs no private Unicorn configuration.
walks.Uc = lambda *_: harness.machine()


def array_count(path, structure, member):
    counts = set()
    with path.open('rb') as stream:
        dwarf = ELFFile(stream).get_dwarf_info()
        for unit in dwarf.iter_CUs():
            if not unit.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('drivers/usb/a733_usb2_native.c'):
                continue
            for die in unit.iter_DIEs():
                label = die.attributes.get('DW_AT_name')
                if die.tag != 'DW_TAG_structure_type' or label is None or label.value.decode() != structure:
                    continue
                for field in die.iter_children():
                    name = field.attributes.get('DW_AT_name')
                    if field.tag != 'DW_TAG_member' or name is None or name.value.decode() != member:
                        continue
                    array = field.get_DIE_from_attribute('DW_AT_type')
                    assert array.tag == 'DW_TAG_array_type'
                    for bounds in array.iter_children():
                        if bounds.tag == 'DW_TAG_subrange_type':
                            count = bounds.attributes.get('DW_AT_count')
                            counts.add(int(count.value) if count else int(bounds.attributes['DW_AT_upper_bound'].value) + 1)
    assert len(counts) == 1, (structure, member, counts)
    return counts.pop()


def boot_tables(image, path):
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        assert all(not section['sh_size'] for section in elf.iter_sections() if section.name == '.uncached_pool'), \
            'the OS still reserves an uncached link-script arena'
        assert not any('USB_NC' in section.name for section in elf.iter_sections())
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                start, end = int(segment['p_vaddr']), int(segment['p_vaddr']) + int(segment['p_memsz'])
                assert not (start < 0x42a00000 and 0x42200000 < end), 'the old 8 MiB USB arena remains in a load segment'
    assert image.address('__uncached_pool_start') == image.address('__uncached_pool_end')
    boot = walks.Machine(image)
    assert boot.call('a733_cached_memory_prepare') == 1, 'the linked USB boot profile refuses preparation'
    pages = {name: bytes(boot.uc.mem_read(image.address(name), length)) for name, length in (
        ('a733_cached_el1_l1', PAGE), ('a733_cached_el1_l2', PAGE),
        ('a733_cached_usb_el1_l3', ALIAS_PAGES * PAGE), ('a733_cached_table_alias_l3', PAGE))}
    assert pages['a733_cached_usb_el1_l3'] == bytes(ALIAS_PAGES * PAGE), 'boot exposes payload before its allocation'
    return pages


class AliasModel(ledger.Model):
    def __init__(self, image, seed, variant='normal'):
        super().__init__(image)
        self.usb_variant = variant
        self.alias_page = image.address('a733_cached_usb_el1_l3')
        self.alias_entry = image.address('os_usb_memory_alias')
        self.owner_entry = image.address('os_owner_memory_below')
        self.alias_bound = image.address('bound_base', unit='platform/a733-smp-cached/memory.c')
        self.pool_published = image.address('published', unit='apps/os/usb-memory.c')
        self.alias_active, self.alias_calls, self.owner_calls = None, [], []
        self.alias_tlb = {va: 0x42200000 + va - USB_ALIAS for va in PAYLOAD}
        self.regs = dict(daif=0x140, par_el1=0x12345001)
        self.completed_binding = False
        for name, data in seed.items():
            self.u.mem_write(image.address(name), data)
        self.u.mem_write(ledger.ALIAS, seed['a733_cached_el1_l1'])
        self.u.mem_write(ledger.ALIAS + PAGE, seed['a733_cached_el1_l2'])
        self.u.mem_write(ledger.ALIAS + 2 * PAGE, seed['a733_cached_usb_el1_l3'])
        self.table_regions.append((ledger.ALIAS + 2 * PAGE, ledger.ALIAS + (2 + ALIAS_PAGES) * PAGE))
        harness.hook_addresses(self.u, [self.alias_entry, self.owner_entry], self.code)
        immediate_masks = harness.instruction_sites(image.parts, lambda word:
            word & 0xfffff0ff in (0xd50340df, 0xd50340ff))
        harness.hook_addresses(self.u, immediate_masks, self.code)

    def call(self, name, *args):
        for index, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + index, value)
        self.u.reg_write(UC_ARM64_REG_SP, ledger.STACK)
        self.u.reg_write(UC_ARM64_REG_LR, ledger.RETURN)
        # The constructor zeroes its declared backing. The instruction budget
        # scales with that input rather than imposing a runtime memory bound.
        budget = max(3000000, self.image.backing_bytes * 4)
        self.u.emu_start(self.image.address(name), ledger.RETURN + 4, count=budget)
        assert self.u.reg_read(UC_ARM64_REG_PC) == ledger.RETURN, (name, 'instruction budget expired')
        assert self.lock_depth == 0
        return self.u.reg_read(UC_ARM64_REG_X0)

    def leaf(self, va):
        first = self.read(self.root + (va >> 30) * 8)
        if first & 3 == 1:
            return first, ledger.GIB
        if first & 3 != 3:
            return 0, 0
        second = self.read((first & ADDRESS) + ((va >> 21) & 511) * 8)
        if second & 3 == 1:
            return second, BLOCK
        if second & 3 != 3:
            return 0, 0
        third = self.read((second & ADDRESS) + ((va >> 12) & 511) * 8)
        return (third, PAGE) if third & 3 == 3 else (0, 0)

    def store(self, u, access, address, size, value, unused):
        if ledger.ALIAS + 2 * PAGE <= address < ledger.ALIAS + (2 + ALIAS_PAGES) * PAGE:
            offset = address - ledger.ALIAS - 2 * PAGE
            u.mem_write(self.alias_page + offset, value.to_bytes(size, 'little'))
            state = self.alias_active
            assert state is not None, 'USB alias descriptor changes outside its binding operation'
            assert size == 8 and not offset & 7
            if state['phase'] == 'validate':
                assert state['ats'] == len(PAYLOAD) and self.regs == state['saved_registers'], \
                    'alias publication precedes complete AT validation or register restoration'
                state['phase'] = 'clear'
            if state['phase'] == 'clear':
                assert value == 0
                state['clears'] += 1
            else:
                assert state['phase'] == 'make', ('table store outside BBM phase', state['phase'])
                page_offset = offset // 8 * PAGE
                expected = (state['base'] + page_offset) | LEAF_FLAGS if DATA_OFFSET <= page_offset < DATA_OFFSET + DATA_BYTES else 0
                assert value == expected, ('payload permissions or physical address differ', hex(page_offset), hex(value), hex(expected))
                if value:
                    va = USB_ALIAS + page_offset
                    assert va not in self.alias_tlb, 'retained alias TLB survives the binding invalidate'
                    self.alias_tlb[va] = state['base'] + page_offset
                state['makes'] += 1
            u.mem_write(address, value.to_bytes(size, 'little'))
        if address == self.alias_bound and value:
            assert self.alias_active and self.alias_active['phase'] == 'complete', 'binding base is published before completed tables'
        if address == self.pool_published and value:
            assert self.completed_binding, 'typed pool is published before its payload alias completes'
        super().store(u, access, address, size, value, unused)

    def code(self, u, address, size, unused):
        state = self.alias_active
        if state is not None and address == state['return']:
            state['status'] = ledger.signed(u.reg_read(UC_ARM64_REG_X0))
            assert self.regs == state['saved_registers'], 'AT changes DAIF or PAR at alias return'
            if state['status'] and not state['existing']:
                assert state['phase'] == 'complete'
                self.completed_binding = True
            self.alias_calls.append(state)
            self.alias_active = None
            if address != ledger.RETURN:
                return
        if address == self.alias_entry:
            assert self.alias_active is None
            self.alias_active = dict(base=u.reg_read(UC_ARM64_REG_X0), bytes=u.reg_read(UC_ARM64_REG_X0 + 1),
                existing=self.read(self.alias_bound), phase='validate', ats=0, clears=0, makes=0,
                tlbis=[], barriers=[], saved_registers=self.regs.copy(), **{'return': u.reg_read(UC_ARM64_REG_LR)})
            at = self.alias_active['return']
            if at not in self.return_hooks:
                harness.hook_addresses(u, [at], self.code)
                self.return_hooks.add(at)
            return
        if address == self.owner_entry:
            self.owner_calls.append(tuple(u.reg_read(UC_ARM64_REG_X0 + index) for index in range(4)))
            return
        if address in (ledger.RETURN, self.entry, self.critical_enter, self.critical_exit) or address in self.return_hooks:
            return super().code(u, address, size, unused)
        if address not in self.instructions:
            self.instructions[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        op = self.instructions[address]
        args = op.op_str.lower().split(', ') if op.op_str else []
        state = self.alias_active
        if op.mnemonic == 'mrs' and args[1] in ('daif', 'par_el1'):
            u.reg_write(ledger.reg(args[0]), self.regs[args[1]])
            return self.skip(address)
        if op.mnemonic == 'msr':
            name, source = args
            if name == 'daifset':
                self.regs['daif'] |= int(source.lstrip('#'), 0) << 6
            elif name == 'daifclr':
                self.regs['daif'] &= ~(int(source.lstrip('#'), 0) << 6)
            else:
                assert name in self.regs, ('unmodeled register write', op.op_str)
                self.regs[name] = u.reg_read(ledger.reg(source))
            return self.skip(address)
        if op.mnemonic == 'at':
            assert state is not None and state['phase'] == 'validate' and args[0] == 's1e1w'
            assert self.regs['daif'] & 0x3c0 == 0x3c0, 'AT is not protected by masked exceptions'
            va = u.reg_read(ledger.reg(args[1]))
            assert va == state['base'] + DATA_OFFSET + state['ats'] * PAGE
            descriptor, granule = self.leaf(va)
            assert descriptor and descriptor & (1 << 7) == 0, 'the identity mapping is not writable at EL1'
            physical = (descriptor & ADDRESS & ~(granule - 1)) | (va & (granule - 1))
            attr = {1: 0xff, 2: 0x44}.get((descriptor >> 2) & 7, 0)
            share = (descriptor >> 8) & 3
            if self.usb_variant == 'at-fault' and state['ats'] == len(PAYLOAD) // 2:
                par = 1
            else:
                if self.usb_variant == 'at-physical': physical ^= PAGE
                if self.usb_variant == 'at-attribute': attr = 0xff
                if self.usb_variant == 'at-shareability': share = 0
                par = (physical & ADDRESS) | (attr << 56) | (share << 7)
            self.regs['par_el1'] = par
            state['ats'] += 1
            return self.skip(address)
        if state is not None and op.mnemonic == 'tlbi':
            assert state['phase'] == 'invalidate' and args[0] == 'vaae1is'
            va = u.reg_read(ledger.reg(args[1])) << 12
            assert va == PAYLOAD.start + len(state['tlbis']) * PAGE
            state['tlbis'].append(va)
            if self.usb_variant != 'omit-tlbi': self.alias_tlb.pop(va, None)
            self.operations['tlbi'] += 1
            return self.skip(address)
        if state is not None and op.mnemonic in ('dsb', 'isb'):
            if state['phase'] == 'validate':
                assert op.mnemonic == 'isb', 'unexpected barrier during AT validation'
            else:
                event = op.mnemonic + (':' + args[0] if args else '')
                state['barriers'].append(event)
                if state['phase'] == 'clear':
                    assert event == 'dsb:ish' and state['clears'] == ALIAS_PAGES * 512
                    state['phase'] = 'invalidate'
                elif state['phase'] == 'invalidate':
                    assert event == 'dsb:ish' and len(state['tlbis']) == len(PAYLOAD)
                    state['phase'] = 'invalidate-complete'
                elif state['phase'] == 'invalidate-complete':
                    assert event == 'isb'
                    state['phase'] = 'make'
                elif state['phase'] == 'make':
                    assert event == 'dsb:ish' and state['makes'] == ALIAS_PAGES * 512
                    state['phase'] = 'make-complete'
                elif state['phase'] == 'make-complete':
                    assert event == 'isb'
                    state['phase'] = 'complete'
                else:
                    raise AssertionError(('unexpected binding barrier', state['phase'], event))
            return self.skip(address)
        return super().code(u, address, size, unused)

    def zero_alias(self):
        assert bytes(self.u.mem_read(self.alias_page, ALIAS_PAGES * PAGE)) == bytes(ALIAS_PAGES * PAGE)
        assert bytes(self.u.mem_read(ledger.ALIAS + 2 * PAGE, ALIAS_PAGES * PAGE)) == bytes(ALIAS_PAGES * PAGE)

    def allocate(self, policy=1, owner=0xffff):
        if owner == 0xffff:
            result = self.call('os_owner_memory_below', self.image.backing_bytes, BLOCK, policy, 1 << 32)
            assert result
            return result
        self.bootstrap()
        status, handle = self.request(ledger.FLOOR + BLOCK, policy, owner=owner, size=self.image.backing_bytes)
        assert status == 0
        return self.read(handle + self.field('umem_buf', 'base'))

    def refuse(self, base, size):
        before = bytes(self.u.mem_read(self.alias_page, ALIAS_PAGES * PAGE))
        assert self.call('os_usb_memory_alias', base, size) == 0
        state = self.alias_calls[-1]
        assert not state['clears'] and not state['makes'] and not state['tlbis']
        assert bytes(self.u.mem_read(self.alias_page, ALIAS_PAGES * PAGE)) == before
        assert self.read(self.alias_bound) == 0
        return dict(at_pages=state['ats'], alias_stores=0, tlbi=0)


def prepare_image(path):
    layouts = ('umem', 'umem_request', 'umem_buf', 'umem_extent', 'umem_el1_map', 'umem_policy_table',
               'ss_manifest', 'ss_region', 'ss_application', 'app_space', 'device_memory_pool', 'device_memory_extent')
    image = harness.Image(path, layout_names=layouts)
    image.tcr = ledger.a733_tcr.for_image(path)
    assert image.tcr == ledger.a733_tcr.CACHED_WALK, 'USB ledger cases require cacheable table walks'
    image.sizes = ledger.structure_sizes(path, ('ss_manifest', 'ss_region', 'umem_policy_table'))
    image.admitted_storage = ledger.variable_storage(path, 'apps/os/large.c', 'admitted')
    image.devices = array_count(path, 'a733_xhci', 'devices')
    image.endpoints = array_count(path, 'a733_xhci_device', 'endpoints')
    assert 1 <= image.devices <= 127 and 2 <= image.endpoints <= 31
    record_bytes = PAGE * (2 + image.endpoints * 2)
    head_records = (DATA_OFFSET - (16384 + 8 * PAGE)) // record_bytes
    image.parent_bytes = DATA_OFFSET + DATA_BYTES + max(0, image.devices - head_records) * record_bytes
    image.backing_bytes = (image.parent_bytes + 0x300000 + BLOCK - 1) & ~(BLOCK - 1)
    return image


def finish(args, image, digest, cases):
    assert hashlib.sha256(args.image.read_bytes()).hexdigest() == digest, 'image changes during validation'
    harness.save(args.output / 'result.json', dict(passed=True, physical_access=False, image=str(args.image),
        image_sha256=digest, cases=cases,
        scope='Actual OS boot tables, NC ledger allocation, USB pool construction and alias instructions; AT/PAR, cache and TLB effects are modeled. No physical USB or coherence qualification.'))
    print(f'PASS USB ledger alias: {len(cases)} linked instruction-model cases, {image.devices} device records')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--binding-only', action='store_true', help='check construction and payload exclusion for another compiled device count')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(args.image.read_bytes()).hexdigest()
    image = prepare_image(args.image)
    seed = boot_tables(image, args.image)
    cases = {'boot-link-shape': dict(initial_alias_entries=ALIAS_PAGES * 512, nonzero_entries=0, static_nc_bytes=0)}

    model = AliasModel(image, seed)
    model.zero_alias()
    assert model.call('a733_usb_memory_parent_base') == 0
    declared = model.read(image.address('ss_usb_devices'), 4)
    assert declared in (0, image.devices)
    pool = model.call('a733_memory_uncached_init')
    assert pool and model.call('a733_usb_memory_pool_valid', pool) == 1
    assert model.owner_calls == [(image.backing_bytes, BLOCK, 1, 1 << 32)]
    assert len(model.alias_calls) == 1
    binding = model.alias_calls[0]
    assert binding['status'] == 1 and binding['bytes'] == image.parent_bytes and binding['phase'] == 'complete'
    base = binding['base']
    assert model.call('a733_usb_memory_parent_base') == base
    backing = pool + image.layouts['device_memory_pool']['backing']
    assert tuple(model.read(backing + image.layouts['device_memory_extent'][field]) for field in ('cpu', 'physical', 'bytes')) == \
        (base, base, image.backing_bytes)
    for offset in range(0, image.backing_bytes, PAGE):
        descriptor, granule = model.leaf(USB_ALIAS + offset)
        expected = (base + offset) | LEAF_FLAGS if DATA_OFFSET <= offset < DATA_OFFSET + DATA_BYTES else 0
        assert descriptor == expected and (granule == PAGE if expected else granule == 0)
    assert binding['barriers'] == ['dsb:ish', 'dsb:ish', 'isb', 'dsb:ish', 'isb']
    cases['constructor-binding'] = dict(devices=image.devices, endpoints=image.endpoints, parent_bytes=image.parent_bytes,
        backing_bytes=image.backing_bytes, base=hex(base), validated_pages=binding['ats'], payload_pages=len(PAYLOAD),
        clears=binding['clears'], makes=binding['makes'], tlbi=len(binding['tlbis']), barriers=binding['barriers'])
    before = bytes(model.u.mem_read(model.alias_page, ALIAS_PAGES * PAGE))
    assert model.call('os_usb_memory_alias', base, image.parent_bytes) == 1
    assert model.call('os_usb_memory_alias', base + BLOCK, image.parent_bytes) == 0
    assert model.call('os_usb_memory_alias', base, image.parent_bytes - PAGE) == 0
    assert bytes(model.u.mem_read(model.alias_page, ALIAS_PAGES * PAGE)) == before
    assert all(not call['ats'] and not call['clears'] and not call['tlbis'] for call in model.alias_calls[1:])
    cases['idempotence-and-rebinding'] = dict(same_binding=1, different_binding=0, wrong_bytes=0, alias_stores=0)
    if args.binding_only:
        finish(args, image, digest, cases)
        return

    model = AliasModel(image, seed)
    model.zero_alias()
    cases['before-allocation'] = model.refuse(ledger.FLOOR, image.parent_bytes)
    for base, size in ((0, image.parent_bytes), (ledger.FLOOR + PAGE, image.parent_bytes),
                       (ledger.FLOOR, image.parent_bytes - PAGE), (1 << 32, image.parent_bytes)):
        model.refuse(base, size)
    cases['malformed-arguments'] = dict(refused=4, alias_stores=0, at_pages=0)

    for policy, owner, name in ((0, 0xffff, 'write-back-policy'), (1, 7, 'foreign-owner')):
        model = AliasModel(image, seed)
        base = model.allocate(policy=policy, owner=owner)
        cases[name] = model.refuse(base, image.parent_bytes)

    for variant in ('at-fault', 'at-physical', 'at-attribute', 'at-shareability'):
        model = AliasModel(image, seed, variant)
        base = model.allocate()
        cases[variant] = model.refuse(base, image.parent_bytes)
    assert cases['at-fault']['at_pages'] == len(PAYLOAD) // 2 + 1

    model = AliasModel(image, seed)
    base = model.allocate()
    admitted, width = image.admitted_storage
    model.u.mem_write(admitted, (1).to_bytes(width, 'little'))
    cases['application-active'] = model.refuse(base, image.parent_bytes)

    model = AliasModel(image, seed, 'omit-tlbi')
    base = model.allocate()
    cases['omitted-tlbi'] = harness.refused(lambda: model.call('os_usb_memory_alias', base, image.parent_bytes),
        'retained alias TLB survives', AssertionError)

    finish(args, image, digest, cases)


if __name__ == '__main__':
    main()
