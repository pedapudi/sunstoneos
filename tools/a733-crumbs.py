#!/usr/bin/env python3
"""Decode returning-image breadcrumbs and arm or read WDT0 over FEL.

The layout, NPU stage names and GPU/NPU first-failure group names come from
include/sunstone_crumb.h, so the image and decoder use one definition.
No device access happens here:
callers pass the FEL transaction function they already own.
"""
import hashlib
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'include/sunstone_crumb.h'
RTC = 0x07090000
WORDS = {'tag': 0x100, 'last': 0x104, 'time': 0x10c, 'fault': 0x110, 'esr': 0x114, 'elr': 0x118, 'far': 0x11c}
WDT_MODE, WDT_CONFIG, WDT_OUTPUT, WDT_RESTART = 0x02050014, 0x02050010, 0x02050018, 0x0205000c
KEY = 0x16aa0000
RETURN_ROM = 0x0503  # SUNSTONE_CRUMB_RETURN_ROM: value is the WDT0 mode after the disarm
EL3_STOP = 0x0111    # SUNSTONE_CRUMB_EL3_STOP: value is min(status, 15) << 12 | detail & 0xfff
FAIL_LEVEL = 0xb     # SUNSTONE_CRUMB_FAIL_LEVEL: the first board failure, from EL0 or EL1
BOUNDED = 0x736      # cpu-return bounded(): its value carries board_live_refusal() bits
# The entry guard's rail refusal (apps/os/cpu-return-entry.c), whose detail is
# DCDC5 << 10 | DCDC3, each in 10 mV units.
GUARD_RAIL_LINE = next((i for i, line in enumerate(
    (Path(__file__).resolve().parents[1] / 'apps/os/cpu-return-entry.c').read_text().splitlines(), 1)
    if 'return REFUSE((c->rail_uv[0]/' in line), None)
# board_live_refusal() bits (apps/board/main.c) and bounded()'s margin bit.
LIVE_REFUSAL = ((28, 'board.fatal already set'), (29, 'service window (live_until) expired'),
                (30, 'session deadline passed'), (31, 'within 10 ms of the deadline'))
# Watchdog interval for mode bits 7:4 (drivers/watchdog/a733_watchdog.c).
INTERVALS = {0: 0.5, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 8, 8: 10, 9: 12, 10: 14, 11: 16}


def header():
    """Word offsets, magic values and stage names from the C header."""
    text = HEADER.read_text()
    defines = {m[1]: int(m[2], 16) for m in re.finditer(r'#define SUNSTONE_CRUMB_(\w+) (0x[0-9a-f]+)u?\b', text)}
    stages = {int(m[2], 16): m[1] for m in re.finditer(r'SUNSTONE_CRUMB_(\w+) = (0x[0-9a-f]+),', text)}
    return defines, stages


def interval(mode):
    return INTERVALS[(mode >> 4) & 15]


def arm_arguments(mode):
    """One FEL invocation: clear the crumb words (never 0x108, the FEL flag), then
    the measured WDT0 order: reset output, system reset, mode, restart."""
    args = []
    for offset in WORDS.values():
        args += ['writel', hex(RTC + offset), '0x0']
    for address, value in ((WDT_OUTPUT, KEY | 2), (WDT_CONFIG, KEY | 1), (WDT_MODE, KEY | mode),
                           (WDT_RESTART, 0x14af)):
        args += ['writel', hex(address), hex(value)]
    return args


def return_arguments():
    """One FEL invocation on the returned instance: mode, disarm, mode again, crumbs."""
    args = ['readl', hex(WDT_MODE), 'writel', hex(WDT_MODE), hex(KEY), 'readl', hex(WDT_MODE)]
    for offset in WORDS.values():
        args += ['readl', hex(RTC + offset)]
    return args


def parse_return(stdout):
    values = [int(x, 16) for x in stdout.split()]
    if len(values) != 2 + len(WORDS):
        raise ValueError('Unexpected watchdog return readback.')
    return values[0], values[1], dict(zip(WORDS, values[2:]))


def bundle_image(bundle, image_sha256):
    """Select the validated bundle's ELF for caller decoding.

    CPU development bundles keep it under board/. Finite NPU bundles keep
    it at the root. Only the image matching validated metadata is eligible;
    a present CPU-layout image retains precedence even if its hash differs.
    """
    bundle = Path(bundle)
    cpu = bundle / 'board/image.elf'
    image = cpu if cpu.exists() else bundle / 'image.elf'
    if not image.is_file():
        return None
    if hashlib.sha256(image.read_bytes()).hexdigest() != image_sha256:
        return None
    return image


