#!/usr/bin/env python3
"""Check CPU clock tables against pinned sources and run the transition model.

It also runs the native cycle-counter code against modeled system registers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
from harness import CACHE, CLANG, SANITIZE, environment  # noqa: E402

SOURCES = CACHE / 'sources'
UBOOT = SOURCES / 'u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a'
BINS = ['vf0000', 'vf0100', 'vf0200', 'vf0300', 'vf0400', 'vf0500', 'vf0600', 'vf0401', 'vf0501']
UNITS = ['drivers/clock/a733_cpu_clock.c', 'drivers/power/a733_pmic.c',
         'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
         'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
         'drivers/pinctrl/a733_pin_tables.c', 'drivers/thermal/a733_thermal.c']
CHANGED = ['drivers/clock/a733_cpu_clock.c', 'drivers/power/a733_pmic.c',
           'drivers/clock/a733_clock_leases.c', 'drivers/thermal/a733_thermal.c',
           'drivers/clock/a733_cpu_cycles_native.c']
# System registers the native cycle code may access: (op0, op1, CRn, CRm, op2).
SYSREGS = {(3, 3, 9, 12, 0): 'pmcr', (3, 3, 9, 12, 1): 'pmcntenset', (3, 3, 9, 13, 0): 'pmccntr',
           (3, 3, 14, 15, 7): 'pmccfiltr', (3, 3, 14, 0, 1): 'cntpct', (3, 0, 0, 0, 0): 'midr',
           (3, 0, 0, 0, 5): 'mpidr', (3, 3, 4, 2, 1): 'daif', (3, 0, 4, 2, 2): 'el'}
DAIFSET_I = 0xd50342df  # msr daifset, #2


def node(text, label):
    """Body of the device-tree node that starts with label."""
    start = text.index(label)
    depth, i = 0, text.index('{', start)
    for j in range(i, len(text)):
        depth += (text[j] == '{') - (text[j] == '}')
        if not depth:
            return text[i:j]
    raise AssertionError(label)


def dtsi_rows(text, label):
    rows = []
    for m in re.finditer(r'opp[@-][\w-]*\s*\{(.*?)\};', node(text, label), re.S):
        hz = int(re.search(r'opp-hz = /bits/ 64 <(\d+)>', m[1])[1])
        cols = dict(re.findall(r'opp-microvolt-26m-(vf\w+) = <(\d+)>', m[1]))
        mv = [int(cols[b]) // 1000 for b in BINS]
        assert all(int(cols[b]) % 1000 == 0 for b in BINS)
        if any(mv):
            rows.append((hz // 1000000, mv))
    return sorted(rows)


def c_rows(text, name):
    body = re.search(r'static const struct a733_cpu_opp ' + name + r'\[\]=\{(.*?)\n\};', text, re.S)[1]
    return [(int(m[1]), [int(v) for v in m[2].split(',')])
            for m in re.finditer(r'\{(\d+),\{([\d,]+)\}\}', body)]


def linux_best(rate, nmin, nmax):
    """ccu_nkmp_find_best with k=m=1 and P in 1,2,4,8, 26 MHz parent."""
    best = (0, 0, 0)
    for n in range(nmin, nmax + 1):
        p = 1
        while p <= 8:
            tmp = 26000000 * n // p
            if tmp <= rate and rate - tmp < rate - best[0]:
                best = (tmp, n, p)
            p <<= 1
    return best


def native_call(code, entry, state, out=None):
    """Run one native function under Unicorn with modeled system registers.

    state maps register names to values and is updated by writes. PMCR P and C
    are write-only resets and must never be written as 1; PMCNTENSET writes
    set bits. With out, x0 points at those bytes. Returns the 32-bit result,
    the ordered register events and the bytes at x0 afterwards."""
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INSN, UC_HOOK_CODE
    from unicorn.arm64_const import (UC_ARM64_INS_MRS, UC_ARM64_INS_MSR, UC_ARM64_REG_LR, UC_ARM64_REG_PC,
                                     UC_ARM64_REG_X0)
    base, data, end = 0x10000, 0x20000, 0x10ffc
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(base, 0x1000)
    uc.mem_write(base, code)
    uc.mem_map(data, 0x1000)
    events = []

    def key(cp):
        return SYSREGS[(cp.op0, cp.op1, cp.crn, cp.crm, cp.op2)]

    def mrs(machine, reg, cp, user):
        name = key(cp)
        if name in ('cntpct', 'pmccntr'):
            assert state['daif'] & 0x80, 'counter read with IRQs unmasked'
        events.append(('read', name))
        machine.reg_write(reg, state[name])
        return True

    def msr(machine, reg, cp, user):
        name, value = key(cp), cp.val
        assert name in ('pmcr', 'pmcntenset', 'pmccfiltr', 'daif'), name
        if name == 'pmcr':
            assert not value & 6, 'counter reset written'
            if not state.get('drop'):
                state['pmcr'] = value
        elif name == 'pmcntenset':
            state[name] |= value
        else:
            state[name] = value
        events.append(('write', name, value))
        return True

    def code_hook(machine, address, size, user):
        if struct.unpack('<I', machine.mem_read(address, 4))[0] == DAIFSET_I:
            state['daif'] |= 0x80
            events.append(('mask',))

    uc.hook_add(UC_HOOK_INSN, mrs, None, 1, 0, UC_ARM64_INS_MRS)
    uc.hook_add(UC_HOOK_INSN, msr, None, 1, 0, UC_ARM64_INS_MSR)
    uc.hook_add(UC_HOOK_CODE, code_hook, begin=base, end=base + len(code))
    if out is not None:
        uc.mem_write(data, out)
    uc.reg_write(UC_ARM64_REG_X0, data if out is not None else 0)
    uc.reg_write(UC_ARM64_REG_LR, end)
    uc.emu_start(base + entry, end, count=1000)
    assert uc.reg_read(UC_ARM64_REG_PC) == end, 'function did not return'
    return uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff, events, bytes(uc.mem_read(data, 32))


def cycles_model(obj):
    """Nominal and refusal cases for a733_cpu_cycles_enable and _sample."""
    from elftools.elf.elffile import ELFFile
    with obj.open('rb') as f:
        elf = ELFFile(f)
        assert not elf.get_section_by_name('.rela.text'), 'native cycle code needs no relocations'
        code = elf.get_section_by_name('.text').data()
        entry = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
    enable, sample = entry['a733_cpu_cycles_enable'], entry['a733_cpu_cycles_sample']
    reset = {'el': 4, 'pmcr': 0x41002008, 'pmcntenset': 1, 'pmccfiltr': 0x80000000, 'pmccntr': 123456789,
             'cntpct': 987654, 'midr': 0x410fd055, 'mpidr': 0x81000000, 'daif': 0}
    cases = []
    # Enable sets E and LC, clears D, preserves other counters, never resets.
    state = dict(reset)
    result, events, _ = native_call(code, enable, state)
    assert result == 1 and state['pmcr'] == 0x41002041 and state['pmcntenset'] == 0x80000001
    assert state['pmccfiltr'] == 0 and state['pmccntr'] == 123456789
    assert [e for e in events if e[0] == 'write'] == [('write', 'pmccfiltr', 0), ('write', 'pmcntenset', 0x80000000),
                                                      ('write', 'pmcr', 0x41002041)]
    cases.append('enable')
    enabled = dict(state)
    for name, change in [('enable at EL0', {'el': 0}), ('enable at EL2', {'el': 8})]:
        state = {**reset, **change}
        result, events, _ = native_call(code, enable, state)
        assert result == 0 and events == [('read', 'el')], name
        cases.append(name)
    state = {**reset, 'drop': True}
    result, events, _ = native_call(code, enable, state)
    assert result == 0, 'ignored PMCR write accepted'
    cases.append('enable with an ignored PMCR write')
    # Sample: both counters read together with IRQs masked, DAIF restored.
    state = dict(enabled)
    result, events, out = native_call(code, sample, state, bytes(32))
    assert result == 1 and out == struct.pack('<4Q', 0x410fd055, 0x81000000, 123456789, 987654)
    assert state['daif'] == 0
    order = [events.index(e) for e in [('mask',), ('read', 'cntpct'), ('read', 'pmccntr'), ('write', 'daif', 0)]]
    assert order == sorted(order) and order[2] + 1 == order[3], events
    cases.append('sample')
    fill = bytes([0xa5]) * 32
    for name, change in [('sample with E clear', {'pmcr': 0x41002040}), ('sample with D set', {'pmcr': 0x41002049}),
                         ('sample with the cycle counter disabled', {'pmcntenset': 1}),
                         ('sample with EL1 filtered', {'pmccfiltr': 0x80000000}), ('sample at EL0', {'el': 0})]:
        state = {**enabled, **change}
        result, events, out = native_call(code, sample, state, fill)
        assert result == 0 and out == fill and state['daif'] == 0, name
        cases.append(name)
    state = dict(enabled)
    result, events, _ = native_call(code, sample, state)
    assert result == 0 and events == [], 'null output'
    cases.append('sample to a null output')
    return cases


def blob(path):
    data = path.read_bytes()
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path, help='new directory for logs and results')
    out = ap.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {}

    def read(path):
        data = path.read_bytes()
        inputs[str(path)] = hashlib.sha256(data).hexdigest()
        return data.decode()

    def run(name, argv):
        r = subprocess.run([str(a) for a in argv], cwd=ROOT, env=environment(), text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f'{name}.log').write_text(' '.join(map(str, argv)) + '\n' + r.stdout)
        assert r.returncode == 0, f'{name} failed:\n{r.stdout}'
        return r.stdout

    lock = json.loads((ROOT / 'sources.lock.json').read_text())
    pinned = {}
    for key in ['npu-platform-sun60iw2p1-cpu-vf-dtsi', 'board-gpu-power-sun60i-a733-orangepi-zero3w-dts',
                'board-gpu-power-sun60iw2p1-dtsi', 'thermal-sunxi-sid-c', 'board-pmic-driver',
                'board-pmic-registers']:
        path = SOURCES / lock[key]['archive']
        pinned[key] = read(path)
        assert inputs[str(path)] == lock[key]['sha256'], key
    tree = {e['path']: e['sha'] for e in json.loads(read(SOURCES / 'board-kernel/tree-2ac08e8.json'))['tree']}
    kernel = {}
    for name, where in [('ccu-sun60iw2-cpupll.c', 'bsp/drivers/clk/sunxi-ng/'), ('ccu_nkmp.c', 'bsp/drivers/clk/sunxi-ng/'),
                        ('ccu_common.c', 'bsp/drivers/clk/sunxi-ng/'),
                        ('axp2101-regulator.c', 'bsp/drivers/power/regulator/'),
                        ('sunxi-dsufreq.c', 'bsp/drivers/dsufreq/'),
                        ('cpufreq-dt.c', 'bsp/drivers/cpufreq/cpufreq-linux-6.6/')]:
        path = SOURCES / 'board-kernel' / name
        kernel[name] = read(path)
        assert blob(path) == tree[where + name], name
    # The board kernel configuration of orangepi-build a95129bf
    # (external/config/kernel/linux-sun60iw2-current-a733.config).
    config_path = SOURCES / 'board-kernel-config/linux-sun60iw2-current-a733.config.a95129bf'
    config = read(config_path)
    assert inputs[str(config_path)] == 'ae50f80a9f46cf06adfd79aa5c754809fc9c5f59d3ded9863ebe0dc783b1e3dc'
    assert 'CONFIG_AW_STANDARD_CCU=y' in config.splitlines() and 'CONFIG_AW_CPUFREQ_DT=y' in config.splitlines()
    driver = read(ROOT / 'drivers/clock/a733_cpu_clock.c')
    vf = pinned['npu-platform-sun60iw2p1-cpu-vf-dtsi']

    # Operating points: every 26 MHz row of the three tables, exactly.
    assert 'vf-version = "V0.9"' in vf
    for c, label in [('little', 'cluster0_opp_table:'), ('big', 'cluster1_opp_table:'), ('dsu', 'dsu_opp_table:')]:
        assert c_rows(driver, c) == dtsi_rows(vf, label), c
    # Bin map: vf_mapping_table code -> column; codes without a column are refused.
    pairs = re.findall(r'0x([0-9a-f]+)\s+0x([0-9a-f]{4})', re.search(r'table = <(.*?)>;', vf, re.S)[1])
    codes = [int(v, 16) for v in re.search(r'codes\[\]=\{([^}]*)\}', driver)[1].split(',')]
    for code, column in ((int(a, 16), 'vf' + b) for a, b in pairs):
        if column not in BINS:
            assert code not in codes and code in (0x11, 0x12)
        elif code == 0:
            assert column == 'vf0000'  # Driver uses the envelope instead; see docs/cpu-clock.md.
        else:
            assert codes.index(code) + 1 == BINS.index(column)
    assert len(codes) == len(set(codes)) == 8
    # SID: dvfs word 0x0300624c, bak (bits23:16) over ori (bits15:8); no dvfs2 keys.
    dtsi = pinned['board-gpu-power-sun60iw2p1-dtsi']
    sid = node(dtsi, 'sid: sid@3006000')
    assert 'reg = <0x0 0x03006000' in sid and 'dvfs2' not in sid
    assert re.search(r'dvfs_ori \{\s*offset = <0x24C>;\s*mask = <0xff>;\s*shift = <8>;', sid)
    assert re.search(r'dvfs_bak \{\s*offset = <0x24C>;\s*mask = <0xff>;\s*shift = <16>;', sid)
    sidc = pinned['thermal-sunxi-sid-c']
    assert '*dvfs = dvfs_bak ? dvfs_bak : dvfs_ori;' in sidc and 'if (!sunxi_sid_from_dvfs2(dvfs))' in sidc
    assert '#define SID_DVFS UINT32_C(0x0300624c)' in driver
    # CPU PLL fields, 26 MHz VCO ranges, parents and wait bounds.
    pll = kernel['ccu-sun60iw2-cpupll.c']
    for c, reg, nmax in [('l', '0x1000', 84), ('b', '0x2000', 86), ('dsu', '0x3000', 86)]:
        block = re.search(r'static struct ccu_nkmp pll_cpu_' + c + r'_clk = \{(.*?)\n\};', pll, re.S)[1]
        for term in ['.lock\t\t= BIT(28)', '.lock_enable\t= BIT(29)', '.ldo_en\t\t= BIT(30)',
                     '.enable\t\t= BIT(31)', '.output\t\t= BIT(27)', '_SUNXI_CCU_MULT_OFFSET_MIN_MAX(8, 8, 0',
                     '.p\t\t= _SUNXI_CCU_DIV(16, 2)', '.clear\t\t= BIT(26)', '.reg\t\t= ' + reg,
                     '.p_reg\t\t= ' + hex(int(reg, 16) + 0x1c)]:
            assert term in block, (c, term)
        assert re.search(r'pll_cpu_' + c + r'_clk\.n\.max = ' + str(nmax) + r';\s*/\* 2\.26G/26M', pll)
        assert re.search(r'pll_cpu_' + c + r'_clk\.n\.min = 26;\s*/\* 676M/26M', pll)
        assert re.search(r'cpu_' + c + r'_parents\[\] = \{ "dcxo", "dcxo", "dcxo", "pll-cpu-' + c + '"', pll)
        assert re.search(r'cpu_' + c + r'_clk, "cpu_' + c + r'", cpu_' + c + r'_parents,\s*' + hex(int(reg, 16) + 0x1c) + r', 24, 3', pll)
    assert 'reg & lock, 100, 70000' in pll and '!(reg & clear), 100, 10000' in pll
    assert '#define UPDATE_US 10000u' in driver and '#define LOCK_US 70000u' in driver
    # Pattern and LFM words the inspection reads, and the enables the pinned probe sets.
    for c, base in [('L', 0x1000), ('B', 0x2000)]:
        for name, off in [('PAT0', 0x4), ('PAT1', 0x8), ('LFM', 0x18)]:
            assert re.search(r'#define SUN60IW2_PLL_CPU_%s_%s_REG\s+0x%04x\b' % (c, name, base + off), pll), (c, name)
    for term in ['val |= BIT(29) | BIT(30);  /* ues Triangular(3bit) */', 'val |= BIT(31);  /* enable lfm_en */',
                 'val |= BIT(31) | BIT(8);  /* 1.125MHz/us */']:
        assert term in pll, term
    for define in ['#define PAT0 0x4u', '#define PAT1 0x8u', '#define LFM 0x18u',
                   '#define PAT0_TYPE (BIT(29)|BIT(30))', '#define PAT1_LFM BIT(31)', '#define LFM_ON BIT(31)']:
        assert define in driver, define
    assert 'const unsigned nmax=c->cluster==A733_CPU_LITTLE ? 84u : 86u;' in driver
    nkmp = kernel['ccu_nkmp.c']
    assert 'rate = ccu_nkmp_calc_rate(parent_rate, n, k, m, 1 << p);' in nkmp  # P is a power of two.
    assert 'new_p = ilog2(_nkmp.p);' in nkmp
    for table, nmax in [('little', 84), ('big', 86)]:
        for mhz, _ in c_rows(driver, table):
            p = 1 if mhz < 676 else 0
            n = (mhz << p) // 26
            assert linux_best(mhz * 1000000, 26, nmax) == (mhz * 1000000, n, 1 << p), (table, mhz)
    # PMIC: DCDC3/DCDC5 selectors, enables and step ranges; board supplies and ramp.
    axp, regs = pinned['board-pmic-driver'], pinned['board-pmic-registers']
    for rail, reg, bit in [('dcdc3', 0x14, 2), ('dcdc5', 0x16, 4)]:
        entry = re.search(r'\{ "' + rail + r'",[^}]+\}', axp)[0]
        assert '500, 1540, AXP8191_DC%sOUT_VOL, 0x7f, 10, 1200, 20' % rail[-1] in entry
        assert 'AXP8191_DCDC_POWER_ON_OFF_CTL1, %d' % bit in entry
        assert re.search(r'AXP8191_DC%sOUT_VOL\s+\((0x[0-9A-F]+)\)' % rail[-1], regs)[1] == '0x%X' % reg
    assert re.search(r'AXP8191_DCDC_POWER_ON_OFF_CTL1\s+\(0x10\)', regs)
    pmic = read(ROOT / 'drivers/power/a733_pmic.c')
    assert 'case A733_RAIL_DCDC3:*p=(struct profile){0x10,2,0x14,127};' in pmic
    assert 'case A733_RAIL_DCDC5:*p=(struct profile){0x10,4,0x16,127};' in pmic
    dts = pinned['board-gpu-power-sun60i-a733-orangepi-zero3w-dts']
    for ref, rail in [('&cpu0', 'reg_dcdc5'), ('&dsufreq', 'reg_dcdc5'), ('&cpu6', 'reg_dcdc3')]:
        assert re.search(re.escape(ref) + r' \{\s*(cpu|dsu)-supply = <&' + rail + '>;', dts), ref
    for rail in ['dcdc3', 'dcdc5']:
        assert 'regulator-ramp-delay = <250>;' in node(dts, 'reg_' + rail + ': ' + rail)
    assert '#define RAMP_UV_US 250u' in driver
    # Voltage settle: |dV| / ramp rounded up to whole milliseconds.
    regulator = kernel['axp2101-regulator.c']
    assert 'delay = (DIV_ROUND_UP(abs(new_uV - old_uV), ramp_delay) + 999) / 1000;' in regulator
    assert 'return delay * 1000;' in regulator and '#define RAMP_ROUND_US 1000u' in driver
    # Lock confirmation of the standard CCU helper: three lock reads 3 us apart, then 20 us.
    common = kernel['ccu_common.c']
    helper = common[common.index('#if IS_ENABLED(CONFIG_AW_STANDARD_CCU)'):]
    for term in ['while (read_times != 100 && lock_times != 3) {', 'udelay(3);', 'if (lock_times == 3)\n\t\tudelay(20);']:
        assert term in helper, term
    for define in ['#define LOCK_READS 3u', '#define LOCK_GAP_US 3u', '#define LOCK_SETTLE_US 20u', '#define SETTLE_US 20u']:
        assert define in driver, define
    # DSU rule: three quarters of the faster cluster, rounded down to the
    # 26 MHz step, at most the DSU point at CPU-L's voltage, and no voltage
    # write of its own.
    dsu = kernel['sunxi-dsufreq.c']
    for term in ['dsu_next_freq = (max_next_freq * 3)/4;', 'dsu_next_freq -= (dsu_next_freq % DCXO_CLK_26M);',
                 'return min(dsu_next_freq, dsu_max_freq);', 'if (volt == dsufreq_dev->opp[idx - i].volt_uv) {',
                 'ret = clk_set_rate(clk, freq);']:
        assert term in dsu, term
    assert 'regulator_set' not in dsu
    assert 'cap=fast*3u/4u;' in driver and 'cap-=cap%26000000u;' in driver
    # The throttle ceiling needs 800 mV in every column of both clusters.
    header = read(ROOT / 'include/a733_cpu_clock.h')
    assert '#define A733_CPU_CLOCK_THROTTLE_MHZ 1014u' in header
    for table in ['little', 'big']:
        assert dict(c_rows(driver, table))[1014] == [800] * len(BINS), table
    assert '#define A733_CPU_CLOCK_DSU_THROTTLE_MHZ 780u' in header
    assert dict(c_rows(driver, 'dsu'))[780] == [800] * len(BINS)
    # Sensor channels in the thermal header's physical order: CPUB, DDR, NPU, CPUL, GPU.
    assert 'Physical channel order: CPUB, DDR, NPU, CPUL, GPU.' in read(ROOT / 'include/a733_thermal.h')
    assert 'static const unsigned sensor[3]={3,0,3}; /* Physical channels CPUL, CPUB and CPUL. */' in driver

    # Transition model with sanitizers, then AArch64 compilation of changed units.
    for path in UNITS + ['checks/cpu-clock-model.c', 'checks/a733-pmic.c']:
        read(ROOT / path)
    for path in sorted((ROOT / 'include').glob('*.h')):
        read(path)
    common = [CLANG, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Iinclude']
    model = out / 'cpu-clock-model'
    run('host-build', [*common, '-O1', '-g', *SANITIZE, '-Wl,--wrap=a733_rail_floor',
                      'checks/cpu-clock-model.c', *UNITS, '-o', model])
    result = run('model', [model])
    for unit in CHANGED:
        read(ROOT / unit)
        run('native-' + Path(unit).stem, [*common, '--target=aarch64-none-elf', '-march=armv8-a',
            '-mstrict-align', '-ffreestanding', '-fno-builtin', '-O2', '-c', unit,
            '-o', out / (Path(unit).stem + '.o')])
    native = out / 'a733_cpu_cycles_native.o'
    cycles = cycles_model(native)
    (out / 'results.json').write_text(json.dumps({
        'status': 'PASS', 'inputs': inputs, 'model': result.strip(),
        'model_sha256': hashlib.sha256(model.read_bytes()).hexdigest(),
        'cycles_native_sha256': hashlib.sha256(native.read_bytes()).hexdigest(), 'cycles_cases': cycles,
        'limits': 'Host and instruction models only. No physical clock, voltage, settling, lock time, '
                  'PMU access or frequency measurement.'},
        indent=2) + '\n')
    print('PASS: tables, bin map, SID, PLL fields, Linux N/P choice, PMIC profile, board rails; ' + result.strip() +
          '; native cycle code: %d instruction-model cases' % len(cycles))


if __name__ == '__main__':
    main()
