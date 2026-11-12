#!/usr/bin/env python3
"""Check the cache order and the page-table protection of the OS image's sealed session.

The OS image keeps the session copy and the EL1 copy of the lifetime in `.sealed`, a NOLOAD section last in the CODE
block (apps/os/sealed.c, apps/os/sealed-link.patch; docs/designs/one-os-image.md, "Lifetime word and sealed session").
Only EL3 writes it, with translation and caches off. The check executes the linked image under the shared Unicorn
machine with modeled system registers, counter and cache maintenance, and requires:

  cache order   board_session_prepare at EL3 issues DC CIVAC on every 64-byte line of the section before its first
                store to the section and again after its last store; the EL1 entry path of every core
                (a733_cached_enter_el1, the one caller of a733_cpu_coherence_enable_el1) issues DC CIVAC on every line
                of the section before it enables translation; the EL1 readers store nothing to the section; and only
                sealed.c and that entry path materialize an address inside the section (ADRP, ADR or LDR literal),
                except the lifetime copy, which any code may read (the EL1 vector and the breadcrumb check), and a
                manifest end label equal to the section start when its use never dereferences that address.
  page tables   after a733_cached_memory_prepare, every EL1 descriptor at any level whose output range overlaps the
                section is invalid or read-only (AP[2], or APTable[1] on a table above it). The walk covers the whole
                level-1 table of the EL1 regime and every table below it: the level-2 blocks of 0x40000000 to
                0x80000000 (slot 14, the GPU blocks, the DRAM blocks of dram_map), the level-1 DRAM blocks and any USB
                alias level-3 table. Application spaces use the linked app_space_build_tables with copies of the same
                prepared boot pages that the OS getters read. They are walked the same way: one whose owner holds every usable
                range (with and without 1 GiB blocks), and one whose ledger claims the CODE block, which the builder
                must refuse or leave read-only.

The model has no cache: it checks the order of the maintenance operations and stores, not their effect. The mutations
are applied to the linked image in the model and each must fail the check:
  cache order   the first flush in board_session_prepare removed; the second replaced with a733_cpu_coherence_publish
                (DC CVAC); the flush on the EL1 entry path removed.
  page tables   the section placed in .bss (the walk of a range at __bss_start of the same size); AP[2] cleared on the
                CODE block.
Device MMU mappings (GPU, NPU, IOMMU) are outside this check.
Usage: os-sealed-session.py --image ELF [--image ELF ...] --output DIR
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64_const import ARM64_OP_REG
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402  Refuses to run with assertions disabled.

GATE_RUNS = [['--image', '{elf:sup}', '--image', '{elf:ret-npu-load}', '--output', '{output}']]
CHECK_INPUTS = ['apps/os/session.c', 'apps/os/return-terminal.patch']
GPR = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_LR]
SENTINEL = 0x47fff000
STACK = 0x47f00000
SCRATCH = 0x47800000          # model-only buffers: base tables, ledger, space, arena
SESSION = 0x41f00000          # A733_BOARD_SESSION_ADDRESS
CONTRACT = 0x43200000
MOV_W0_1 = 0x52800020
LINE = 64
KERNEL_BLOCK_BITS = 1 | 4 | (3 << 8) | (1 << 10) | (1 << 11) | (1 << 53) | (1 << 54)   # apps/os/large.c KERNEL_BLOCK
APP_REGION_FLOOR, APP_REGION_BASE, APP_REGION_LIMIT = 0x48000000, 0x80000000, 0x340000000
LAYOUTS = {'a733_smp_contract', 'a733_board_session', 'app_space', 'app_table_arena', 'app_region_ledger',
           'app_region_entry'}


class Image(harness.Image):
    def __init__(self, path):
        super().__init__(path, layout_names=LAYOUTS)
        self.path = Path(path)
        with self.path.open('rb') as f:
            elf = ELFFile(f)
            self.sizes = {s.name: int(s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()
                          if s['st_info']['type'] == 'STT_FUNC'}
            self.function_ranges = sorted((int(s['st_value']), int(s['st_size']), s.name)
                                          for s in elf.get_section_by_name('.symtab').iter_symbols()
                                          if s['st_info']['type'] == 'STT_FUNC' and s['st_size'])
            self.code = [(int(p['p_vaddr']), p.data()) for p in elf.iter_segments()
                         if p['p_type'] == 'PT_LOAD' and p['p_flags'] & 1]
        self.sealed = (self.address('__os_sealed_start'), self.address('__os_sealed_end'))

    def function_of(self, address):
        for base, size, name in self.function_ranges:
            if base <= address < base + size:
                return name
        return None

    def words(self, name):
        base = self.address(name)
        for start, data in self.code:
            if start <= base < start + len(data):
                offset = base - start
                return [(base + i, struct.unpack_from('<I', data, offset + i)[0]) for i in range(0, self.sizes[name], 4)]
        raise AssertionError(name)

    def calls(self, name, target):
        """Addresses of the BL instructions in function name whose target is function target."""
        goal = self.address(target)
        return [a for a, w in self.words(name) if w >> 26 == 0x25 and a + (((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4 == goal]

    def branches(self, target):
        """Addresses of every B and BL instruction in executable segments whose target is function target."""
        goal = self.address(target)
        return [base + i for base, data in self.code for i in range(0, len(data) & ~3, 4)
                for w in struct.unpack_from('<I', data, i) if w >> 26 in (0x05, 0x25) and
                base + i + (((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4 == goal]

    def reference_target(self, address, kind):
        """The byte that the ADR at address names, or that the ADRP at address names together with the first ADD or 32-bit
        LDR (unsigned offset) on its register among the next four instructions; None when there is no such instruction."""
        for base, data in self.code:
            if base <= address < base + len(data) - 4:
                w = struct.unpack_from('<I', data, address - base)[0]
                imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
                imm -= (imm & (1 << 20)) << 1
                if kind == 'adr':
                    return address + imm
                page, rd = (address & ~0xfff) + (imm << 12), w & 31
                for i in range(1, 5):
                    if address - base + 4 * i + 4 > len(data):
                        return None
                    x = struct.unpack_from('<I', data, address - base + 4 * i)[0]
                    if (x >> 5) & 31 != rd:
                        continue
                    if x & 0xffc00000 == 0xb9400000:
                        return page + ((x >> 10) & 0xfff) * 4
                    if x & 0xffc00000 == 0x91000000:
                        return page + ((x >> 10) & 0xfff)
                return None
        return None

    def references(self, low, high):
        """(address, kind) of each ADRP, ADR and LDR literal in executable segments whose target is in [low, high)."""
        found = []
        for base, data in self.code:
            for i in range(0, len(data) & ~3, 4):
                w = struct.unpack_from('<I', data, i)[0]
                pc = base + i
                if w & 0x1f000000 == 0x10000000:
                    imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
                    imm -= (imm & (1 << 20)) << 1
                    target = ((pc & ~0xfff) + (imm << 12)) if w >> 31 else pc + imm
                    kind = 'adrp' if w >> 31 else 'adr'
                elif w & 0x3b000000 == 0x18000000:
                    imm = (w >> 5) & 0x7ffff
                    imm -= (imm & (1 << 18)) << 1
                    target, kind = pc + imm * 4, 'ldr'
                else:
                    continue
                if (low & ~0xfff if kind == 'adrp' else low) <= target < high:
                    found.append((pc, kind))
        return found


def bl(source, target):
    return 0x94000000 | (((target - source) >> 2) & 0x3ffffff)


class Machine:
    """The linked image with modeled system registers, counter and cache maintenance, and a record of stores to .sealed."""

    def __init__(self, image, patches=()):
        self.image = image
        self.uc = harness.machine()
        self.uc.mem_map(0x40000000, 0x08000000)
        for address, data in image.parts:
            self.uc.mem_write(address, data)
        for address, word in patches:
            self.uc.mem_write(address, struct.pack('<I', word))
        self.model = {'currentel': 12, 'daif': 0x3c0, 'sctlr_el3': 0x30c50830, 'sctlr_el1': 0x30d00800,
                      'mpidr_el1': 0x81000000, 'ctr_el0': 4 << 16, 'cntfrq_el0': 24000000,
                      'id_aa64mmfr0_el1': 0, 'tcr_el1': 0}
        self.clock = 24
        self.el = 3
        self.trace = []
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.decoded = {}
        sites = harness.instruction_sites(image.parts, harness.system_register_access)
        harness.hook_addresses(self.uc, sites, self.system)
        self.stubs = {}
        self.uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, self.upper_dram)
        low, high = image.sealed
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.wrote, begin=low, end=high - 1)

    def stub(self, name, value):
        address = self.image.address(name)
        self.stubs[address] = value
        harness.hook_addresses(self.uc, [address], self.returned)

    def returned(self, uc, address, size, unused):
        uc.reg_write(UC_ARM64_REG_X0, self.stubs[address])
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def upper_dram(self, uc, access, address, size, value, unused):
        # The address-test decode check (cmake/a733-smp-cached.cmake) writes and reads tokens across the upper DRAM.
        if not 0x48000000 <= address < APP_REGION_LIMIT:
            return False
        uc.mem_map(address & ~0xfff, 0x1000)
        return True

    def gpr(self, name):
        return 0 if name in ('xzr', 'wzr') else self.uc.reg_read(GPR[int(name[1:])])

    def wrote(self, uc, access, address, size, value, unused):
        self.trace.append(('write', address, size, self.el))

    def system(self, uc, address, size, unused):
        if address not in self.decoded:
            self.decoded[address] = next(self.decoder.disasm(bytes(uc.mem_read(address, 4)), address))
        ins = self.decoded[address]
        args = ins.op_str.split(', ')
        if ins.mnemonic == 'mrs':
            if args[1] == 'cntpct_el0':
                value = self.clock
                self.clock += 24
            else:
                assert args[1] in self.model, (hex(address), ins.op_str)
                value = self.model[args[1]]
            uc.reg_write(GPR[int(args[0][1:])], value)
        elif ins.mnemonic == 'msr' and not args[0].startswith('daif'):
            self.model[args[0]] = self.gpr(args[1])
        elif ins.mnemonic in ('dc', 'ic', 'tlbi'):
            self.trace.append((ins.mnemonic + ' ' + args[0], self.gpr(args[1]) if len(args) > 1 else 0, 0, self.el))
        else:
            return
        uc.reg_write(UC_ARM64_REG_PC, address + 4)

    def call(self, name, *args, start=None, until=SENTINEL):
        for i, v in enumerate(args):
            self.uc.reg_write(GPR[i], v)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK)
        self.uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        self.uc.emu_start(start if start is not None else self.image.address(name), until, count=20_000_000)
        assert self.uc.reg_read(UC_ARM64_REG_PC) == until, (name, hex(self.uc.reg_read(UC_ARM64_REG_PC)))
        return self.uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff

    def at_el1(self):
        self.el = 1
        self.model.update(currentel=4, sctlr_el1=0x30d00800)

    def read(self, address):
        return int.from_bytes(self.uc.mem_read(address, 8), 'little')


# ---- cache order ----

def lines(low, high):
    return list(range(low & ~(LINE - 1), high, LINE))


def order_faults(trace, low, high):
    """The faults in the EL3 order: lines without DC CIVAC before the first or after the last EL3 store to the section."""
    stores = [i for i, t in enumerate(trace) if t[0] == 'write' and t[3] == 3]
    if not stores:
        return ['no EL3 store to the section']
    faults = []
    for line in lines(low, high):
        flushes = [i for i, t in enumerate(trace) if t[0] == 'dc civac' and t[1] & ~(LINE - 1) == line and t[3] == 3]
        if not any(i < stores[0] for i in flushes):
            faults.append('line %#x: no DC CIVAC before the first store' % line)
        if not any(i > stores[-1] for i in flushes):
            faults.append('line %#x: no DC CIVAC after the last store' % line)
    return faults


def prepare_session(image, patches=(), lifetime=2):
    """Run board_session_prepare at EL3 on a contract that names a 4,096-byte session; return the machine and result."""
    m = Machine(image, patches)
    session = bytearray((i * 7 + 3) & 0xff for i in range(4096))
    s = image.layouts['a733_board_session']
    c = image.layouts['a733_smp_contract']
    identity = bytes(m.uc.mem_read(image.address('a733_smp_build_id'), 32))
    struct.pack_into('<3Q', session, s['epoch'], 1, 2, 3)
    assert (s['generation'], s['lease']) == (s['epoch'] + 8, s['epoch'] + 16), s
    session[s['image']:s['image'] + 32] = identity
    contract = bytearray(c['reserved'] + 48)
    for field, value in (('epoch', 1), ('generation', 2), ('lease', 3), ('startup_deadline', 10 ** 9)):
        struct.pack_into('<Q', contract, c[field], value)
    contract[c['build_id']:c['build_id'] + 32] = identity
    struct.pack_into('<2Q', contract, c['reserved'], SESSION, len(session))
    contract[c['reserved'] + 16:c['reserved'] + 48] = hashlib.sha256(session).digest()
    m.uc.mem_write(SESSION, bytes(session))
    m.uc.mem_write(CONTRACT, bytes(contract))
    m.uc.mem_write(image.address('a733_smp_lifetime'), struct.pack('<I', lifetime))
    # The session checks have their own cases (checks/board-session-seal-cases.py); this check needs the store order.
    m.stub('a733_board_session_valid', 1)
    result = m.call('board_session_prepare', CONTRACT)
    return m, result, bytes(session)


def entry_flush(image, patches=()):
    """Run the EL1 entry path from its first reference to the section to the call of a733_cpu_coherence_enable_el1."""
    m = Machine(image, patches)
    m.at_el1()
    low, high = image.sealed
    sites = [a for a, _ in image.references(low, high) if image.function_of(a) == 'a733_cached_enter_el1']
    assert sites, 'the EL1 entry path names no address in the section'
    enable = image.calls('a733_cached_enter_el1', 'a733_cpu_coherence_enable_el1')
    assert len(enable) == 1 and min(sites) < enable[0], (sites, enable)
    m.uc.reg_write(GPR[22], 10 ** 9)
    m.call('a733_cached_enter_el1', start=min(sites), until=enable[0])
    flushed = {t[1] & ~(LINE - 1) for t in m.trace if t[0] == 'dc civac' and t[3] == 1}
    return [line for line in lines(low, high) if line not in flushed]


def manifest_end_references(image, references, patches=()):
    """Prove the two manifest consumers use an aliased end label only as a bound.

    A layout can place __stop_sunstone_manifest exactly at os_sealed. The
    decoder subtracts the manifest start; the restore assembly compares the
    bounds before replacing the end register with a region count. Neither
    dereferences the end register or passes it to another function. Follow
    both sides of branches until that register is overwritten or returns.
    Other readers and other references retain the sealed-owner restriction.
    """
    low, _ = image.sealed
    if image.address('__stop_sunstone_manifest') != low:
        return {}
    overrides = dict(patches)
    manifest = image.address('__start_sunstone_manifest')
    dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    dis.detail = True

    def instruction(pc):
        for start, data in image.code:
            if start <= pc and pc + 4 <= start + len(data):
                raw = struct.pack('<I', overrides[pc]) if pc in overrides else data[pc-start:pc-start+4]
                return next(dis.disasm(raw, pc))
        raise AssertionError(('no boundary instruction', hex(pc)))

    def number(ins, reg):
        name = ins.reg_name(reg) or ''
        return int(name[1:]) if name[:1] in ('w', 'x') and name[1:].isdigit() else None

    def clobbered_before_use(target, reg):
        """A direct callee must overwrite the pointer before reading or saving it."""
        name = image.function_of(target)
        if not name:
            return False
        end = image.address(name) + image.sizes[name]
        for pc in range(target, end, 4):
            ins = instruction(pc)
            reads, writes = ins.regs_access()
            if reg in {number(ins, r) for r in reads}:
                return False
            if reg in {number(ins, r) for r in writes}:
                return True
            if ins.mnemonic in ('bl', 'blr', 'br', 'ret', 'b') or ins.mnemonic.startswith('b.') or ins.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                return False
        return False

    def bound_use(pc, owner, begin, end):
        first = instruction(pc)
        if first.mnemonic != 'adr':
            return None
        reg = number(first, first.operands[0].reg)
        # Only a straight-line, unmodified ADR(start) establishes provenance.
        starts, overwritten = {}, set()
        for at in range(pc-4, begin-1, -4):
            ins = instruction(at)
            if ins.mnemonic in ('bl', 'blr', 'br', 'ret', 'b') or ins.mnemonic.startswith('b.') or ins.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                break
            if ins.mnemonic == 'adr' and ins.operands[-1].imm == manifest:
                dest = number(ins, ins.operands[0].reg)
                if dest is not None and dest not in overwritten:
                    starts[dest] = at
            _, writes = ins.regs_access()
            overwritten.update(number(ins, r) for r in writes)
        starts.pop(reg, None)
        # A branch into the setup could bypass ADR(start). Unknown indirect
        # control flow also prevents proving that assignment dominates ADR(end).
        for at in range(begin, end, 4):
            ins = instruction(at)
            if ins.mnemonic in ('br', 'blr'):
                starts.clear()
                break
            if ins.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.'):
                target = ins.operands[-1].imm
                starts = {r: definition for r, definition in starts.items() if not definition < target <= pc}
        pending, visited, uses = [(pc+4, frozenset(starts))], set(), []
        while pending:
            at, starts = pending.pop()
            if (at, starts) in visited:
                continue
            if not begin <= at < end:
                return None
            visited.add((at, starts))
            ins = instruction(at)
            reads, writes = ins.regs_access()
            if reg in {number(ins, r) for r in reads}:
                # Reject loads, stores, copies, calls and address arithmetic.
                # SUB is the C decoder's manifest-length calculation only.
                span = (owner == 'os_session_decode_why' and ins.mnemonic == 'sub' and len(ins.operands) == 3 and
                        ins.operands[1].type == ARM64_OP_REG and ins.operands[2].type == ARM64_OP_REG and
                        number(ins, ins.operands[1].reg) == reg and number(ins, ins.operands[2].reg) in starts and
                        not ins.operands[2].shift.value and not ins.operands[2].ext)
                if ins.mnemonic != 'cmp' and not span:
                    return None
                uses.append(at)
            if ins.mnemonic == 'ret' and reg == 0:
                return None
            if reg in {number(ins, r) for r in writes} or ins.mnemonic == 'ret':
                continue
            if ins.mnemonic in ('bl', 'blr', 'br'):
                if ins.mnemonic == 'bl' and clobbered_before_use(ins.operands[-1].imm, reg):
                    continue
                return None
            starts = starts - {number(ins, r) for r in writes}
            if ins.mnemonic == 'b':
                pending.append((ins.operands[-1].imm, starts))
            else:
                pending.append((at+4, starts))
                if ins.mnemonic.startswith('b.') or ins.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                    pending.append((ins.operands[-1].imm, starts))
        return (reg, min(uses)) if uses else None

    accepted = {}
    for pc, kind in references:
        if image.reference_target(pc, kind) != low:
            continue
        owner = image.function_of(pc)
        if owner == 'os_session_decode_why':
            begin = image.address(owner)
            end = begin + image.sizes[owner]
        elif image.address('a733_qualification_system_restore') <= pc < image.address('a733_qualification_power_down'):
            # These assembly entry labels have no STT_FUNC size. Bound this
            # consumer by its own entry and the next assembly entry label.
            owner = 'a733_qualification_system_restore'
            begin, end = image.address(owner), image.address('a733_qualification_power_down')
        else:
            continue
        proof = bound_use(pc, owner, begin, end)
        if proof:
            accepted[pc] = {'owner': owner, 'register': proof[0], 'use': proof[1]}
    return accepted


def cache_order(image):
    cases, metrics = {}, {}
    low, high = image.sealed
    metrics['sealed'] = [hex(low), hex(high)]
    metrics['sealed_lines'] = len(lines(low, high))
    # Every branch to the routine that enables EL1 translation is on the one entry path.
    callers = sorted({image.function_of(a) for a in image.branches('a733_cpu_coherence_enable_el1')})
    assert callers == ['a733_cached_enter_el1'], callers
    cases['one-el1-translation-entry'] = True
    for lifetime in (1, 2):
        m, result, session = prepare_session(image, lifetime=lifetime)
        assert result == 1, ('board_session_prepare refused', lifetime)
        faults = order_faults(m.trace, low, high)
        assert not faults, faults[:4]
        stores = [t for t in m.trace if t[0] == 'write']
        assert all(low <= t[1] and t[1] + t[2] <= high for t in stores)
        assert bytes(m.uc.mem_read(low, 4096)) == session
        # EL1 reads the copy and stores nothing to the section.
        m.at_el1()
        before = len(m.trace)
        assert m.call('board_session_snapshot') == low & 0xffffffff
        assert m.call('board_session_sealed') == low & 0xffffffff
        if 'board_session_lifetime' in image.sizes:
            assert m.call('board_session_lifetime') == lifetime
        assert not [t for t in m.trace[before:] if t[0] == 'write'], 'an EL1 reader stored to the section'
        metrics[f'el3_stores_lifetime{lifetime}'] = len(stores)
        metrics[f'el3_civac_lifetime{lifetime}'] = len([t for t in m.trace if t[0] == 'dc civac'])
    cases['el3-civac-before-first-and-after-last-store'] = True
    cases['el1-readers-store-nothing'] = True
    missing = entry_flush(image)
    assert not missing, ['%#x' % a for a in missing[:4]]
    cases['el1-entry-flushes-every-line-before-translation'] = True
    references = image.references(low, high)
    # Any code may name the lifetime copy alone (apps/os/sealed-lifetime.h, os_crumb_lifetime): the EL1 vector's branches
    # (the breadcrumb SVCs, SVC 0x735, the synchronous fault) and the breadcrumb check of sunstone_crumb read it. Every
    # other address in the section is named only by sealed.c and the entry path.
    copy = image.address('os_sealed') + 4100 if 'os_sealed' in image.symbols else None
    lifetime_loads = [a for a, kind in references if image.reference_target(a, kind) == copy]
    bounds = manifest_end_references(image, references)
    owners = sorted({image.function_of(a) or 'unnamed %#x' % a for a, _ in references
                     if a not in lifetime_loads and a not in bounds})
    allowed = {'board_session_prepare', 'board_session_snapshot', 'board_session_sealed', 'board_session_lifetime',
               'a733_cached_enter_el1'}
    assert set(owners) <= allowed, owners
    metrics['references'] = owners
    metrics['lifetime_copy_references'] = len(lifetime_loads)
    metrics['manifest_end_references'] = {hex(a): b['owner'] for a, b in bounds.items()}
    cases['only-sealed-c-and-the-entry-path-address-the-section-beside-the-lifetime-copy'] = True
    for address, proof in bounds.items():
        # Turn a proven bound use into LDR x0,[end]. The special case must
        # then refuse it; knowing a consumer's name alone is insufficient.
        load = 0xf9400000 | proof['register'] << 5
        assert address not in manifest_end_references(image, references, [(proof['use'], load)])
        if proof['owner'] == 'os_session_decode_why':
            # SUB alias,end,#0 is a pointer copy, not a manifest span.
            copy = 0xd1000000 | proof['register'] << 5
            assert address not in manifest_end_references(image, references, [(proof['use'], copy)])
            begin = image.address('os_session_decode_why')
            skip = 0x14000000 | (((address-begin) >> 2) & 0x3ffffff)
            assert address not in manifest_end_references(image, references, [(begin, skip)])
    if bounds:
        cases['manifest-end-alias-is-used-only-as-a-bound'] = True
        cases['mutation-manifest-end-dereference-fails'] = True
        if any(proof['owner'] == 'os_session_decode_why' for proof in bounds.values()):
            cases['mutation-manifest-end-copy-fails'] = True
            cases['mutation-manifest-start-assignment-skipped-fails'] = True

    # Mutations, applied to the linked image in the model.
    flushes = image.calls('board_session_prepare', 'a733_cpu_coherence_flush')
    assert len(flushes) == 2, flushes
    m, result, _ = prepare_session(image, [(flushes[0], MOV_W0_1)])
    assert order_faults(m.trace, low, high), 'removing the first flush passed'
    cases['mutation-first-flush-removed-fails'] = True
    m, result, _ = prepare_session(image, [(flushes[1], bl(flushes[1], image.address('a733_cpu_coherence_publish')))])
    assert result == 1 and any(t[0] == 'dc cvac' for t in m.trace), 'the publication mutation did not run'
    assert order_faults(m.trace, low, high), 'replacing the second flush with the DC CVAC publication passed'
    cases['mutation-second-flush-as-publication-fails'] = True
    site = image.calls('a733_cached_enter_el1', 'a733_cpu_coherence_flush')
    assert len(site) == 1, site
    assert entry_flush(image, [(site[0], MOV_W0_1)]), 'removing the EL1 entry flush passed'
    cases['mutation-el1-entry-flush-removed-fails'] = True
    return cases, metrics


# ---- page tables ----

def leaves(read, root, levels_from=1):
    """(level, index path, descriptor, output base, output bytes, read_only_above) for every valid leaf under root.

    The EL1 regime of these images starts at level 1 (T0SZ 25): 1 GiB, 2 MiB and 4 KiB granules."""
    size = {1: 1 << 30, 2: 1 << 21, 3: 1 << 12}
    found, tables = [], 0

    def walk(table, level, path, read_only):
        nonlocal tables
        tables += 1
        for i in range(512):
            d = read(table + 8 * i)
            if not d & 1:
                continue
            if level < 3 and d & 3 == 3:
                walk(d & 0x0000fffffffff000, level + 1, path + (i,), read_only or bool(d >> 62 & 1))
            elif level == 3 and d & 3 != 3:
                continue        # reserved encoding at level 3: invalid
            else:
                found.append((level, path + (i,), d, d & 0x0000fffffffff000 & ~(size[level] - 1), size[level], read_only))
    walk(root, levels_from, (), False)
    return found, tables


def writable_over(found, low, high):
    """The leaves whose output range overlaps [low, high) and whose AP[2] is clear with no read-only table above."""
    return [(lv, path, hex(d)) for lv, path, d, base, size, read_only in found
            if base < high and low < base + size and not (d >> 7 & 1) and not read_only]


def covering(found, low, high):
    return [(lv, path, hex(d)) for lv, path, d, base, size, _ in found if base < high and low < base + size]


def usable_ranges(base_l1, base_l2):
    """apps/os/large.c usable_ranges: blocks above the floor that the kernel maps as the plain EL1-only block."""
    out = []
    for pa in range(APP_REGION_FLOOR, APP_REGION_BASE, 1 << 21):
        if base_l2[(pa - 0x40000000) >> 21] == pa | KERNEL_BLOCK_BITS:
            out.append([pa, 1 << 21])
    for slot in range(2, APP_REGION_LIMIT >> 30):
        if base_l1[slot] == (slot << 30) | KERNEL_BLOCK_BITS:
            out.append([slot << 30, 1 << 30])
    merged = []
    for base, size in out:
        if merged and merged[-1][0] + merged[-1][1] == base:
            merged[-1][1] += size
        else:
            merged.append([base, size])
    return merged


def build_space(m, entries, flags):
    """Call the linked policy-aware builder on the boot tables; return (result, space root)."""
    image = m.image
    L = image.layouts
    base_l1, base_l2 = SCRATCH, SCRATCH + 0x1000
    # Unicorn accesses here are physical: the OS getters use a virtual table
    # alias after EL1 translation is enabled. Copy the same live boot pages.
    m.uc.mem_write(base_l1, bytes(m.uc.mem_read(image.address('a733_cached_el1_l1'), 4096)))
    m.uc.mem_write(base_l2, bytes(m.uc.mem_read(image.address('a733_cached_el1_l2'), 4096)))
    base_tables = SCRATCH + 0x4000
    pointers = [base_l2] + [0] * ((APP_REGION_LIMIT >> 30) - 2)
    m.uc.mem_write(base_tables, struct.pack('<%dQ' % len(pointers), *pointers))
    entry_at, ledger_at, space_at, arena_at = SCRATCH + 0x2000, SCRATCH + 0x3000, SCRATCH + 0x3100, SCRATCH + 0x3200
    pages = SCRATCH + 0x10000
    entry = L['app_region_entry']
    m.uc.mem_write(entry_at, bytes(0x1000))
    for i, (base, size) in enumerate(entries):
        record = bytearray(24)
        struct.pack_into('<2Q', record, entry['base'], base, size)
        struct.pack_into('<H', record, entry['owner'], 1)
        record[entry['access']] = 3
        m.uc.mem_write(entry_at + 24 * i, bytes(record))
    ledger = bytearray(64)
    struct.pack_into('<Q', ledger, L['app_region_ledger']['entry'], entry_at)
    struct.pack_into('<I', ledger, L['app_region_ledger']['capacity'], len(entries))
    m.uc.mem_write(ledger_at, bytes(ledger))
    arena = bytearray(64)
    struct.pack_into('<2Q', arena, L['app_table_arena']['page'], pages, pages)
    struct.pack_into('<I', arena, L['app_table_arena']['capacity'], 64)
    struct.pack_into('<I', arena, L['app_table_arena']['used'], 0)
    m.uc.mem_write(arena_at, bytes(arena))
    m.uc.mem_write(space_at, bytes(64))
    m.uc.mem_write(pages, bytes(64 * 4096))
    result = m.call('app_space_build_tables', space_at, arena_at, base_l1, base_tables, ledger_at, 1, 1, flags)
    root = m.read(space_at + L['app_space']['root_physical'])
    return result, root


def page_tables(image):
    cases, metrics = {}, {}
    low, high = image.sealed
    m = Machine(image)
    assert m.call('a733_cached_memory_prepare') == 1
    root = image.address('a733_cached_el1_l1')
    found, tables = leaves(m.read, root)
    assert not writable_over(found, low, high), writable_over(found, low, high)
    cover = covering(found, low, high)
    assert len(cover) == 1 and cover[0][:2] == (2, (1, 8)), cover
    metrics['el1_tables'] = tables
    metrics['el1_leaves'] = len(found)
    metrics['el1_covering'] = cover
    metrics['el1_level1_dram_blocks'] = len([f for f in found if f[0] == 1 and f[3] >= APP_REGION_BASE])
    metrics['el1_level2_dram_blocks'] = len([f for f in found if f[0] == 2 and APP_REGION_FLOOR <= f[3] < APP_REGION_BASE])
    metrics['el1_level3_tables'] = tables - 2
    assert metrics['el1_level1_dram_blocks'] and metrics['el1_level2_dram_blocks'], metrics
    cases['el1-tables-leave-the-section-read-only'] = True

    base_l1 = [m.read(root + 8 * i) for i in range(512)]
    base_l2 = [m.read(image.address('a733_cached_el1_l2') + 8 * i) for i in range(512)]
    usable = usable_ranges(base_l1, base_l2)
    assert usable, 'no usable application range'
    metrics['usable_ranges'] = [[hex(b), hex(s)] for b, s in usable]
    for flags, label in ((0, 'gib-blocks'), (1, 'no-gib-blocks')):
        result, space = build_space(m, usable, flags)
        assert result == 0, ('app_space_build_tables refused the usable ranges', label, result)
        found_space, tables_space = leaves(m.read, space)
        assert not writable_over(found_space, low, high), (label, writable_over(found_space, low, high))
        assert covering(found_space, low, high), (label, 'the application space does not map the image text')
        metrics[f'space_{label}_tables'] = tables_space
        cases[f'application-space-{label}-leaves-the-section-read-only'] = True
    result, space = build_space(m, [(0x41000000, 1 << 21)], 0)
    if result == 0:
        found_space, _ = leaves(m.read, space)
        assert not writable_over(found_space, low, high), writable_over(found_space, low, high)
    metrics['space_code_claim_result'] = result
    cases['application-space-claiming-code-is-refused-or-read-only'] = True

    # Mutations.
    bss = image.address('__bss_start')
    assert writable_over(found, bss, bss + (high - low)), 'a section in .bss passed'
    cases['mutation-sealed-in-bss-fails'] = True
    code = image.address('a733_cached_el1_l2') + 8 * ((low - 0x40000000) >> 21)
    m.uc.mem_write(code, struct.pack('<Q', m.read(code) & ~(1 << 7)))
    mutated, _ = leaves(m.read, root)
    assert writable_over(mutated, low, high), 'clearing AP[2] on the CODE block passed'
    cases['mutation-code-block-ap2-cleared-fails'] = True
    return cases, metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {'passed': True, 'images': {},
              'scope': 'Linked-image instruction model: system registers, counter and cache maintenance are modeled; '
                       'no physical cache, walk or coherence behaviour is established.'}
    total = 0
    for path in a.image:
        image = Image(path.resolve())
        order, order_metrics = cache_order(image)
        tables, table_metrics = page_tables(image)
        report['images'][str(path)] = {'cases': {**order, **tables}, 'metrics': {**order_metrics, **table_metrics}}
        total += len(order) + len(tables)
        print(f'{path.name}: sealed {order_metrics["sealed"]}, {len(order) + len(tables)} cases')
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS os sealed session: {total} cases over {len(a.image)} images; cache order at EL3 and EL1 entry, '
          'read-only in the EL1 tables and the application spaces; cache, table and manifest-boundary mutations fail')


if __name__ == '__main__':
    main()