def symbolize(elf, address):
    """Source location of a code address in the image ELF, or None."""
    if elf is None or not Path(elf).exists():
        return None
    for tool in ('llvm-symbolizer-21', 'llvm-symbolizer'):
        path = shutil.which(tool)
        if path:
            result = subprocess.run([path, '--obj', str(elf), '--inlining', '--functions=linkage', hex(address)],
                                    capture_output=True, text=True, timeout=30)
            lines = [x for x in result.stdout.splitlines() if x.strip()]
            if result.returncode == 0 and lines and not lines[0].startswith('??'):
                return ' <- '.join(f'{lines[i]} {lines[i + 1]}' for i in range(0, len(lines) - 1, 2))
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        return None
    with open(elf, 'rb') as f:
        table = ELFFile(f).get_section_by_name('.symtab')
        best = None
        for sym in table.iter_symbols() if table else ():
            v, n = sym['st_value'], sym['st_size']
            if sym['st_info']['type'] == 'STT_FUNC' and v <= address < v + max(n, 1):
                best = f"{sym.name}+{address - v:#x}"
        return best


def finite_npu_failure_image(elf):
    """Frozen finite NPU images used group 8 before CPU retention used it.

    Require the finite return producers, rather than a general NPU service:
    a mixed CPU/NPU OS image can legitimately emit CPU retention failures.
    The caller supplies the validated launched ELF, not the current source tree.
    """
    if elf is None:
        return False
    try:
        from elftools.elf.elffile import ELFFile
        from elftools.common.exceptions import ELFError
    except ImportError:
        return False
    try:
        with open(elf, 'rb') as stream:
            table = ELFFile(stream).get_section_by_name('.symtab')
            names = {s.name for s in table.iter_symbols()
                     if s['st_shndx'] != 'SHN_UNDEF'} if table else set()
        return {'npu_finite_receipt', 'npu_finite_rom_return'} <= names
    except (OSError, ELFError):
        return False


