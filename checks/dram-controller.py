#!/usr/bin/env python3
"""Check memory-bus priority and the latency window against pinned sources.

The priority hook (table and reads-only builds), the latency window and the
decode check run on the host under ASan and UBSan against counting register
models. The register facts those models observe (CCU words and required bits,
the address of every port word, field offsets, masks and targets, and the
counter access sequences) are compared with the pinned sunxi-nsi sources,
the nsi0 node of sun60iw2p1.dtsi, the pinned U-Boot NSI driver and the A733
manual text. The port numbering is checked in the same sources: the sun60iw2
port enum, the node's sub_node_id_mapping, the register index it selects and
the clock that times each port's counters. The component is then compiled for
AArch64 with the image flags. Its code size, its barriers and the copy loop
are read from the objects, and Unicorn runs the linked objects against
counting callbacks to count the instructions of the hook and of each window
step outside those callbacks. Every figure is compared with the budgets of
docs/designs/dram-bus-priority.md. Physical NSI behavior is outside every
case.

Usage: dram-controller.py OUTPUT   (a new directory)
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
from harness import CACHE, CLANG, LLD, SANITIZE, Image, environment  # noqa: E402

SOURCES = CACHE / 'sources'
NEWLIB = CACHE / 'host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include'
MANUAL = ('camera-research/A733_User_Manual_V1.00.txt',
          '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2')
LOCKED = ['gpu-isolation-drivers-bus-sunxi-nsi-c', 'gpu-isolation-drivers-bus-sunxi-nsi-h',
          'gpu-isolation-drivers-bus-sunxi-nsi-bw-c', 'board-gpu-power-sun60iw2p1-dtsi']
# The pinned U-Boot archive and the two members that give its port addresses.
UBOOT = ('board-uboot', 'drivers/nsi/sunxi_nsi.c',
         'arch/arm/include/asm/arch-sunxi/plat-sun60iw2p1/cpu_autogen.h')
# The image C flags that change code size (cmake/aarch64-clang.cmake).
TARGET = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only',
          '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
          '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections', '-O2',
          '-include', 'stdint.h', '-Iinclude', '-isystem', str(NEWLIB)]
TEXT_BUDGET, DATA_BUDGET, DECODE_BUDGET = 2048, 128, 16
OBJDUMP = 'llvm-objdump-21'
# Instructions outside the I/O callbacks, and register accesses per session.
INSTRUCTION_BUDGET = {'hook': 600, 'start': 200, 'collect': 300, 'pending': 50}
SESSION_BUDGET = 4100
NSI_PORT2 = 0x02020000 + 0x200 * 2
STUBS, SENTINEL, HEAP, STACK = 0x50000000, 0x50000100, 0x60000000, 0x70000000


class Emulation:
    """The linked AArch64 objects under Unicorn with Python callbacks.

    Every callback (read, write, enter, leave, counter) is a RET in a stub
    page; a code hook on it acts on a register dictionary first. The memset
    and memmove calls that the compiler emits for structure copies are stubs
    too; library counts them so a measured case can show it makes none. A
    second hook counts the instructions executed inside the linked .text,
    which holds only the package objects, so the count excludes the stubs.
    """

    def __init__(self, path):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
        self.image = Image(path, layout_names={'a733_dram_io', 'a733_dram_window_ops', 'a733_dram_window'})
        layout = self.image.layouts
        assert layout['a733_dram_io'] == {'context': 0, 'read': 8, 'write': 16}, layout['a733_dram_io']
        assert layout['a733_dram_window_ops'] == {'io': 0, 'enter': 24, 'leave': 32, 'counter': 40}
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for base in (0x40000000, STUBS, HEAP, STACK - 0x10000):
            self.uc.mem_map(base, 0x100000)
        for address, data in self.image.parts:
            self.uc.mem_write(address, data)
        self.uc.mem_write(STUBS, struct.pack('<I', 0xd65f03c0) * 64)  # RET
        self.callback = {STUBS + 4 * i: name for i, name in
                         enumerate(('read', 'write', 'enter', 'leave', 'counter', 'memset', 'memmove'))}
        self.stub = {name: address for address, name in self.callback.items()}
        self.regs, self.now, self.step, self.free = {}, 0, 0, False
        self.instructions = self.accesses = self.library = 0
        text = self.text(path)
        self.uc.hook_add(UC_HOOK_CODE, self.count, begin=text[0], end=text[1] - 1)
        self.uc.hook_add(UC_HOOK_CODE, self.act, begin=STUBS, end=SENTINEL)

    @staticmethod
    def text(path):
        from elftools.elf.elffile import ELFFile
        with Path(path).open('rb') as f:
            section = ELFFile(f).get_section_by_name('.text')
            return section['sh_addr'], section['sh_addr'] + section['sh_size']

    def count(self, uc, address, size, data):
        self.instructions += 1

    def x(self, i, value=None):
        from unicorn.arm64_const import UC_ARM64_REG_X0
        if value is None:
            return self.uc.reg_read(UC_ARM64_REG_X0 + i)
        self.uc.reg_write(UC_ARM64_REG_X0 + i, value & (1 << 64) - 1)

    def act(self, uc, address, size, data):
        if address == SENTINEL:
            uc.emu_stop()
            return
        name = self.callback[address]
        if name == 'read':
            self.accesses += 1
            value = self.regs.get(self.x(1), 0)
            if self.free and self.x(1) == NSI_PORT2 + 0xd4:
                self.regs[self.x(1)] = value + 64  # Free-running: read data keeps counting.
            uc.mem_write(self.x(2), struct.pack('<I', value))
            self.x(0, 1)
        elif name == 'write':
            self.accesses += 1
            self.regs[self.x(1)] = self.x(2) & 0xffffffff
            self.x(0, 1)
        elif name == 'enter':
            self.x(0, 1)
        elif name == 'counter':
            self.now += self.step
            self.x(0, self.now)
        elif name == 'memset':
            self.library += 1
            uc.mem_write(self.x(0), bytes([self.x(1) & 0xff]) * self.x(2))
        elif name == 'memmove':
            self.library += 1
            uc.mem_write(self.x(0), bytes(uc.mem_read(self.x(1), self.x(2))))

    def call(self, name, *args):
        """Instructions executed in .text by one call and its return value."""
        from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_SP
        for i, value in enumerate(args):
            self.x(i, value)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK - 64)
        self.uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        self.instructions = self.accesses = self.library = 0
        self.uc.emu_start(self.image.address(name), SENTINEL, count=100000)
        return self.instructions, self.x(0) & 0xffffffff

    def io(self, at):
        """An a733_dram_io at `at` over the stubs; returns the next free address."""
        self.uc.mem_write(at, struct.pack('<QQQ', 0, self.stub['read'], self.stub['write']))
        return at + 24


def node(text, label):
    """Body of the device-tree node that starts with label."""
    start = text.index(label)
    depth, i = 0, text.index('{', start)
    for j in range(i, len(text)):
        depth += (text[j] == '{') - (text[j] == '}')
        if not depth:
            return text[i:j]
    raise AssertionError(label)


def sizes(path):
    """Berkeley text, data and bss of an ELF64 relocatable object."""
    raw = path.read_bytes()
    assert raw[:4] == b'\x7fELF' and raw[4] == 2 and raw[5] == 1
    shoff, = struct.unpack_from('<Q', raw, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', raw, 0x3a)
    text = data = bss = 0
    for i in range(shnum):
        _, kind, flags, _, _, size = struct.unpack_from('<IIQQQQ', raw, shoff + i * shentsize)
        if not flags & 2:  # SHF_ALLOC
            continue
        if kind == 8:  # SHT_NOBITS
            bss += size
        elif flags & 1:  # SHF_WRITE
            data += size
        else:
            text += size
    return text, data, bss


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path, help='new directory for logs and results')
    out = ap.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs, results = {}, {}

    def read(path):
        data = path.read_bytes()
        inputs[str(path)] = hashlib.sha256(data).hexdigest()
        return data.decode(errors='replace')

    def run(name, argv):
        r = subprocess.run([str(a) for a in argv], cwd=ROOT, env=environment(), text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f'{name}.log').write_text(' '.join(map(str, argv)) + '\n' + r.stdout)
        assert r.returncode == 0, f'{name} failed:\n{r.stdout}'
        return r.stdout

    lock = json.loads((ROOT / 'sources.lock.json').read_text())
    pinned = {}
    for key in LOCKED:
        path = SOURCES / lock[key]['archive']
        pinned[key] = read(path)
        assert inputs[str(path)] == lock[key]['sha256'], key
    entry = lock[UBOOT[0]]
    archive = SOURCES / entry['archive']
    digest = hashlib.sha256()
    with archive.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    inputs[str(archive)] = digest.hexdigest()
    assert inputs[str(archive)] == entry['sha256'], UBOOT[0]
    with tarfile.open(archive) as tar:
        uboot_nsi, autogen = (tar.extractfile(f"{entry['directory']}/{name}").read().decode()
                              for name in UBOOT[1:])
    manual = read(SOURCES / MANUAL[0])
    assert inputs[str(SOURCES / MANUAL[0])] == MANUAL[1]
    nsi, nsih = pinned['gpu-isolation-drivers-bus-sunxi-nsi-c'], pinned['gpu-isolation-drivers-bus-sunxi-nsi-h']
    bw, dtsi = pinned['gpu-isolation-drivers-bus-sunxi-nsi-bw-c'], pinned['board-gpu-power-sun60iw2p1-dtsi']

    # Vendor facts. Port words: mode, priority and select at +0x10, +0x14,
    # +0x18 with a 0x200 stride; counters at +0xc0..+0xdc.
    offset = {}
    for name in ['IAG_MODE', 'IAG_PRI_CFG', 'IAG_INPUT_OUTPUT_CFG', 'MBUS_PMU_ENABLE', 'MBUS_PMU_CLR',
                 'MBUS_PMU_CYCLE', 'MBUS_PMU_RQ_RD', 'MBUS_PMU_RQ_WR', 'MBUS_PMU_DT_RD', 'MBUS_PMU_DT_WR',
                 'MBUS_PMU_LA_RD']:
        m = re.search(r'#define ' + name + r'\(n\)\s+\((0x[0-9a-fA-F]+) \+ \(0x200 \* \(n\)\)\)', nsih)
        assert m, name
        offset[name] = int(m[1], 16)
    # Field writes: read-modify-write of mode bits 1-0, both 2-bit priority
    # subfields set to p, select bit 0; counters: period, disable, clear.
    assert re.search(r'value &= ~0x3;\s*writel_relaxed\(value \| mode, sunxi_nsi\.base \+ IAG_MODE\(port\)\);', nsi)
    assert re.search(r'value &= ~0xf;\s*writel_relaxed\(value \| \(pri << 2\) \| pri,\s*sunxi_nsi\.base \+ IAG_PRI_CFG\(port\)\);', nsi)
    assert re.search(r'value &= ~0x1;\s*writel_relaxed\(value \| io,\s*sunxi_nsi\.base \+ IAG_INPUT_OUTPUT_CFG\(port\)\);', nsi)
    assert re.search(r'#define MBUS_PRI_MAX\s+0x3', nsih)
    assert re.search(r'writel_relaxed\(cycle, sunxi_nsi\.base \+ MBUS_PMU_CYCLE\(port\)\);\s*/\* disabled the pmu count \*/\s*'
                     r'nsi_pmu_disable\(port\);\s*/\* clean the insight counter \*/\s*nsi_pmu_clear\(port\);', nsi)
    assert re.search(r'return period \* \(__rate_for_master\(port\) / 1000000\);', nsi)
    # Latency: sum over requests, read while counting, 0 for no requests.
    assert re.search(r'laread = readl_relaxed\(sunxi_nsi\.base \+ MBUS_PMU_LA_RD\(ia_index\)\);\s*'
                     r'request = readl_relaxed\(sunxi_nsi\.base \+ MBUS_PMU_RQ_RD\(ia_index\)\);', bw)
    # nsi0 node: base, interrupt, port owners and the vendor values.
    n = node(dtsi, 'nsi0: nsi-controller@2020000')
    assert re.search(r'reg = <0x0 0x02020000 0x0 0x10000>', n) and 'GIC_SPI 43' in n
    assert re.search(r'ia_pmu_data_unit = <16>;', n)
    table = {}
    for label in ['iommu0', 'iommu1', 'de']:
        body = node(n, label + '{' if label != 'de' else 'de {')
        table[label] = tuple(int(re.search(k + r'\s*=\s*<(\d+)>', body)[1]) for k in ('id', 'mode', 'pri', 'select'))
    assert table == {'de': (2, 0, 2, 1), 'iommu0': (14, 0, 3, 1), 'iommu1': (15, 0, 3, 1)}, table
    nsi_base = int(re.search(r'reg = <0x0 (0x[0-9a-fA-F]+) 0x0 0x10000>', n)[1], 16)
    # Port numbering. The sun60iw2 branch of the port enum gives the same ids
    # as the nsi0 node.
    branch = nsih[nsih.index('#elif IS_ENABLED(CONFIG_ARCH_SUN60IW2)'):]
    branch = branch[:branch.index('#elif', 1)]
    enum = {k: int(v) for k, v in re.findall(r'MBUS_PMU_(\w+)\s*=\s*(\d+),', branch)}
    assert (enum['DE'], enum['IOMMU0'], enum['IOMMU1']) == (table['de'][0], table['iommu0'][0],
                                                          table['iommu1'][0]), enum
    # sub_node_id_mapping with child ids makes a port's register index its
    # id. The other mapping subtracts one; the header defines its macro only
    # for sun55iw3, and every subtraction sits under that macro.
    assert re.search(r'^\s*sub_node_id_mapping;', n, re.M) and 'port_id' not in n
    assert 'master_dev->ia_master.ia_index = id;' in nsi
    assert re.findall(r'#(?:el)?if[^\n]*\n#define AW_NSI_CPU_CHANNEL\s+1\n#define NSI_HARDCODED_PORT_MAPPING 1', nsih) == \
        ['#if IS_ENABLED(CONFIG_ARCH_SUN55IW3)\n#define AW_NSI_CPU_CHANNEL\t1\n#define NSI_HARDCODED_PORT_MAPPING 1']
    assert nsih.count('NSI_HARDCODED_PORT_MAPPING') == 1
    subtract = re.findall(r'([^\n]*)\n\s*port = port - 1;', nsi)
    assert subtract and set(subtract) == {'#if defined(AW_NSI_CPU_CHANNEL) && defined(NSI_HARDCODED_PORT_MAPPING)'}
    # Counter clock: with topology 2 a port absent from master_clks keeps
    # clock index 1, which the node names "bus" and points at CLK_MBUS.
    assert re.search(r'topology_type = <2>;', n) and re.search(r'NSI_TOPO_V1,[^\n]*\n\s*NSI_TOPO_V2,', nsih)
    assert re.search(r'for_each_ports\(i\) \{\s*sunxi_nsi\.clk_idx\[i\] = 1;', nsi)
    clocked = [int(a) for a, _ in re.findall(r'<(\d+) (\d+)>', re.search(r'master_clks = ([^;]*);', n)[1])]
    assert not {table[k][0] for k in table} & set(clocked), clocked
    assert re.search(r'clocks = <&ccu CLK_PLL_DDR>, <&ccu CLK_MBUS> ,', n)
    assert re.search(r'clock-names = "pll", "bus",', n)
    # U-Boot addresses the same words: base 0x02020000, 0x200 per port.
    assert int(re.search(r'#define SUNXI_NSI_BASE\s+(0x[0-9a-fA-F]+)', autogen)[1], 16) == nsi_base
    for name, word in (('IAG_MODE', 0x10), ('IAG_PRI_CFG', 0x14), ('IAG_QOS_SEL_CFG', 0x18)):
        assert re.search(r'#define ' + name + r'\(n\) \(SUNXI_NSI_BASE \+ 0x%04x \+ \(0x200 \* \(n\)\)\)' % word,
                         uboot_nsi), name
    # CCU: NSI_CLK_REG gate 31 and reset 30; NSI_BGR_REG reset 16 and gate 0.
    ccu = int(re.search(r'CCU\s+(0x[0-9A-Fa-f]{8})---0x[0-9A-Fa-f]{8}', manual)[1], 16)
    assert re.search(r'NSI_CLK_REG\s+0x0580\s+NSI Clock Register', manual)
    assert re.search(r'NSI_BGR_REG\s+0x0584\s+NSI Bus Gating Reset Register', manual)
    clk = manual[manual.index('4.1.6.74          0x0580'):manual.index('4.1.6.75          0x0584')]
    bgr = manual[manual.index('4.1.6.75          0x0584'):manual.index('4.1.6.76          0x0588')]
    assert re.search(r'NSI_CLK_GATING\.\s*Gating Special Clock\s*31\s+R/W', clk)
    assert re.search(r'NSI_RST\.\s*NSI Reset\.\s*30\s+R/W', clk)
    assert re.search(r'NSI_CFG_RST\.\s*NSI Reset\s*16\s+R/W', bgr)
    assert re.search(r'NSI_CFG_GATING\.\s*Gating Clock for NSI_CFG\s*0\s+R/W', bgr)
    results['sources'] = 'pass'

    # Host models under the sanitizers.
    common = [CLANG, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', *SANITIZE, '-Iinclude']
    for mode in (2, 1):
        exe = out / f'dram-priority-{mode}'
        run(f'build-priority-{mode}', [*common, f'-DSUNSTONE_DRAM_PRIORITY={mode}',
                                       'checks/dram-priority.c', 'drivers/dram/a733_dram.c',
                                       'drivers/dram/a733_dram_decode.c', '-o', exe])
        log = run(f'priority-{mode}', [exe])
        assert f'PASS memory-bus priority hook, mode {mode}' in log
        results[f'priority-{mode}'] = json.loads(log.splitlines()[0])
    exe = out / 'dram-window'
    run('build-window', [*common, '-DSUNSTONE_DRAM_PRIORITY=2', '-DSUNSTONE_GPU_DISPLAY_DRAM_LOAD=1',
                         'checks/dram-window.c', 'drivers/dram/a733_dram.c', 'drivers/dram/a733_dram_load.c',
                         '-o', exe])
    log = run('window', [exe])
    assert 'PASS latency window' in log
    window = results['window'] = json.loads(log.splitlines()[0])
    assert window['session_accesses'] <= SESSION_BUDGET and window['after_failure'] == 0, window

    # The facts the compiled code showed against the vendor facts.
    hook = results['priority-2']
    assert hook['ccu'] == [[ccu + 0x580, (1 << 31) | (1 << 30)], [ccu + 0x584, (1 << 16) | 1]], hook['ccu']
    assert results['priority-1']['ccu'] == hook['ccu']
    port_of = {v[0]: v for v in table.values()}
    words = (offset['IAG_MODE'], offset['IAG_PRI_CFG'], offset['IAG_INPUT_OUTPUT_CFG'])
    order = [ccu + 0x580, ccu + 0x584] + [nsi_base + 0x200 * port + word for port in (2, 14, 15) for word in words]
    for mode in (2, 1):
        assert results[f'priority-{mode}']['order'] == order, results[f'priority-{mode}']['order']
    assert [f[4] for f in hook['fields']] == order[2:], hook['fields']
    assert all(f[4] == 0 for f in results['priority-1']['fields'])
    fields = [(offset['IAG_MODE'], 0x3, lambda p: p[1]), (offset['IAG_PRI_CFG'], 0xf, lambda p: p[2] << 2 | p[2]),
              (offset['IAG_INPUT_OUTPUT_CFG'], 0x1, lambda p: p[3])]
    expect = [[port, word, mask, value(port_of[port])]
              for port in (2, 14, 15) for word, mask, value in fields]
    assert [f[:4] for f in hook['fields']] == expect, hook['fields']
    assert all(f[2] == 0 for f in results['priority-1']['fields'])  # Reads only: nothing written.
    assert window['port'] == nsi_base + 0x200 * table['de'][0]
    assert window['start'] == [[offset['MBUS_PMU_CYCLE'], 1], [offset['MBUS_PMU_ENABLE'], 0],
                               [offset['MBUS_PMU_ENABLE'], 1], [offset['MBUS_PMU_CLR'], 0],
                               [offset['MBUS_PMU_CLR'], 1], [offset['MBUS_PMU_ENABLE'], 1]], window['start']
    assert window['collect'] == [offset[k] for k in ('MBUS_PMU_RQ_RD', 'MBUS_PMU_RQ_WR', 'MBUS_PMU_DT_RD',
                                                     'MBUS_PMU_DT_WR', 'MBUS_PMU_LA_RD', 'MBUS_PMU_DT_RD')]
    assert window['period_cycles'] == 50 * (600000000 // 1000000) * 1000  # 50 ms x MHz, as the vendor computes.
    results['observed'] = 'match'

    # AArch64 code size, barriers and the copy loop with the image flags.
    size, barriers = {}, {}
    for name, defines, source in [
            ('hook', ['-DSUNSTONE_DRAM_PRIORITY=2'], 'drivers/dram/a733_dram.c'),
            ('hook-reads-only', ['-DSUNSTONE_DRAM_PRIORITY=1'], 'drivers/dram/a733_dram.c'),
            ('load', ['-DSUNSTONE_DRAM_PRIORITY=2', '-DSUNSTONE_GPU_DISPLAY_DRAM_LOAD=1'], 'drivers/dram/a733_dram_load.c'),
            ('copy', [], 'drivers/dram/a733_dram_copy.S'),
            ('decode', [], 'drivers/dram/a733_dram_decode.c')]:
        obj = out / f'{name}.o'
        flags = [f for f in TARGET if f not in ('-mgeneral-regs-only', '-include', 'stdint.h')] \
            if source.endswith('.S') else TARGET
        run(f'size-{name}', [CLANG, *flags, *defines, '-c', source, '-o', obj])
        size[name] = dict(zip(('text', 'data', 'bss'), sizes(obj)))
        listing = run(f'disassemble-{name}', [OBJDUMP, '-d', '--no-show-raw-insn', obj])
        barriers[name] = len(re.findall(r'\t(?:dsb|dmb)\t', listing))
        if name == 'copy':
            loop = re.search(r'<a733_dram_copy_2mib>:\n(.*?)\n\s*[0-9a-f]+:\s+ret', listing, re.S)[1].splitlines()[1:]
            assert [line.split('\t')[1] for line in loop] == ['ldp', 'ldp', 'stp', 'stp', 'subs', 'b.ne'], loop
            results['copy_loop_instructions_per_64_bytes'] = len(loop)
        if name == 'load':
            body = re.search(r'<a733_dram_copy_pass>:\n(.*?)\n\n', listing + '\n\n', re.S)[1]
            results['copy_pass_acquire_loads'] = len(re.findall(r'\tldar\t', body))
            assert results['copy_pass_acquire_loads'] == 2, body
    for name in ('hook', 'hook-reads-only'):
        assert size[name]['text'] <= TEXT_BUDGET and size[name]['data'] + size[name]['bss'] <= DATA_BUDGET, size
    assert size['decode']['text'] <= DECODE_BUDGET and not size['decode']['data'] + size['decode']['bss'], size
    assert not any(barriers.values()), barriers
    results['aarch64_size'], results['barriers'] = size, barriers
    # The framebuffer's raw path brackets each access with DSB SY.
    results['dsb_sy_per_sequence'] = {
        'hook_worst': 2 * (2 + hook['worst']), 'start': 2 * window['start_accesses'],
        'collect': [2 * window['collect_accesses'], 2 * window['collect_after_fallback']]}

    # Instructions outside the I/O callbacks, from the linked objects.
    script = out / 'emulation.ld'
    script.write_text('SECTIONS { . = 0x40000000; .text : { *(.text*) } .rodata : { *(.rodata*) } '
                      '.data : { *(.data*) } .bss : { *(.bss*) } }\n')
    instructions = {}
    for mode in (2, 1):
        objects = []
        for name, source in (('hook', 'drivers/dram/a733_dram.c'), ('load', 'drivers/dram/a733_dram_load.c'),
                             ('copy', 'drivers/dram/a733_dram_copy.S')):
            obj = out / f'emulation-{mode}-{name}.o'
            flags = [f for f in TARGET if f not in ('-mgeneral-regs-only', '-include', 'stdint.h')] \
                if source.endswith('.S') else TARGET
            run(f'emulation-{mode}-{name}', [CLANG, *flags, '-g', f'-DSUNSTONE_DRAM_PRIORITY={mode}',
                                             '-DSUNSTONE_GPU_DISPLAY_DRAM_LOAD=1', '-c', source, '-o', obj])
            objects.append(obj)
        elf = out / f'emulation-{mode}.elf'
        run(f'link-{mode}', [LLD, '-static', '-nostdlib', '--no-undefined', '-e', 'a733_dram_priority',
                             f'--defsym=memset={STUBS + 20:#x}', f'--defsym=memmove={STUBS + 24:#x}',
                             '-T', script, *objects, '-o', elf])
        emu = Emulation(elf)
        evidence, io = HEAP, HEAP + 0x100
        emu.io(io)
        ccu = {0x02002580: (1 << 31) | (1 << 30), 0x02002584: (1 << 16) | 1}
        target = {}
        for port, p in ((2, 2), (14, 3), (15, 3)):
            base = 0x02020000 + 0x200 * port
            target.update({base + 0x10: 0, base + 0x14: p << 2 | p, base + 0x18: 1})
        cases = {'reset': {a: 0 for a in target}, 'table': dict(target),
                 'worst': {a: (3 if a & 0x1f == 0x10 else 0) for a in target}}
        for case, fields in cases.items():
            emu.regs = {**ccu, **fields}
            count, result = emu.call('a733_dram_priority', io, evidence)
            assert result == 1 and not emu.library, case
            instructions[f'hook-{mode}-{case}'] = [count, emu.accesses]
        emu.regs = {0x02002580: 0, 0x02002584: 0}
        count, result = emu.call('a733_dram_priority', io, evidence)
        assert result == 0 and emu.accesses == 2
        instructions[f'hook-{mode}-off'] = [count, emu.accesses]
    assert [instructions[f'hook-2-{c}'][1] for c in ('reset', 'table', 'worst')] == [23, 11, 29], instructions
    # The window, in the last image: init, then one step per case.
    w, ops, report = HEAP + 0x1000, HEAP + 0x800, HEAP + 0x2000
    emu.uc.mem_write(ops, struct.pack('<QQQQQQ', 0, emu.stub['read'], emu.stub['write'], emu.stub['enter'],
                                      emu.stub['leave'], emu.stub['counter']))
    hz, period = 24000000, 24000000 // 20
    emu.now, emu.regs = 1 << 40, {NSI_PORT2 + 0xcc: 4000, NSI_PORT2 + 0xd4: 20000, NSI_PORT2 + 0xdc: 4000 * 300}
    _, ok = emu.call('a733_dram_window_init', w, ops, hz, 600000000, emu.now + 18 * hz, report)
    assert ok == 1
    in_flight = w + emu.image.layouts['a733_dram_window']['in_flight']

    def step(advance, accesses, flying):
        emu.now += advance
        count, result = emu.call('a733_dram_window_step', w, 1)
        assert result == 0 and emu.accesses == accesses and not emu.library, (result, emu.accesses)
        assert struct.unpack('<I', bytes(emu.uc.mem_read(in_flight, 4)))[0] == flying
        return count
    steps = {'start': [step(0, 6, 1)], 'pending': [step(period // 10, 0, 1)], 'collect': [step(period, 6, 0)]}
    steps['start'].append(step(0, 6, 1))
    emu.free = True
    steps['collect'].append(step(period, 6, 0))  # Refused as free-running; the fallback engages.
    emu.free = False
    steps['start'].append(step(0, 6, 1))
    steps['pending'].append(step(1, 0, 1))
    steps['collect'].append(step(period, 7, 0))
    for name, counts in steps.items():
        instructions[name] = max(counts)
        assert instructions[name] <= INSTRUCTION_BUDGET[name], (name, counts)
    assert all(v[0] <= INSTRUCTION_BUDGET['hook'] for k, v in instructions.items() if k.startswith('hook')), instructions
    results['instructions'] = instructions
    (out / 'results.json').write_text(json.dumps({'results': results, 'inputs': inputs}, indent=1) + '\n')
    print(json.dumps(results, indent=1))
    print('PASS DRAM controller: priority hook, latency window, decode check, observed register facts '
          'against the pinned sources, code size, barriers, copy loop and instruction counts')


if __name__ == '__main__':
    main()
