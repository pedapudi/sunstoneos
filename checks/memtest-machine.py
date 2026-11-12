"""Unicorn model of the returning platform for the memory qualification checks.

Loaded with importlib by checks/memtest-image.py and checks/memtest-emulate.py.
It maps the linked image into a host-owned 64 MiB window buffer, so tests can
alias a tested page onto a window page, and emulates system registers at
their instruction sites:

- the counter returns a modeled value that advances by `step` ticks per read;
- SMC 0x733 stops the run (platform_exit) and SMC 0x734 records a renewal;
- DC ZVA zeroes its line; other cache maintenance is counted and skipped;
- AT walks the translation tables that platform_init wrote, through the
  architecture's level-1 and level-2 descriptor formats.

Device registers are plain memory. None of this models caches, so a check
that depends on eviction or on write-back timing is outside its scope.
"""
import ctypes
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL  # noqa: E402
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_X0,  # noqa: E402
                                 UC_ARM64_REG_PSTATE)

WINDOW, WINDOW_BYTES = 0x40000000, 64 << 20
SENTINEL = 0x43fff000
STACK = 0x43ff0000
K = 0xa5c35a3c00000000
GOLDEN_ATTRIBUTES = (1 << 54) | (1 << 53) | (1 << 10) | (3 << 8) | (1 << 2) | 1
SMC = 0xd4000003
DEVICES = ((0x03000000, 8 << 20), (0x07000000, 1 << 20), (0x08800000, 1 << 20))
REGISTERS = {
    (3, 3, 14, 0, 1): 'cntpct_el0', (3, 3, 14, 0, 0): 'cntfrq_el0', (3, 3, 4, 2, 1): 'daif',
    (3, 3, 0, 0, 7): 'dczid_el0', (3, 1, 0, 0, 1): 'clidr_el1', (3, 1, 0, 0, 0): 'ccsidr_el1',
    (3, 2, 0, 0, 0): 'csselr_el1', (3, 0, 0, 7, 2): 'id_aa64mmfr2_el1', (3, 0, 1, 0, 0): 'sctlr_el1',
    (3, 3, 0, 0, 1): 'ctr_el0', (3, 0, 7, 4, 0): 'par_el1', (3, 0, 4, 2, 2): 'currentel',
    (3, 0, 4, 6, 0): 'icc_pmr_el1', (3, 0, 10, 2, 0): 'mair_el1', (3, 0, 2, 0, 2): 'tcr_el1',
    (3, 0, 2, 0, 0): 'ttbr0_el1', (3, 0, 5, 2, 0): 'esr_el1', (3, 0, 4, 0, 1): 'elr_el1',
    (3, 0, 6, 0, 0): 'far_el1',
    (1, 3, 7, 4, 1): 'dc zva', (1, 3, 7, 14, 1): 'dc civac', (1, 3, 7, 10, 1): 'dc cvac',
    (1, 0, 7, 6, 1): 'dc ivac', (1, 0, 7, 8, 0): 'at s1e1r', (1, 0, 7, 8, 1): 'at s1e1w',
}


def decode_system(word):
    """(name or encoding, is_read, Rt) of an MRS, MSR (register) or SYS word."""
    key = ((word >> 19) & 3, (word >> 16) & 7, (word >> 12) & 15, (word >> 8) & 15, (word >> 5) & 7)
    return REGISTERS.get(key, key), bool(word >> 21 & 1), word & 31


_images = {}


