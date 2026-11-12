#!/usr/bin/env python3
"""Execute ledger cache-policy conversion and its EL1 maintenance in the linked OS image.

The model follows the free-chunk and break-before-make contract in
docs/designs/unified-memory.md. It models architectural registers, cache-line
completion and per-address-space TLBs; it does not qualify physical coherence.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

GATE_RUNS = [['--image', '{elf:sup}', '--output', '{output}']]
CHECK_INPUTS = ['apps/os/large.c', 'apps/os/owner-memory.h', 'kernel/umem/umem.c',
                'kernel/umem/umem_policy.c', 'arch/aarch64/umem_cpu.c',
                'arch/aarch64/device_memory.c', 'include/umem.h', 'include/umem_policy.h',
                'include/os_memory_tables.h', 'docs/designs/unified-memory.md']

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

    def skip(self, address):
        self.u.reg_write(UC_ARM64_REG_PC, address + 4)

    def code(self, u, address, size, unused):
        if self.active is not None and address == self.active['return']:
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
                               events=[], return_address=u.reg_read(UC_ARM64_REG_LR))
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
            values = {'currentel': 4, 'sctlr_el1': 5, 'ctr_el0': (4 << 24) | (4 << 16) if self.cache_stride else 0}
            assert args[1] in values, ('unmodeled system register', op.op_str)
            u.reg_write(reg(args[0]), values[args[1]])
            self.skip(address)
        elif op.mnemonic == 'dc':
            if self.active is not None:
                assert args[0] == 'civac', 'conversion must clean and invalidate, not discard dirty lines'
                va = u.reg_read(reg(args[1]))
                state = self.active
                assert not state['events'], 'cache maintenance follows a descriptor edit or TLB operation'
                assert state['chunk'] <= va < state['chunk'] + BLOCK and not va & 63, 'cache operation misses its chunk'
                assert self.translate(self.root, va), 'cache maintenance uses an invalid translation'
                if self.variant != 'omit-cache':
                    state['cleaned'].add(va)
            self.skip(address)
        elif op.mnemonic == 'tlbi':
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
        root, l2, table = 0x47000000, 0x47001000, 0x47002000
        self.u.mem_write(root, bytes(self.u.mem_read(self.root, 4096)))
        self.u.mem_write(l2, bytes(self.u.mem_read(self.slot1, 4096)))
        self.write_words(root + 8, [l2 | 3])
        self.u.mem_write(table, bytes(24))
        self.write_words(table + self.field('umem_policy_table', 'l2'), [l2])
        self.write_words(table + self.field('umem_policy_table', 'base'), [GIB])
        self.call('umem_policy_add_table', self.policy, table)
        self.roots.append(root)

    def policy_byte(self, chunk):
        pointer = self.read(self.ledger + self.field('umem', 'chunk_policy'))
        base = self.read(self.ledger + self.field('umem', 'chunk_base'))
        return self.read(pointer + (chunk - base) // BLOCK, 1)

    def owner(self, pointer):
        handle, extent = SCRATCH + 768, SCRATCH + 1024
        assert self.call('umem_lookup', self.ledger, pointer, handle) == 0
        assert self.call('umem_describe', self.ledger, handle, extent, 0) == 0
        return self.read(extent + self.field('umem_extent', 'owner'), 2)


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
    want = (2 if expected_policy else 1) << 2
    for (address, old), (after_address, new) in zip(state['before'], state['after']):
        assert address == after_address and new & ATTR == want
        assert old & ~ATTR == new & ~ATTR, 'conversion changes physical output or permissions'
    assert model.policy_byte(state['chunk']) == expected_policy
    return dict(chunk=hex(state['chunk']), table_entries=entries, civac_lines=len(state['cleaned']),
                order=state['events'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image_sha256 = hashlib.sha256(args.image.read_bytes()).hexdigest()
    image = harness.Image(args.image, layout_names=('umem', 'umem_request', 'umem_buf', 'umem_extent', 'umem_el1_map', 'umem_policy_table'))
    cases = {}

    model = Model(image)
    pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
    assert pointer and not pointer & (BLOCK - 1)
    cases['owner-nc-allocation'] = check_conversion(model, 1)

    model = Model(image)
    model.bootstrap()
    model.add_private()
    pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
    cases['private-l2-policy'] = check_conversion(model, 1)
    assert cases['private-l2-policy']['table_entries'] == 2

    model = Model(image)
    live = model.bootstrap()
    before = bytes(model.u.mem_read(model.slot1, 4096))
    result, _ = model.request(FLOOR + 0x10000, 1)
    assert result == -2 and not model.conversions
    assert bytes(model.u.mem_read(model.slot1, 4096)) == before
    assert model.policy_byte(FLOOR) == 0 and model.owner(live) == 0xffff
    cases['chunk-in-use'] = dict(status=result, cache_and_table_operations=0)

    model = Model(image)
    model.bootstrap()
    model.cache_stride = False
    free_before = model.read(model.ledger + model.field('umem', 'free_bytes'))
    pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
    assert pointer == 0 and len(model.conversions) == 1
    state = model.conversions[0]
    assert state['status'] == -1 and state['before'] == state['after'] and not state['cleaned'] and not state['events']
    assert model.policy_byte(state['chunk']) == 0
    assert model.read(model.ledger + model.field('umem', 'free_bytes')) == free_before
    cases['maintenance-refusal'] = dict(status=state['status'], unchanged_policy=0, unchanged_descriptors=True)

    model = Model(image)
    pointer = model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32)
    check_conversion(model, 1)
    handle = SCRATCH + 512
    assert model.call('umem_lookup', model.ledger, pointer, handle) == 0
    assert model.call('umem_free', model.ledger, handle) == 0
    status, _ = model.request(pointer, 0)
    assert status == 0
    cases['nc-to-wb'] = check_conversion(model, 0, 2)

    for variant, reason in [('omit-cache', 'cache maintenance incomplete before break'),
                            ('omit-tlbi', 'cached translation survives make')]:
        model = Model(image, variant)
        model.bootstrap()
        cases[variant] = harness.refused(
            lambda: model.call('os_owner_memory_below', 4096, BLOCK, 1, 1 << 32), reason, AssertionError)

    model = Model(image)
    model.bootstrap()
    for chunk in (0x340000000, 0x10000000000):
        assert signed(model.call('umem_policy_convert', model.policy, chunk, 1)) == -1
    assert all(not state['events'] and not state['cleaned'] for state in model.conversions)
    cases['out-of-map'] = dict(refused_chunks=2, cache_and_table_operations=0)

    assert hashlib.sha256(args.image.read_bytes()).hexdigest() == image_sha256, 'image changed during the check'
    harness.save(args.output / 'result.json', dict(passed=True, physical_access=False, image=str(args.image),
        image_sha256=image_sha256, cases=cases,
        scope='Linked OS ledger, policy conversion and EL1 maintenance instructions. Cache completion, address-space TLBs and critical sections are modeled; physical coherence is not qualified.'))
    print(f'PASS ledger NC: {len(cases)} native instruction-model cases, including cache/TLBI negative controls')


if __name__ == '__main__':
    main()
