"""Board state and pinned rail-helper validation used by the board runner."""


import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


CACHE = Path.home() / '.cache/sunstoneos'


EVIDENCE = CACHE / 'evidence'


FEL = CACHE / 'build/sunxi-a733/sunxi-fel'


CONTROL_IMAGE = CACHE / 'build/mimus-split-1031/control-hello/sunstone-a733-os-hello.elf'


RECORDS = EVIDENCE / 'wdt-breadcrumbs-1031/control/offline-proof/prepared/board/session-template.bin'


RAIL_HELPER = EVIDENCE / 'rails-both-1031/helper/probe.bin'


RAIL_HELPER_SHA256 = '84af1ec5f1a095b30a2c29d836f90aac77d4252a376895fee8bc5560b858e38c'


RAIL_COMPLETE = 'COMPLETE board bundle 33: DCDC3 and DCDC5 restored to 800 mV at boot clock words'


WORDS = (0x2002000, 0x20020a0, 0x20020a8, 0x20020ac, 0x2002518, 0x2002fc0, 0x2002fe4, 0x2003908,
         0x2003988, 0x701000c, 0x7010010, 0x701019c, 0x701020c, 0x3006244, 0x3006248, 0x300624c,
         *(0x8871000 + 0x1000 * d + o for d in range(3) for o in (0, 4, 8, 0x18, 0x1c)), 0x7090160,
         # CE clock (CE_CLK_REG) and bus gating/reset (CE_BGR_REG: CE_RST bit 16, CE_SYS_RST bit 17, gates bits 0 and 1).
         # CCU reads are always safe; the CE's own registers are not read here, because a gated or reset-held CE can
         # stall the bus. They say whether anything before the image left the CE running, which could leave DMA active.
         0x2002ac0, 0x2002ac4,
         # The PF pin bank (the microSD signalling pins) as the boot chain and the runner's reset, DRAM and rail steps leave it, read just
         # before launch: GPIO_POW_MOD_SEL (withstand mode, PF bits 11:10), GPIO_POW_VAL (detected level, PF bits 11:10) and the PF supply
         # selector (bit 0). The SD open records what it found too (docs/sd-card.md, the PF bank's withstand mode); this places the value
         # before the OS. PIO registers are always clocked, so the reads cannot stall.
         0x2000040, 0x2000048, 0x2000070)


BOOT_PLL, BOOT_MUX = (0xf8802600, 0xf8802700, 0xf8802600), (0x03010000, 0x03000000, 0x03010105)


DOMAINS = ('CPU-L', 'CPU-B', 'DSU')


def speed_bin(sid_word):
    """The CPU clock driver's column for a SID DVFS word: 1-8, 9 for code 0, None if refused."""
    text = (ROOT / 'drivers/clock/a733_cpu_clock.c').read_text()
    codes = [int(v, 16) for v in re.search(r'codes\[\]=\{([^}]*)\}', text)[1].split(',')]
    code = (sid_word >> 16) & 255 or (sid_word >> 8) & 255
    return 9 if not code else codes.index(code) + 1 if code in codes else None


def state_check(words):
    """The image's own initial-state predicates on FEL-readable words.

    Returns (findings, derived thermal record fields). An empty findings list
    means the readable part of the native guards admits this board state.
    The PMIC rails (DCDC3 and DCDC5 must read 800 mV) are behind the PMIC
    controller and are validated by the pinned rail helper."""
    w = {int(k, 16): v for k, v in words.items()}
    findings = []
    osc, ref = w[0x7090160], w[0x2002000]
    dcxo = (24000000, 19200000, 26000000, 24000000)[(osc >> 14) & 3]
    n, m = ((ref >> 8) & 255) + 1, ((ref >> 16) & 127) + 1
    if not ((osc & 0x10000002) == 0x10000002 and ref & 0xf8000000 == 0xf8000000 and
            80 <= n <= 116 and dcxo * n == 24000000 * m):
        findings.append('normalized 24 MHz PLL_REF invalid (thermal clocks, CPU guard oscillator bits)')
    if dcxo != 26000000:
        findings.append('DCXO is not 26 MHz (CPU entry guard)')
    apb1 = w[0x2002518] & 0x0300001f
    parent, div = apb1 >> 24, (apb1 & 31) + 1
    rate = {0: 24000000, 3: 600000000}.get(parent)
    if not rate or rate % div:
        findings.append(f'APB1 {apb1:#x} is not SYS24M or PERI0/600 MHz with an exact divider (thermal clocks)')
    if parent == 3 and not (w[0x20020a8] >> 31 == 0 and w[0x20020ac] & 0x8e000000 == 0 and
                            w[0x2003908] & 0x0a000a00 == 0x0a000a00 and w[0x2003988] & 0x0a000000 == 0x0a000000):
        findings.append('PERI0 pattern, LFM or output gates differ (thermal clocks with PERI0 APB1)')
    gpadc = w[0x2002fc0] & 0x8700001f
    if gpadc == 0x80000000 or (dcxo == 24000000 and gpadc == 0x81000000):
        configure = 0   # adopt the enabled normalized clock
    elif not gpadc >> 31:
        configure = 1   # the thermal owner configures the disabled clock
    else:
        configure = None
        findings.append(f'GPADC24M {w[0x2002fc0]:#x} is enabled with another source or divider (thermal clocks)')
    for d, name in enumerate(DOMAINS):
        base = 0x8871000 + 0x1000 * d
        if w[base] != BOOT_PLL[d] or w[base + 0x1c] != BOOT_MUX[d]:
            findings.append(f'{name} PLL/mux {w[base]:#x}/{w[base + 0x1c]:#x} differ from the boot point '
                            f'{BOOT_PLL[d]:#x}/{BOOT_MUX[d]:#x} (CPU entry guard)')
        if w[base + 4] & (3 << 29) or w[base + 8] >> 31 or w[base + 0x18] >> 31:
            findings.append(f'{name} pattern or LFM enabled (CPU entry guard)')
    if speed_bin(w[0x300624c]) != 4:
        findings.append(f'SID DVFS word {w[0x300624c]:#x} does not select speed bin 4 (CPU entry guard)')
    derived = {'apb1': apb1, 'apb1_hz': rate // div if rate and not rate % div else None,
               'configure_adc_clock': configure}
    return findings, derived