class Machine:
    def __init__(self, elf, step=1, a55_caches=True):
        key = str(Path(elf).resolve())
        if key not in _images:
            _images[key] = harness.Image(elf)
        self.e = _images[key]
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.window = ctypes.create_string_buffer(WINDOW_BYTES)
        self.uc.mem_map_ptr(WINDOW, WINDOW_BYTES, UC_PROT_ALL, ctypes.addressof(self.window))
        for address, size in DEVICES:
            self.uc.mem_map(address, size)
        for address, data in self.e.parts:
            self.uc.mem_write(address, data)
        self.now = 1000
        self.step = step
        self.reads = 0
        self.renewals = []
        self.exit_status = None
        self.counts = {}
        self.sys = {'sctlr_el1': 0x30d00805, 'ctr_el0': 0x8444c004, 'currentel': 4, 'icc_pmr_el1': 0xf0,
                    'dczid_el0': 4, 'clidr_el1': (3 | (4 << 3) | (4 << 6)) if a55_caches else 3,
                    'id_aa64mmfr2_el1': 0, 'csselr_el1': 0, 'par_el1': 0, 'cntfrq_el0': 24000000}
        # Cortex-A55 model: 32 KiB L1D, 256 KiB L2, 2 MiB DSU L3.
        self.ccsidr = {0: (2 | (3 << 3) | (127 << 13)), 2: (2 | (3 << 3) | (1023 << 13)),
                       4: (2 | (15 << 3) | (2047 << 13))}
        self.mapped = [(WINDOW, WINDOW_BYTES)] + list(DEVICES)
        sites = []
        for address, data in self.e.parts:
            for index, (word,) in enumerate(struct.iter_unpack('<I', data[:len(data) & ~3])):
                if harness.system_register_access(word) or word == SMC:
                    sites.append(address + 4 * index)
        self.sites = harness.hook_addresses(self.uc, sites, self._system)
        harness.hook_addresses(self.uc, [SENTINEL], lambda uc, a, s, d: uc.emu_stop())

    # Memory -----------------------------------------------------------------
    def map(self, address, size, alias=None):
        """Map fresh memory, or alias the range onto window memory at alias."""
        if alias is None:
            self.uc.mem_map(address, size)
        else:
            assert WINDOW <= alias and alias + size <= WINDOW + WINDOW_BYTES
            self.uc.mem_map_ptr(address, size, UC_PROT_ALL, ctypes.addressof(self.window) + alias - WINDOW)
        self.mapped.append((address, size))

    def read(self, address, size=8):
        return int.from_bytes(self.uc.mem_read(address, size), 'little')

    def write(self, address, value, size=8):
        self.uc.mem_write(address, value.to_bytes(size, 'little'))

    def address(self, name):
        return self.e.address(name)

    # System registers --------------------------------------------------------
    def translate(self, va, write):
        tcr = self.sys.get('tcr_el1', 0)
        assert tcr & 63 == 25, hex(tcr)
        table = self.sys['ttbr0_el1'] & 0x0000fffffffff000
        if va >> 39:
            return 1
        for shift in (30, 21, 12):
            entry = self.read(table + ((va >> shift) & 511) * 8)
            kind = entry & 3
            if kind == 3 and shift != 12:
                table = entry & 0x0000fffffffff000
                continue
            if not ((kind == 1 and shift != 12) or (kind == 3 and shift == 12)):
                return 1
            if entry & (((1 << shift) - 1) & ~4095) or not entry & (1 << 10) or (write and entry & (1 << 7)):
                return 1
            physical = (entry & 0x0000fffffffff000 & ~((1 << shift) - 1)) | (va & ((1 << shift) - 1))
            attribute = (self.sys['mair_el1'] >> (((entry >> 2) & 7) * 8)) & 255
            return (physical & ~4095) | (attribute << 56) | (((entry >> 8) & 3) << 7)
        return 1

    def _system(self, uc, address, size, data):
        word = int.from_bytes(uc.mem_read(address, 4), 'little')
        reg = UC_ARM64_REG_X0
        if word == SMC:
            operation = uc.reg_read(reg)
            if operation == 0x733:
                self.exit_status = uc.reg_read(reg + 1)
                uc.emu_stop()
                return
            assert operation == 0x734, hex(operation)
            self.renewals.append(self.now)
            uc.reg_write(reg, 0)
            self.counts['smc'] = self.counts.get('smc', 0) + 1
            uc.reg_write(UC_ARM64_REG_PC, address + 4)
            return
        name, is_read, rt = decode_system(word)
        self.counts[name] = self.counts.get(name, 0) + 1
        value = uc.reg_read(reg + rt) if rt != 31 else 0
        if name == 'cntpct_el0':
            self.reads += 1
            self.now += self.step
            result = self.now
        elif name == 'daif':
            result = uc.reg_read(UC_ARM64_REG_PSTATE) & 0x3c0
        elif name == 'ccsidr_el1':
            result = self.ccsidr.get(self.sys['csselr_el1'], 0)
        elif name == 'dc zva':
            self.uc.mem_write(value & ~63, bytes(64))
            result = None
        elif name in ('at s1e1r', 'at s1e1w'):
            self.sys['par_el1'] = self.translate(value, name.endswith('w'))
            result = None
        elif is_read:
            result = self.sys.get(name, 0)
        else:
            if name == 'daif':
                pstate = uc.reg_read(UC_ARM64_REG_PSTATE)
                uc.reg_write(UC_ARM64_REG_PSTATE, (pstate & ~0x3c0) | (value & 0x3c0))
            elif name == 'icc_pmr_el1':
                # The CPU interface implements priority bits 7:4 only.
                self.sys[name] = value & 0xf0
            elif not (isinstance(name, str) and name.startswith(('dc', 'at'))):
                self.sys[name] = value
            result = None
        if is_read and rt != 31:
            uc.reg_write(reg + rt, result)
        uc.reg_write(UC_ARM64_REG_PC, address + 4)

    # Calls -------------------------------------------------------------------
    def call(self, name, *args, count=0, address=None):
        """Call a function with up to eight arguments; return x0.

        The call ends at the sentinel return address or at platform_exit."""
        self.exit_status = None
        self.uc.reg_write(UC_ARM64_REG_SP, STACK)
        self.uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        for i, value in enumerate(args):
            self.uc.reg_write(UC_ARM64_REG_X0 + i, value)
        start = address if address is not None else self.address(name)
        try:
            self.uc.emu_start(start, 0, count=count)
        except UcError:
            pc = self.uc.reg_read(UC_ARM64_REG_PC)
            raise AssertionError(f'emulation of {name} stopped at {pc:#x}')
        return self.uc.reg_read(UC_ARM64_REG_X0)

    def pc(self):
        return self.uc.reg_read(UC_ARM64_REG_PC)

    # Monitor model -----------------------------------------------------------
    def platform_tables(self, tested_map=True):
        """Run platform_init in the state the monitor's EL1 entry leaves:
        DAIF masked and SCTLR_EL1 M, C and I clear. The memtest platform
        writes the tested-map entries (level-2 entries 64-511, level-1
        entries 2-12) itself. With tested_map False they are cleared
        afterwards, which models a platform that maps only 128 MiB. The
        counter advances one tick per read here, whatever step a case sets
        for the run, so the table flushes meet their 10 ms deadlines."""
        self.sys['sctlr_el1'] = 0x30d00800
        self.uc.reg_write(UC_ARM64_REG_PSTATE, self.uc.reg_read(UC_ARM64_REG_PSTATE) | 0x3c0)
        step, self.step = self.step, 1
        self.call('platform_init')
        self.step = step
        assert self.exit_status is None and self.pc() == SENTINEL, f'platform_init stopped at {self.pc():#x}'
        level1, level2 = self.address('level1'), self.address('level2')
        if not tested_map:
            for i in range(64, 512):
                self.write(level2 + 8 * i, 0)
            for i in range(2, 13):
                self.write(level1 + 8 * i, 0)
        return level1, level2

    def table_summary(self):
        """Bytes mapped Normal write-back and non-cacheable, from the tables."""
        level1 = self.sys['ttbr0_el1'] & 0x0000fffffffff000
        wb = nc = 0
        mair = self.sys['mair_el1']
        def account(entry, size):
            nonlocal wb, nc
            attribute = (mair >> (((entry >> 2) & 7) * 8)) & 255
            if attribute == 0xff:
                wb += size
            elif attribute == 0x44:
                nc += size
        for i in range(512):
            entry = self.read(level1 + 8 * i)
            if entry & 3 == 1:
                account(entry, 1 << 30)
            elif entry & 3 == 3:
                for j in range(512):
                    e2 = self.read((entry & 0x0000fffffffff000) + 8 * j)
                    if e2 & 3 == 1:
                        account(e2, 1 << 21)
        return {'write_back_mib': wb >> 20, 'non_cacheable_mib': nc >> 20}
