#!/usr/bin/env python3
"""Execute ledger cache-policy conversion and its EL1 maintenance in the linked OS image.

The model follows the free-chunk and break-before-make contract in
docs/designs/unified-memory.md. It models architectural registers, cache-line
completion and per-address-space TLBs; it does not qualify physical coherence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import UC_HOOK_MEM_INVALID, UC_HOOK_MEM_WRITE, UC_MEM_READ_UNMAPPED, UC_MEM_WRITE_UNMAPPED
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import a733_tcr

# This OS application has no pinned upper-memory inputs; the cases below add
# their own reservation fixture without depending on an external application.
GATE_RUNS = [['--image', '{elf:ret-walks-cached}', '--output', '{output}']]
CHECK_INPUTS = ['apps/os/large.c', 'apps/os/owner-memory.h', 'kernel/umem/umem.c',
                'kernel/umem/umem_policy.c', 'arch/aarch64/umem_cpu.c',
                'arch/aarch64/device_memory.c', 'include/umem.h', 'include/umem_policy.h',
                'include/os_memory_tables.h', 'docs/designs/unified-memory.md',
                'apps/slot1-test/manifest-preserve.json']

CASES = ('owner-nc-allocation', 'private-l2-policy', 'chunk-in-use', 'maintenance-refusal',
         'nc-to-wb', 'omit-cache', 'omit-tlbi', 'out-of-map', 'pinned-reservations',
         'coalesced-root-refusal', 'omit-table-clean')

BLOCK = 0x200000
GIB = 0x40000000
FLOOR = 0x48000000
ALIAS = 0x79e00000
ADDRESS = 0x0000fffffffff000
ATTR = 7 << 2
KERNEL_FLAGS = (3 << 53) | (1 << 11) | (1 << 10) | (3 << 8) | (1 << 2) | 1
SCRATCH, STACK, RETURN = 0x47d00000, 0x47e00000, 0x47fff000


def reg(name):
    return UC_ARM64_REG_X0 + int(name[1:])


class Model:
    def __init__(self, image, variant='normal'):
        self.image, self.variant = image, variant
        self.u = harness.machine()
        self.u.mem_map(0x40000000, 0x10000000)
        self.u.mem_map(ALIAS, 0x6000)
        for address, data in image.parts:
            self.u.mem_write(address, data)
        self.root = image.address('a733_cached_el1_l1')
        self.slot1 = image.address('a733_cached_el1_l2')
        self.ledger = image.address('ledger', unit='apps/os/large.c')
        self.policy = image.address('policy_map', unit='apps/os/large.c')
        self.roots = [self.root]
        self.tlb = {}
        self.cache_stride = True
        self.active = None
        self.conversions = []
        self.lock_depth = 0
        self.operations = dict(civac=0, table_cleans=0, tlbi=0, descriptor_stores=0)
        self.uncached_walk = not image.tcr & 0xf00
        self.table_dirty, self.table_clean_pending = set(), set()
        self.table_regions = [(ALIAS, ALIAS + 8192)]
        self.split_bytes = ((self.field('umem_el1_map', 'split_mask') - self.field('umem_el1_map', 'split')) //
                            image.sizes['umem_policy_table']) * 4096
        self.sparse_pages = set()
        self.decoder, self.instructions = Cs(CS_ARCH_ARM64, CS_MODE_ARM), {}
        self.entry = image.address('umem_policy_convert')
        self.critical_enter = image.address('vTaskEnterCritical')
        self.critical_exit = image.address('vTaskExitCritical')
        self.return_hooks = set()
        root, l2 = [0] * 512, [0] * 512
        root[1] = self.slot1 | 3
        for slot in range(2, 13):
            root[slot] = slot * GIB | KERNEL_FLAGS
        for index in range(512):
            physical = GIB + index * BLOCK
            if physical < 0x79000000:
                l2[index] = physical | KERNEL_FLAGS | ((1 << 6) if index < 64 else 0)
        l2[16] = 0
        self.write_words(self.root, root)
        self.write_words(self.slot1, l2)
        self.write_words(ALIAS, root)
        self.write_words(ALIAS + 4096, l2)
        self.u.hook_add(UC_HOOK_MEM_WRITE, self.store)
        self.u.hook_add(UC_HOOK_MEM_INVALID, self.unmapped)
        sites = harness.instruction_sites(image.parts, lambda word:
            harness.system_register_access(word) or bool(harness.barriers(word)))
        harness.hook_addresses(self.u, [*sites, self.entry, self.critical_enter, self.critical_exit, RETURN], self.code)

    def read(self, address, size=8):
        return int.from_bytes(self.u.mem_read(address, size), 'little')

    def write_words(self, address, values):
        self.u.mem_write(address, struct.pack('<' + 'Q' * len(values), *values))

    def field(self, structure, name):
        return self.image.layouts[structure][name]

    def walk(self, root, va):
        first = self.read(root + (va >> 30) * 8)
        if first & 3 == 1:
            return first
        if first & 3 != 3:
            return 0
        second = self.read((first & ADDRESS) + ((va >> 21) & 511) * 8)
        return second if second & 3 == 1 else 0

    def translate(self, root, va):
        key = (root, va & ~(BLOCK - 1))
        if key not in self.tlb:
            value = self.walk(root, va)
            if value:
                self.tlb[key] = value
        return self.tlb.get(key, 0)

    def audit(self):
        if self.active is None:
            return
        chunk = self.active['chunk']
        attrs = {(value & ATTR) for root in self.roots if (value := self.walk(root, chunk))}
        attrs.update(value & ATTR for (root, va), value in self.tlb.items() if va == chunk)
        assert len(attrs) <= 1, 'cached translation survives make with another cache policy'

    def speculative_read(self):
        if self.active is not None:
            for root in self.roots:
                self.translate(root, self.active['chunk'])
            self.audit()

    def entries(self, chunk):
        result, table = [], self.read(self.policy + self.field('umem_el1_map', 'tables'))
        while table:
            base = self.read(table + self.field('umem_policy_table', 'base'))
            page = self.read(table + self.field('umem_policy_table', 'l2'))
            if base <= chunk < base + GIB:
                address = page + ((chunk - base) // BLOCK) * 8
                result.append((address, self.read(address)))
            table = self.read(table + self.field('umem_policy_table', 'next'))
        return result

    def store(self, u, access, address, size, value, unused):
        split = self.read(self.policy + self.field('umem_el1_map', 'split_pages'))
        if any(lo <= address < hi for lo, hi in self.table_regions) or split and split <= address < split + self.split_bytes:
            self.operations['descriptor_stores'] += 1
            if self.uncached_walk:
                line = address & ~63
                self.table_dirty.add(line)
                self.table_clean_pending.discard(line)
        if ALIAS <= address < ALIAS + 8192:
            physical = (self.root if address < ALIAS + 4096 else self.slot1) + (address & 4095)
            u.mem_write(physical, value.to_bytes(size, 'little'))
        if self.active is None:
            return
        targets = {entry for entry, old in self.active['before']}
        if address not in targets:
            return
        state = self.active
        assert size == 8, 'descriptor is not stored atomically as one 64-bit word'
        if not value & 1:
            assert len(state['cleaned']) == BLOCK // 64 and state['cache_complete'], \
                'cache maintenance incomplete before break'
            state['events'].append('break')
        else:
            state['events'].append('make')
        # CPU writes are observed before memory changes. Publish the modeled
        # table word for the speculation check after this instruction.
        u.mem_write(address, value.to_bytes(size, 'little'))
        if ALIAS <= address < ALIAS + 8192:
            physical = (self.root if address < ALIAS + 4096 else self.slot1) + (address & 4095)
            u.mem_write(physical, value.to_bytes(size, 'little'))
        self.speculative_read()

    def unmapped(self, u, access, address, size, value, unused):
        """Map only the table extent chosen by the real ledger allocator."""
        split = self.read(self.policy + self.field('umem_el1_map', 'split_pages'))
        assert access in (UC_MEM_READ_UNMAPPED, UC_MEM_WRITE_UNMAPPED), 'unexpected unmapped execution or protection fault'
        assert split and split <= address and address + size <= split + self.split_bytes, \
            ('access outside the allocated split-table extent', hex(address), hex(split))
        for page in range(split, split + self.split_bytes, 4096):
            first = self.read(self.root + (page >> 30) * 8)
            descriptor = self.walk(self.root, page)
            assert descriptor and descriptor & ATTR == 1 << 2, 'table storage is not mapped write-back'
            granule = GIB if first & 3 == 1 else BLOCK
            physical = (descriptor & ADDRESS & ~(granule - 1)) | (page & (granule - 1))
            assert physical == page, 'table storage is not identity mapped'
            if not any(begin <= page and page + 4096 <= end + 1 for begin, end, permissions in u.mem_regions()):
                u.mem_map(page, 4096)
                self.sparse_pages.add(page)
        return True

    def skip(self, address):
        self.u.reg_write(UC_ARM64_REG_PC, address + 4)

    def code(self, u, address, size, unused):
        if self.active is not None and address == self.active['return']:
            assert not self.table_dirty, 'conversion returns before descriptor cache cleaning completes'
            self.active['status'] = signed(u.reg_read(UC_ARM64_REG_X0))
            self.active['after'] = self.entries(self.active['chunk'])
            self.conversions.append(self.active)
            self.active = None
            if address != RETURN:
                return
        if address == RETURN:
            u.emu_stop()
            return
        if address == self.critical_enter:
            self.lock_depth += 1
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
            return
        if address == self.critical_exit:
            assert self.lock_depth > 0, 'critical-section exit without ownership'
            self.lock_depth -= 1
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
            return
        if address == self.entry:
            assert self.active is None, 'nested cache-policy conversion'
            chunk = u.reg_read(UC_ARM64_REG_X0 + 1)
            self.active = dict(chunk=chunk, policy=u.reg_read(UC_ARM64_REG_X0 + 2),
                               before=self.entries(chunk), cleaned=set(), cache_complete=False,
                               events=[], table_cleans=[], return_address=u.reg_read(UC_ARM64_REG_LR))
            self.active['return'] = self.active.pop('return_address')
            if FLOOR <= chunk < 0x340000000:
                for root in self.roots:
                    self.translate(root, chunk)
            at = self.active['return']
            if at not in self.return_hooks:
                harness.hook_addresses(u, [at], self.code)
                self.return_hooks.add(at)
            return
        if address not in self.instructions:
            self.instructions[address] = next(self.decoder.disasm(bytes(u.mem_read(address, 4)), address))
        op = self.instructions[address]
        args = op.op_str.lower().split(', ') if op.op_str else []
        if op.mnemonic == 'mrs':
            values = {'currentel': 4, 'sctlr_el1': 5, 'ctr_el0': (4 << 24) | (4 << 16) if self.cache_stride else 0,
                      'id_aa64mmfr0_el1': 0x101122, 'tcr_el1': self.image.tcr}
            assert args[1] in values, ('unmodeled system register', op.op_str)
            u.reg_write(reg(args[0]), values[args[1]])
            self.skip(address)
        elif op.mnemonic == 'dc':
            va = u.reg_read(reg(args[1]))
            if args[0] == 'cvac':
                assert self.uncached_walk, 'cached table walks unexpectedly clean a descriptor line'
                line = va & ~63
                assert line in self.table_dirty, 'descriptor clean does not follow a table store'
                self.operations['table_cleans'] += 1
                if self.active is not None:
                    assert line in {entry & ~63 for entry, old in self.active['before']}, \
                        'conversion cleans a line outside its descriptors'
                    self.active['table_cleans'].append(va)
                if self.variant != 'omit-table-clean' or self.active is None:
                    self.table_clean_pending.add(line)
                self.skip(address)
                return
            if args[0] == 'civac':
                self.operations['civac'] += 1
            if self.active is not None:
                assert args[0] == 'civac', 'conversion must clean and invalidate, not discard dirty lines'
                state = self.active
                assert not state['events'], 'cache maintenance follows a descriptor edit or TLB operation'
                assert state['chunk'] <= va < state['chunk'] + BLOCK and not va & 63, 'cache operation misses its chunk'
                assert self.translate(self.root, va), 'cache maintenance uses an invalid translation'
                if self.variant != 'omit-cache':
                    state['cleaned'].add(va)
            self.skip(address)
        elif op.mnemonic == 'tlbi':
            assert not self.table_dirty, 'TLBI precedes completed descriptor cache cleaning'
            self.operations['tlbi'] += 1
            if self.active is not None:
                state = self.active
                assert args[0] == 'vaae1is', 'TLBI must broadcast the address for every ASID'
                assert u.reg_read(reg(args[1])) == state['chunk'] >> 12, 'TLBI address differs from the converted chunk'
                state['events'].append('tlbi')
                if self.variant != 'omit-tlbi':
                    self.tlb = {key: value for key, value in self.tlb.items() if key[1] != state['chunk']}
                self.speculative_read()
            self.skip(address)
        elif op.mnemonic in ('dsb', 'isb', 'dmb'):
            if op.mnemonic == 'dsb' and self.uncached_walk:
                assert self.table_dirty <= self.table_clean_pending, 'descriptor stores reach a barrier without cache cleaning'
                self.table_dirty.clear()
                self.table_clean_pending.clear()
            if self.active is not None:
                state = self.active
                if op.mnemonic == 'dsb' and args[0] == 'sy' and not state['events']:
                    if len(state['cleaned']) == BLOCK // 64:
                        state['cache_complete'] = True
                else:
                    state['events'].append(op.mnemonic + (':' + args[0] if args else ''))
                    self.speculative_read()
            self.skip(address)
        else:
            raise AssertionError(('unmodeled system instruction', op.mnemonic, op.op_str))

    def call(self, name, *args):
        for index, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + index, value)
        self.u.reg_write(UC_ARM64_REG_SP, STACK)
        self.u.reg_write(UC_ARM64_REG_LR, RETURN)
        self.u.emu_start(self.image.address(name), RETURN + 4, count=3000000)
        assert self.u.reg_read(UC_ARM64_REG_PC) == RETURN, (name, 'instruction budget expired')
        assert self.lock_depth == 0, (name, 'ledger lock retained')
        return self.u.reg_read(UC_ARM64_REG_X0)

    def bootstrap(self):
        pointer = self.call('os_owner_memory_below', 4096, 4096, 0, 1 << 32)
        assert FLOOR <= pointer < FLOOR + BLOCK, 'ledger does not allocate its initial WB extent'
        assert not self.conversions, 'a WB request unexpectedly converts a chunk'
        return pointer

    def request(self, base, policy, owner=7, size=4096):
        request, result = SCRATCH, SCRATCH + 256
        self.u.mem_write(request, bytes(128))
        fields = {'bytes': (size, 8), 'align': (4096, 8), 'phys_max': (1 << 32, 8),
                  'base': (base, 8), 'owner': (owner, 2), 'flags': (9, 2), 'policy': (policy, 1)}
        for name, (value, width) in fields.items():
            self.u.mem_write(request + self.field('umem_request', name), value.to_bytes(width, 'little'))
        return signed(self.call('umem_alloc', self.ledger, request, result)), result

    def add_private(self):
        # This fixture starts with an already-published application root and
        # L2 copy; the application-builder publication has its own cases.
        root, l2, table = 0x47000000, 0x47001000, 0x47002000
        self.u.mem_write(root, bytes(self.u.mem_read(self.root, 4096)))
        self.u.mem_write(l2, bytes(self.u.mem_read(self.slot1, 4096)))
        self.write_words(root + 8, [l2 | 3])
        self.u.mem_write(table, bytes(24))
        self.write_words(table + self.field('umem_policy_table', 'l2'), [l2])
        self.write_words(table + self.field('umem_policy_table', 'base'), [GIB])
        self.call('umem_policy_add_table', self.policy, table)
        self.roots.append(root)
        self.table_regions += [(root, root + 4096), (l2, l2 + 4096)]

    def policy_byte(self, chunk):
        pointer = self.read(self.ledger + self.field('umem', 'chunk_policy'))
        base = self.read(self.ledger + self.field('umem', 'chunk_base'))
        return self.read(pointer + (chunk - base) // BLOCK, 1)

    def describe(self, pointer):
        handle, extent = SCRATCH + 768, SCRATCH + 1024
        assert self.call('umem_lookup', self.ledger, pointer, handle) == 0
        assert self.call('umem_describe', self.ledger, handle, extent, 0) == 0
        return {name: self.read(extent + self.field('umem_extent', name), width)
                for name, width in (('base', 8), ('bytes', 8), ('owner', 2), ('policy', 1))}

    def owner(self, pointer):
        return self.describe(pointer)['owner']

    def manifest(self, record):
        application = self.image.address('ss_application')
        pointer = self.read(application + self.field('ss_application', 'manifest'))
        head, region_size = self.image.sizes['ss_manifest'], self.image.sizes['ss_region']
        at = 0x47010000
        self.u.mem_write(at, bytes(self.u.mem_read(pointer, head)) + bytes(len(record['regions']) * region_size))
        self.u.mem_write(at + self.field('ss_manifest', 'name'), record['name'].encode().ljust(16, b'\0'))
        self.u.mem_write(at + self.field('ss_manifest', 'region_count'), len(record['regions']).to_bytes(1, 'little'))
        for index, region in enumerate(record['regions']):
            address = at + self.field('ss_manifest', 'regions') + index * region_size
            access = sum({'read': 1, 'write': 2, 'scratch': 16}[value] for value in region['access'])
            fields = {'address': (int(region['address'], 0), 8), 'bytes': (region['bytes'], 8),
                      'kind': ({'input': 1, 'work': 2, 'output': 3}[region['kind']], 4), 'access': (access, 4)}
            for name, (value, width) in fields.items():
                self.u.mem_write(address + self.field('ss_region', name), value.to_bytes(width, 'little'))
        self.write_words(application + self.field('ss_application', 'manifest'), [at])


def signed(value):
    value &= 0xffffffff
    return value - (1 << 32) if value & (1 << 31) else value


def check_conversion(model, expected_policy, count=1):
    assert len(model.conversions) == count
    state = model.conversions[-1]
    assert state['status'] == 0 and state['policy'] == expected_policy
    assert len(state['cleaned']) == BLOCK // 64 and state['cache_complete']
    entries = len(state['before'])
    expected = ['break'] * entries + ['dsb:ish', 'tlbi', 'dsb:ish', 'isb'] + ['make'] * entries + ['dsb:ish', 'isb']
    assert state['events'] == expected, (state['events'], expected)
    expected_cleans = 2 * entries if model.uncached_walk else 0
    assert len(state['table_cleans']) == expected_cleans and not model.table_dirty
    want = (2 if expected_policy else 1) << 2
    for (address, old), (after_address, new) in zip(state['before'], state['after']):
        assert address == after_address and new & ATTR == want
        assert old & ~ATTR == new & ~ATTR, 'conversion changes physical output or permissions'
    assert model.policy_byte(state['chunk']) == expected_policy
    tables = model.read(model.policy + model.field('umem_el1_map', 'split_pages'))
    extent = model.describe(tables)
    assert extent == dict(base=tables, bytes=model.split_bytes, owner=0xffff, policy=0), \
        'split tables are not backed by one kernel-owned WB ledger extent'
    return dict(chunk=hex(state['chunk']), table_entries=entries, civac_lines=len(state['cleaned']),
                order=state['events'], split_storage=hex(tables), split_bytes=model.split_bytes,
                sparse_metadata_pages=len(model.sparse_pages), descriptor_cleans=len(state['table_cleans']),
                cached_table_walks=not model.uncached_walk)


def run_case(name, image):
    model = Model(image, name if name.startswith('omit-') else 'normal')
    if name == 'owner-nc-allocation':
        pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
        assert pointer == FLOOR, 'table metadata consumes the first aligned free chunk'
        result = check_conversion(model, 1)
        result['first_aligned_free_chunk_preserved'] = True
        return result
    if name == 'private-l2-policy':
        model.bootstrap()
        model.add_private()
        model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
        result = check_conversion(model, 1)
        assert result['table_entries'] == 2
        return result
    if name == 'chunk-in-use':
        live = model.bootstrap()
        before = bytes(model.u.mem_read(model.slot1, 4096))
        operations = model.operations.copy()
        chunk, free_page = live & ~(BLOCK - 1), live + 4096
        assert free_page & ~(BLOCK - 1) == chunk
        result, _ = model.request(free_page, 1)
        assert result == -2 and not model.conversions and model.operations == operations
        assert bytes(model.u.mem_read(model.slot1, 4096)) == before
        assert model.policy_byte(chunk) == 0 and model.owner(live) == 0xffff
        return dict(status=result, cache_and_table_operations=0)
    if name == 'maintenance-refusal':
        model.bootstrap()
        model.cache_stride = False
        free_before = model.read(model.ledger + model.field('umem', 'free_bytes'))
        operations = model.operations.copy()
        pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
        assert pointer == 0 and len(model.conversions) == 1 and model.operations == operations
        state = model.conversions[0]
        assert state['status'] == -1 and state['before'] == state['after'] and not state['cleaned'] and not state['events']
        assert model.policy_byte(state['chunk']) == 0
        assert model.read(model.ledger + model.field('umem', 'free_bytes')) == free_before
        return dict(status=state['status'], unchanged_policy=0, unchanged_descriptors=True)
    if name == 'nc-to-wb':
        pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
        check_conversion(model, 1)
        handle = SCRATCH + 512
        assert model.call('umem_lookup', model.ledger, pointer, handle) == 0
        assert model.call('umem_free', model.ledger, handle) == 0
        status, _ = model.request(pointer, 0)
        assert status == 0
        return check_conversion(model, 0, 2)
    if name in ('omit-cache', 'omit-tlbi', 'omit-table-clean'):
        if name == 'omit-table-clean':
            assert model.uncached_walk, 'table-clean omission requires an uncached-walk image'
        model.bootstrap()
        reason = {'omit-cache': 'cache maintenance incomplete before break',
                  'omit-tlbi': 'cached translation survives make',
                  'omit-table-clean': 'descriptor stores reach a barrier without cache cleaning'}[name]
        return harness.refused(lambda: model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32), reason, AssertionError)
    if name == 'out-of-map':
        model.bootstrap()
        operations = model.operations.copy()
        for chunk in (0x340000000, 0x10000000000):
            assert signed(model.call('umem_policy_convert', model.policy, chunk, 1)) == -1
        assert all(not state['events'] and not state['cleaned'] for state in model.conversions)
        assert model.operations == operations
        return dict(refused_chunks=2, cache_and_table_operations=0)
    if name == 'coalesced-root-refusal':
        model.bootstrap()
        root = 0x47000000
        model.u.mem_write(root, bytes(model.u.mem_read(model.root, 4096)))
        model.write_words(root + 2 * 8, [2 * GIB | KERNEL_FLAGS | (1 << 6)])
        model.roots.append(root)
        space = image.address('space', unit='apps/os/large.c')
        model.write_words(space + model.field('app_space', 'root'), [root])
        admitted, width = image.admitted_storage
        model.u.mem_write(admitted, (1).to_bytes(width, 'little'))
        operations = model.operations.copy()
        free_before = model.read(model.ledger + model.field('umem', 'free_bytes'))
        result, _ = model.request(2 * GIB, 1)
        assert result == -6 and not model.conversions and model.operations == operations
        assert model.policy_byte(2 * GIB) == 0
        assert model.read(model.ledger + model.field('umem', 'free_bytes')) == free_before
        assert model.read(root + 2 * 8) == 2 * GIB | KERNEL_FLAGS | (1 << 6)
        return dict(status=result, cache_and_table_operations=0, unchanged_policy=0)
    assert name == 'pinned-reservations'
    record = json.loads((Path(__file__).resolve().parents[1] / 'apps/slot1-test/manifest-preserve.json').read_text())
    model.manifest(record)
    samples = []
    for index, region in enumerate(record['regions']):
        base, size = int(region['address'], 0), region['bytes']
        for address in (base, base + size - 64):
            value = bytes([0xa1 + index]) * 64
            model.u.mem_write(address, value)
            samples.append((address, value))
    pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
    assert pointer, 'early device allocation is refused despite free memory'
    result = check_conversion(model, 1)
    tables = model.read(model.policy + model.field('umem_el1_map', 'split_pages'))
    reserved = []
    for region in record['regions']:
        base, size = int(region['address'], 0), region['bytes']
        extent = model.describe(base)
        assert extent['owner'] == 1, 'early table/device allocation takes a manifest-pinned extent'
        assert extent['base'] == base and extent['bytes'] == size, 'pinned extent geometry changes'
        assert not (base < tables + model.split_bytes and tables < base + size), 'table pages overlap a pinned extent'
        assert not (base < pointer + BLOCK and pointer < base + size), 'device chunk overlaps a pinned extent'
        assert all(model.policy_byte(chunk) == 0 for chunk in range(base, base + size, BLOCK))
        reserved.append(dict(base=hex(base), bytes=size, owner=1))
    assert all(bytes(model.u.mem_read(address, 64)) == value for address, value in samples), 'pinned data changes during owner setup'
    return dict(**result, reserved=reserved, table_pages=hex(tables))


def structure_sizes(path, names):
    sizes = {}
    with path.open('rb') as stream:
        dwarf = ELFFile(stream).get_dwarf_info()
        for unit in dwarf.iter_CUs():
            for die in unit.iter_DIEs():
                name, size = die.attributes.get('DW_AT_name'), die.attributes.get('DW_AT_byte_size')
                if die.tag != 'DW_TAG_structure_type' or name is None or size is None:
                    continue
                name = name.value.decode()
                if name in names:
                    assert name not in sizes or sizes[name] == int(size.value), name
                    sizes[name] = int(size.value)
    assert sizes.keys() == set(names), (names, sizes)
    return sizes


def variable_storage(path, unit_suffix, name):
    """Resolve an optimized scalar's storage, including a one-byte Boolean."""
    found = set()
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        dwarf = elf.get_dwarf_info()
        parser = DWARFExprParser(dwarf.structs)
        for unit in dwarf.iter_CUs():
            if not unit.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(unit_suffix):
                continue
            for die in unit.iter_DIEs():
                label, location = die.attributes.get('DW_AT_name'), die.attributes.get('DW_AT_location')
                if die.tag != 'DW_TAG_variable' or label is None or label.value.decode() != name or location is None:
                    continue
                assert location.form == 'DW_FORM_exprloc'
                ops = parser.parse_expr(location.value)
                assert ops and ops[0].op_name in ('DW_OP_addr', 'DW_OP_addrx')
                address = ops[0].args[0] if ops[0].op_name == 'DW_OP_addr' else dwarf.get_addr(unit, ops[0].args[0])
                objects = [symbol for symbol in elf.get_section_by_name('.symtab').iter_symbols()
                           if symbol.name == name and symbol['st_value'] == address and symbol['st_info']['type'] == 'STT_OBJECT']
                assert len(objects) == 1 and 1 <= objects[0]['st_size'] <= 8
                found.add((address, int(objects[0]['st_size'])))
    assert len(found) == 1, (unit_suffix, name, found)
    return found.pop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=CASES, action='append', help='run only named cases after a fixture or source correction')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image_sha256 = hashlib.sha256(args.image.read_bytes()).hexdigest()
    image = harness.Image(args.image, layout_names=('umem', 'umem_request', 'umem_buf', 'umem_extent',
        'umem_el1_map', 'umem_policy_table', 'ss_manifest', 'ss_region', 'ss_application', 'app_space'))
    image.tcr = a733_tcr.for_image(args.image)
    image.sizes = structure_sizes(args.image, ('ss_manifest', 'ss_region', 'umem_policy_table'))
    image.admitted_storage = variable_storage(args.image, 'apps/os/large.c', 'admitted')
    selected = args.case or tuple(name for name in CASES if name != 'omit-table-clean' or not image.tcr & 0xf00)
    cases = {name: run_case(name, image) for name in selected}
    assert hashlib.sha256(args.image.read_bytes()).hexdigest() == image_sha256, 'image changed during the check'
    harness.save(args.output / 'result.json', dict(passed=True, physical_access=False, image=str(args.image),
        image_sha256=image_sha256, cases=cases,
        scope='Linked OS ledger, policy conversion and EL1 maintenance instructions. Cache completion, address-space TLBs and critical sections are modeled; physical coherence is not qualified.'))
    print(f'PASS ledger NC: {len(cases)} native instruction-model cases')


if __name__ == '__main__':
    main()