def board_failure(words, defines, elf):
    """Decode a level 0xb record: the first board_fail and its site."""
    fault, site, caller, value = words['fault'], words['esr'], words['elr'], words['far']
    groups = {v: k[len('FAIL_'):].lower() for k, v in defines.items()
              if k.startswith('FAIL_') and k not in ('FAIL_SVC', 'FAIL_LEVEL', 'FAIL_RETURN')}
    group = (site >> 12) & 15
    legacy_npu = group == 8 and finite_npu_failure_image(elf)
    if legacy_npu:
        groups[group] = 'npu'
    slot = (fault >> 8) & 255
    record = {'core': (fault >> 20) & 15, 'level': FAIL_LEVEL, 'group': groups.get(group, f'unknown {group}'),
              'site': site & 0xfff, 'aux': site >> 16 & 0x7fff, 'caller': hex(caller), 'value': hex(value),
              'caller_kind': 'return address' if site >> 31 else 'statement',
              'exception_level': 'EL0' if slot == 0 else 'EL1' if slot in (4, 5) else f'SPSR.M {slot:#x}'}
    if legacy_npu:
        record['group_source'] = 'finite NPU ELF (legacy group 8)'
    record['site_kind'] = 'detail' if record['group'] in ('cpu_return', 'application') else 'source line'
    if record['group'] == 'npu' and site & defines['NPU_REGISTER']:
        record['register_offset'] = hex((record['aux'] & 0x3fff) << 2)
    elif record['group'] == 'npu' and site & defines['NPU_STARTUP']:
        record['startup_stage'] = record['aux'] & 0xff
    if record['group'] == 'cpu_return' and record['site'] == BOUNDED:
        record['deadline_seconds'] = record['aux']
        record['refusal'] = [text for bit, text in LIVE_REFUSAL if value >> bit & 1]
        record['counter_ms'] = value & 0x0fffffff
    if record['group'] == 'cpu_clock' and not site & defines['CPU_AUX']:
        # An earlier image: the value packs phase, cluster and 16 status bits.
        record.update(phase=value >> 24 & 0xff, cluster=value >> 16 & 0xff, status=hex(value & 0xffff),
                      status_bits=16)
    elif record['group'] == 'cpu_clock':
        record.update(phase=record['aux'] >> 8 & 3, cluster=record['aux'] & 0xff, status=hex(value), status_bits=32)
        if site & defines['CPU_GUARD']:
            # The entry guard's refusal word: detail << 12 | guard line.
            record['guard_line'], record['guard_detail'] = value & 0xfff, hex(value >> 12)
            if GUARD_RAIL_LINE is not None and record['guard_line'] == GUARD_RAIL_LINE:
                # Matched against this tree's guard source; an image of
                # another revision can number its lines differently.
                record['dcdc5_mv'], record['dcdc3_mv'] = 10 * (value >> 22 & 0x3ff), 10 * (value >> 12 & 0x3ff)
                record['rail_line_source'] = 'apps/os/cpu-return-entry.c of the decoding tree'
    if record['group'] == 'cpu_pll':
        aux=record['aux']
        record.update(expected_n=aux&255, domain=aux>>8&3, driver_state=aux>>10&15,
                      closing=bool(aux>>14&1), pll=hex(value), observed_n=value>>8&255,
                      update=bool(value>>26&1), locked=bool(value>>28&1))
    if record['group'] == 'cpu_retained':
        aux = record['aux']
        state, domain, origin = aux & 15, aux >> 4 & 3, aux >> 6 & 3
        phase, cursor, prior = aux >> 8 & 3, aux >> 10 & 3, aux >> 12 & 7
        states = ('IDLE', 'CLOCK', 'RAIL', 'INSPECT', 'READ', 'VOLTAGE', 'WRITTEN',
                  'SETTLE', 'PARK', 'UPDATE', 'LOCK', 'UNPARK', 'RELEASE')
        record.update(setter='fault_at', driver_state=state,
                      driver_state_name=states[state] if state < len(states) else 'UNKNOWN',
                      domain=domain, domain_name=('CPU-L', 'CPU-B', 'DSU', 'INVALID')[domain],
                      origin=origin, origin_name=('USER', 'LOWER', 'RESTORE', 'INVALID')[origin],
                      phase=phase, phase_name=('START', 'INSPECT', 'REQUEST', 'FOLLOW')[phase],
                      request_cursor=cursor, prior_status=prior,
                      prior_status_source='driver.result', prior_status_name=('READY', 'PENDING', 'COMPLETE', 'BUSY', 'REFUSED',
                                         'INVALID', 'EXPIRED', 'RETAINED')[prior],
                      provider_status=None if value & 0xff == 0xff else value & 0xff,
                      target_mhz=value >> 8 & 0x7ff, inspection=bool(value >> 19 & 1),
                      closing=bool(value >> 20 & 1), counter_failed=bool(value >> 21 & 1),
                      clocks_failed=bool(value >> 22 & 1), ledger_failed=bool(value >> 23 & 1),
                      pmic_failed=bool(value >> 24 & 1), clock_held=bool(value >> 25 & 1),
                      rail_held=bool(value >> 26 & 1), request_held=bool(value >> 27 & 1),
                      deadline_expired=bool(value >> 28 & 1), timer_expired=bool(value >> 29 & 1),
                      parked=bool(value >> 30 & 1), critical=bool(value >> 31 & 1))
    if caller:
        # A statement address is exact; a return address points after the call.
        record['caller_source'] = symbolize(elf, caller - 4 if site >> 31 else caller)
    return record