def boot_clocks(words):
    """The CPU domains whose PLL or mux word differs from the boot point."""
    w = {int(k, 16): v for k, v in words.items()}
    return [name for d, name in enumerate(DOMAINS)
            if w[0x8871000 + 0x1000 * d] != BOOT_PLL[d] or w[0x887101c + 0x1000 * d] != BOOT_MUX[d]]


RAIL_NAMES = {2: 'DCDC3', 4: 'DCDC5'}


RAIL_STAGES = ('IDLE', 'READ', 'WRITE', 'SETTLE', 'FINAL', 'RELEASE')


def rail_line(line):
    """A RAIL line with its rail and stage named, for example rail=DCDC3 stage=WRITE."""
    def name(token):
        key, _, value = token.partition('=')
        table = {'rail': RAIL_NAMES, 'stage': dict(enumerate(RAIL_STAGES))}.get(key)
        return f'{key}={table[int(value)]}' if table and value.isdigit() and int(value) in table else token
    return ' '.join(name(t) for t in line.split())


BOOT_ROWS = tuple((name, BOOT_PLL[d], BOOT_MUX[d], (494, 1014, 494)[d]) for d, name in enumerate(DOMAINS))


def helper_report(record, log):
    """The helper's SUPPLY, VOLT, RAIL and outcome lines, and whether it returned normally with
    both rails at 800 mV: a VOLT line for each rail above 800 mV at entry, DCDC3 first,
    reading selector 0x1e (800 mV), and all three domains at their boot words before and after."""
    lines = log.splitlines()
    supply = [x for x in lines if x.startswith('SUPPLY ')]
    volt = [x for x in lines if x.startswith('VOLT ')]
    outcome = [x for x in lines if x.startswith(('COMPLETE ', 'REFUSED ', 'STOPPED ', 'FAIL '))]
    # A failure inside a rail change prints the rail, the helper's stage and
    # the provider's last selector bytes.
    rail = [rail_line(x) for x in lines if x.startswith('RAIL ')]
    report = {'supply': supply, 'volt': volt, 'outcome': outcome, 'rail_state': rail, 'complete': False}
    def fields(line):
        return {k: int(v, 0) for k, v in (x.split('=', 1) for x in line.split()[1:])}
    result = record.get('result') or {}
    if isinstance(record.get('controlled'), dict):
        name = record['controlled']['outcome']['name']
        report['reason'] = f"controlled outcome {name}" + (', no rail written' if name.startswith('refused') else '')
        return report
    if not (record.get('complete') is True and record.get('transport_failed') is False and
            record.get('mode') == 'kernel' and 'controlled' not in record and result.get('magic') == 0x534b4552 and
            result.get('stage') == 5 and result.get('status') == 0 and outcome == [RAIL_COMPLETE]):
        report['reason'] = 'helper did not return normally'
        return report
    if len(supply) != 1:
        report['reason'] = 'entry SUPPLY line absent or repeated'
        return report
    entry = fields(supply[0])
    expected = [rail for rail in ('dcdc3', 'dcdc5') if entry.get(rail + '_uv', 0) != 800000]
    if entry.get('bin') != 4 or entry.get('dcxo_hz') != 26000000:
        report['reason'] = 'speed bin or DCXO differs'
        return report
    written = [fields(x) for x in volt]
    if [next(iter(f), '')[:5] for f in written] != expected or not all(
            f[r + '_uv'] == 800000 and f[r + '_selector'] & 127 == 30 for r, f in zip(expected, written)):
        report['reason'] = 'VOLT lines differ from one 800 mV readback per raised rail, DCDC3 first'
        return report
    for name, pll, mux, mhz in BOOT_ROWS:
        for phase in ('before', 'after'):
            rows = [x for x in lines if x.startswith(f'{name} {phase} ')]
            f = fields(rows[0][len(name):]) if len(rows) == 1 else {}
            if not (f.get('pll') == pll and f.get('mux') == mux and f.get('mhz') == mhz and
                    not (f['pat0'] & (3 << 29) or f['pat1'] >> 31 or f['lfm'] >> 31)):
                report['reason'] = f'{name} {phase} inspection absent or not at the boot point'
                return report
    report.update(complete=True, entry_dcdc5_uv=entry['dcdc5_uv'], entry_dcdc3_uv=entry['dcdc3_uv'],
                  written=expected, dcdc3_uv=800000, dcdc5_uv=800000)
    return report