def decode(words, epoch, elf=None):
    """Summarize the seven words; a tag of another session yields no stages.
    elf, the image ELF, symbolizes a board failure's caller."""
    defines, stages = header()
    def stage(word):
        number = word >> 16
        return {'stage': number, 'name': stages.get(number, 'UNKNOWN'), 'value': word & 0xffff}
    result = {'words': {k: hex(v) for k, v in words.items()}}
    tag = words['tag']
    result['session'] = (tag >> 16 == defines['TAG_MAGIC'] and (tag & 0xffff) == (epoch & 0xffff))
    if not result['session']:
        result['summary'] = ('no breadcrumb of this session: the image stopped before its EL3 entry '
                             'or did not write RTC words')
        return result
    last, time, fault = words['last'], words['time'], words['fault']
    if last:
        result['last'] = stage(last)
        result['last']['core'] = time >> 29
        result['last']['seconds_since_power_on'] = round((time & 0x1fffffff) * 1024 / 24e6, 4)
        if result['last']['stage'] == 0x0204:
            value = result['last']['value']
            step = value >> 12
            result['last']['clock_return'] = {
                'step': {1: 'drain', 2: 'lower-request', 3: 'lower-wait',
                         4: 'verify', 5: 'done'}.get(step, 'unknown'),
                'owner_phase': value >> 10 & 3,
                'owner': ('START', 'INSPECT', 'REQUEST', 'FOLLOW')[value >> 10 & 3],
                'cluster': value >> 8 & 3, 'driver_state': value & 255}
        if result['last']['stage'] in (0x0410, 0x0411, 0x0412, 0x0413):
            result['last']['gpu_job'] = {
                'client': 1 if result['last']['stage'] in (0x0412, 0x0413) else 0,
                'boundary': 'submit' if result['last']['stage'] in (0x0410, 0x0412) else 'complete',
                'sequence_low16': result['last']['value']}
        if result['last']['stage'] in (0x0450, 0x0451, 0x0452, 0x0453):
            result['last']['gpu_recovery'] = {
                'phase': {0x0450: 'stop', 0x0451: 'reset', 0x0452: 'restart', 0x0453: 'ready'}[
                    result['last']['stage']],
                'failed_serial_low16': result['last']['value']}
        if result['last']['stage'] == EL3_STOP:
            v = result['last']['value']
            result['last']['status'] = v >> 12 if v >> 12 < 15 else '15 or more'
            result['last']['detail'] = hex(v & 0xfff)
    if fault >> 24 == defines['FAULT_MAGIC'] and (fault >> 16) & 15 == FAIL_LEVEL:
        result['board_failure'] = board_failure(words, defines, elf)
    elif fault >> 24 == defines['FAULT_MAGIC']:
        level = (fault >> 16) & 15
        result['fault'] = {'core': (fault >> 20) & 15, 'level': level, 'vector_slot': (fault >> 8) & 255,
                           'x0': fault & 255, 'esr' if level == 3 or fault & 255 == 4 else 'detail': hex(words['esr']),
                           'elr': hex(words['elr']), 'far': hex(words['far'])}
    parts = []
    if 'last' in result:
        r = result['last']
        parts.append(f"last stage {r['name']} ({r['stage']:#06x}) value {r['value']:#x} on core {r['core']}")
    else:
        parts.append('tagged at EL3 entry; no later stage')
    if result.get('last', {}).get('clock_return'):
        r = result['last']['clock_return']
        parts.append('CPU clock return ' + r['step'] + ', owner %s, cluster %d, driver state %d' %
                     (r['owner'], r['cluster'], r['driver_state']))
    if 'board_failure' in result:
        f = result['board_failure']
        text = (f"first board failure on core {f['core']} at {f['exception_level']}: {f['group']} "
                f"{f['site_kind']} {f['site']:#x}, value {f['value']}, caller {f['caller']}")
        if f.get('caller_source'):
            text += f" ({f['caller_source']})"
        if 'register_offset' in f:
            text += f"; register offset {f['register_offset']}"
        if 'startup_stage' in f:
            text += f"; startup stage {f['startup_stage']}"
        if f['group'] == 'cpu_retained':
            text += (f"; setter {f['setter']}, state {f['driver_state_name']}, domain {f['domain_name']}, "
                     f"origin {f['origin_name']}, target {f['target_mhz']} MHz, phase {f['phase_name']}, "
                     f"request cursor {f['request_cursor']}, prior {f['prior_status_name']}, "
                     f"provider status {f['provider_status']}, closing {f['closing']}, "
                     f"pre-retention faults counter={f['counter_failed']} clock={f['clocks_failed']} "
                     f"ledger={f['ledger_failed']} PMIC={f['pmic_failed']}; "
                     f"leases clock={f['clock_held']} rail={f['rail_held']} request={f['request_held']}; "
                     f"expired deadline={f['deadline_expired']} timer={f['timer_expired']}")
        if 'refusal' in f:
            text += '; ' + (', '.join(f['refusal']) or 'no refusal bit')
        parts.append(text)
    if 'fault' in result:
        f = result['fault']
        kind = 'EL1 fault' if f['level'] == 1 and f['x0'] == 4 else (
            f"exit status {f['x0']}" if f['level'] == 1 else 'EL3 exception')
        parts.append(f"first {kind} on core {f['core']}: " +
                     ', '.join(f'{k} {f[k]}' for k in ('esr', 'detail', 'elr', 'far') if k in f))
    result['summary'] = '; '.join(parts)
    return result
