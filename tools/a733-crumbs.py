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
FAIL_LEVEL_SECOND = 0xc  # SUNSTONE_CRUMB_FAIL_LEVEL_SECOND: a device failure that followed an application failure
CUTOFF = 0x73c       # cpu-return fail_cutoff(): the session reached the supervisor's cutoff without a return request
NPU_RETIREMENT_FAILED = 0x75f  # application ss_finish: the NPU retirement wait gave up before the cutoff
# OS_NPU_STATE_* of apps/os/npu.h, bit by bit (os_npu_retirement_state).
NPU_RETIREMENT_STATE = ('report not published', 'report not stopped', 'report retains a failure', 'os_npu_stopped false',
                        'SDK closed', 'local stop done', 'active', 'started', 'opened', 'failed', 'runtime failed',
                        'irq pending', 'irq stopped', 'worker parked', 'NPU service linked')
BOUNDED = 0x736      # cpu-return bounded(): its value carries board_live_refusal() bits
USB0_REASONS = {int(value): name.lower().replace('_', ' ') for name, value in re.findall(
    r'^#define USB0_STOP_(\w+) (\d+)u$',
    (ROOT / 'include/sunstone_usb0_stop.h').read_text(), re.M)}
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


REFUSAL_HEADER = ROOT / 'include/a733_admission_refusal.h'


def refusal_terms():
    """({term number: name}, {term number: {sub number: name}}) from include/a733_admission_refusal.h.

    EL3 admission stops with a733_smp_stop(1, 0x100 | term << 4 | sub); the image and this decoder share the table."""
    text = REFUSAL_HEADER.read_text()
    terms = {int(m[2]): m[1].lower() for m in re.finditer(r'A733_REFUSAL_([A-Z_]+) = (\d+),', text)}
    family = {'TIMING': 5, 'DECODE': 6, 'BIND': 7, 'STALE': 8, 'MAGIC': 2, 'FIELD': 9}
    subs = {number: {} for number in terms}
    for m in re.finditer(r'A733_(TIMING|DECODE|BIND|STALE|MAGIC|FIELD)_([A-Z_]+) = (\d+)', text):
        subs[family[m[1]]][int(m[3])] = m[2].lower()
    return terms, subs


def refusal_detail(detail):
    """Name an EL3 admission stop detail: {'detail', 'term', 'sub', 'meaning'}, or None for another stop."""
    if not 0x100 <= detail <= 0x1ff:
        return None
    terms, subs = refusal_terms()
    term, sub = detail >> 4 & 15, detail & 15
    named = {'detail': hex(detail), 'term': terms.get(term, 'unknown term %d' % term)}
    if sub:
        named['sub'] = subs.get(term, {}).get(sub, 'unknown sub-term %d' % sub)
    named['meaning'] = named['term'] + (' (%s)' % named['sub'] if 'sub' in named else '')
    return named


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


# Words that tell a cold power-on from a warm reset (docs/designs/card-fel-recovery.md):
# the first-stage gate's record in RTC GP2, the cold-boot marker (RTC IC_CHARA_REG,
# A733 manual 9.1.7.20), the DCXO control word with the reset level in bit 13
# (9.1.7.16) and WDT0's configuration and output. Never cleared with WORDS.
RESET_WORDS = {'flag': RTC + 0x108, 'marker': RTC + 0x1f0, 'xo_ctrl0': RTC + 0x160,
               'wdt_config': WDT_CONFIG, 'wdt_output': WDT_OUTPUT}
MARKER, MARKER_KEYED, MARKER_SET = RTC + 0x1f0, 0x16aa16aa, 0x16aa
# The keyed write that would clear bits 15:0, used only by the runner's approved --marker-clear-test.
MARKER_CLEAR = 0x16aa0000
FEL_REQUEST, GATE_STAGE = 0x5aa5a55a, 0x0c02
GATE_FLAGS = {'cold': 0x8000, 'warm': 0x4000, 'unmarked': 0x2000, 'once': 0x1000}  # include/a733_card_fel_gate.h
# GP2 value that makes the card gate take its cold path once with the marker set (include/a733_card_fel_gate.h).
BOOT_ONCE = 0xb0070c02


# Read at every first contact, in the same transaction as RESET_WORDS and before any write: all eight RTC
# general-purpose words (tag, LAST, GP2, time, fault, ESR, ELR, FAR) and WDT0's interrupt status. RTC words survive
# watchdog resets and short power losses, so after a run that never returned they show where it stopped.
CONTACT_WORDS = {'rtc_%03x' % offset: RTC + offset for offset in range(0x100, 0x120, 4)}
CONTACT_WORDS['wdt_status'] = 0x02050004


def contact_arguments():
    """FEL reads of RESET_WORDS then CONTACT_WORDS; no write."""
    args = reset_arguments()
    for address in CONTACT_WORDS.values():
        args += ['readl', hex(address)]
    return args


def parse_contact(stdout):
    values = [int(x, 16) for x in stdout.split()]
    if len(values) != len(RESET_WORDS) + len(CONTACT_WORDS):
        raise ValueError('Unexpected first-contact readback.')
    return dict(zip(RESET_WORDS, values)), dict(zip(CONTACT_WORDS, values[len(RESET_WORDS):]))


def reset_arguments():
    args = []
    for address in RESET_WORDS.values():
        args += ['readl', hex(address)]
    return args


def gate_record(flag):
    """Decode the first-stage gate's record in GP2, or None for another value."""
    if flag >> 16 != GATE_STAGE:
        return None
    record = {name: bool(flag & bit) for name, bit in GATE_FLAGS.items()}
    record.update(refused_step=flag >> 8 & 0xf, mode=flag & 0xff)
    return record


def effective_mode(before, record):
    """WDT0's mode at the reset: the production gate disarms WDT0 on a warm
    entry and records the mode it read, so a zero readback defers to it."""
    if record and record['warm'] and before == 0:
        return record['mode']
    return before


def return_arguments():
    """One FEL invocation on the returned instance: mode, disarm, mode again, crumbs, reset words."""
    args = ['readl', hex(WDT_MODE), 'writel', hex(WDT_MODE), hex(KEY), 'readl', hex(WDT_MODE)]
    for offset in WORDS.values():
        args += ['readl', hex(RTC + offset)]
    return args + reset_arguments()


def parse_return(stdout):
    values = [int(x, 16) for x in stdout.split()]
    if len(values) != 2 + len(WORDS) + len(RESET_WORDS):
        raise ValueError('Unexpected watchdog return readback.')
    crumbs = values[2:2 + len(WORDS)]
    return values[0], values[1], dict(zip(WORDS, crumbs)), dict(zip(RESET_WORDS, values[2 + len(WORDS):]))


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
              if k.startswith('FAIL_') and k not in ('FAIL_SVC', 'FAIL_LEVEL', 'FAIL_LEVEL_SECOND', 'FAIL_RETURN')}
    group = (site >> 12) & 15
    legacy_npu = group == 8 and finite_npu_failure_image(elf)
    if legacy_npu:
        groups[group] = 'npu'
    slot = (fault >> 8) & 255
    record = {'core': (fault >> 20) & 15, 'level': FAIL_LEVEL, 'group': groups.get(group, f'unknown {group}'),
              'site': site & 0xfff, 'aux': site >> 16 & 0x7fff, 'caller': hex(caller), 'value': hex(value),
              'caller_kind': 'return address' if site >> 31 else 'statement',
              'exception_level': 'EL0' if slot == 0 else 'EL1' if slot in (4, 5) else f'SPSR.M {slot:#x}'}
    if record['group'] == 'quiesce':
        owners = {v: k[len('QUIESCE_'):].lower().removesuffix('_owner')
                  for k, v in defines.items() if k.startswith('QUIESCE_')}
        record['owner'] = owners.get(site >> 16 & 255, 'unknown')
        record['source_group'] = groups.get(site >> 24 & 15, 'unknown')
        record['code'] = site & 0xfff
        storage_phases = ('unverified', 'fat1', 'fat2', 'directory', 'fsinfo',
                          'dirty1', 'dirty2', 'clean1', 'clean2', 'raw', 'failed', 'protocol')
        if record['owner'] == 'storage' and 0x921 <= record['code'] <= 0x92c:
            record['session'] = value
            record['storage_phase'] = storage_phases[record['code'] - 0x921]
    if legacy_npu:
        record['group_source'] = 'finite NPU ELF (legacy group 8)'
    record['site_kind'] = ('code' if record['group'] == 'quiesce' else
                           'detail' if record['group'] in ('cpu_return', 'application', 'usb0') else 'source line')
    if record['group'] == 'usb0':
        record['usb0_guard'] = USB0_REASONS.get(record['site'], 'unknown guard')
        record['provider_step'] = record['aux'] & 255
        record['rail_index'] = record['aux'] >> 8
        record['provider_result'] = value if value < 0x80000000 else value - (1 << 32)
    if record['group'] == 'npu' and site & defines['NPU_REGISTER']:
        record['register_offset'] = hex((record['aux'] & 0x3fff) << 2)
    elif record['group'] == 'npu' and site & defines['NPU_STARTUP']:
        record['startup_stage'] = record['aux'] & 0xff
    elif record['group'] == 'npu' and site & defines['NPU_MEASUREMENT']:
        job=site >> 16 & 255
        record.update(measurement_stage=23+(site >> 24 & 7),
                      job_index=None if job==255 else job,
                      observed_word=hex(value),
                      observed_word_index=int(bool(site & defines['NPU_MEASUREMENT_HIGH_WORD'])))
    elif record['group'] == 'npu' and site & defines.get('NPU_REQUEST', 0):
        operations=('STATUS','OPEN','SUBMIT','CANCEL','REAP','CLOSE','WAIT','LOAD')
        record.update(request_operation=operations[site >> 24 & 7],
                      expected_ticket_low8=site >> 16 & 255,
                      observed_word=hex(value),ticket_bits=8)
    elif record['group']=='npu' and not site&defines.get('NPU_REQUEST',0) and site&defines['NPU_CANCEL']:
        sample=site>>23&3
        record.update(cancel_sample=None if sample==3 else sample,ticket_low_bits=site>>16&127,ticket_bits=7,
                      observed_word=hex(value),observed_word_index=int(bool(site&defines['NPU_CANCEL_HIGH_WORD'])))
    elif record['group']=='npu' and not site&defines.get('NPU_REQUEST',0) and site&defines['NPU_OUTPUT']:
        record.update(ticket_low_bits=site>>16&511,ticket_bits=9)
        if value&defines['NPU_OUTPUT_LENGTH']:record['output_bytes']=value&0x7fffffff
        else:record.update(output_byte_index=value>>8,output_byte=value&255)
    if record['group'] == 'thermal' and value >> 31:
        # apps/os/thermal.c critical_reading(): the reading behind a CRITICAL stop.
        record['thermal_reading'] = {'channel': value >> 24 & 7, 'celsius': (value & 0xffffff) / 1000,
                                     'stale': bool(value >> 30 & 1)}
    if record['group'] == 'thermal' and value >> 29 == 1:
        # apps/os/thermal.c lost_liveness(): the thermal owner left liveness; which terms of live() failed.
        names = ('board_live', 'application_deadline', 'clock_provider_failed', 'thermal_failed', 'reading_deadline_passed',
                 'register_lock_retained')
        record['thermal_liveness_lost'] = [n for i, n in enumerate(names) if value >> i & 1]
        record['board_live_refusal_bits'] = value >> 8 & 7
    if record['group'] == 'cpu_return' and record['site'] == CUTOFF:
        # value: low 32 bits of the return address of the application's latest ss_progress call (its waiting loop);
        # aux: the NPU retirement state, 15 bits, zero in an image without the NPU service.
        record['application_progress_site'] = hex(value)
        record['npu_retirement_state'] = [t for i, t in enumerate(NPU_RETIREMENT_STATE) if record['aux'] >> i & 1]
    if record['group'] == 'application' and record['site'] == NPU_RETIREMENT_FAILED:
        record['npu_retirement_state'] = [t for i, t in enumerate(NPU_RETIREMENT_STATE) if value >> i & 1]
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
        if result['last']['stage'] == 0x0c02:
            result['last']['name'] = 'CARD_FEL_GATE'
            value = result['last']['value']
            result['last']['gate_progress'] = {
                1: 'entry', 2: 'mode checked', 3: 'watchdog set to the profile mode',
                4: 'request cleared', 5: 'before ROM branch',
                0x81: 'refused local cache/MMU state', 0x82: 'refused ROM redirect',
                0x83: 'refused watchdog mode/configuration/output',
                0x84: 'request clear readback failed', 0x86: 'watchdog readback differs from the profile mode',
            }.get(value, 'unknown gate progress')
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
            d = v & 0xfff
            # the supervisor's stops of a return (apps/os/cpu-return.c, cpu-return-step.h)
            named = {0x736: 'bounded(): the session deadline passed', 0x744: 'bounded(): board.fatal is set (no NPU record)',
                     0x741: 'devices not ready in a normal return', 0x742: 'the failure return took longer than 3 s',
                     0x73c: 'no return request before the cutoff'}.get(d)
            if d & 0xc00 == 0x800:
                named = 'bounded(): board.fatal is set; the NPU service\'s first failure is at source line %d of apps/os/npu.c' % (d & 0x3ff)
            if named:
                result['last']['return_stop'] = named
            if v >> 12 == 1:
                legacy = {1: 'unnamed admission refusal (an image built before the refusal names)',
                          2: 'watchdog or reset-redirect state differs from the admitted profile'}
                named = refusal_detail(v & 0xfff)
                if named:
                    result['last']['admission_refusal'] = named
                elif v & 0xfff in legacy:
                    result['last']['admission_refusal'] = {'detail': hex(v & 0xfff), 'meaning': legacy[v & 0xfff]}
    if fault >> 24 == defines['FAULT_MAGIC'] and (fault >> 16) & 15 in (FAIL_LEVEL, FAIL_LEVEL_SECOND):
        result['board_failure'] = board_failure(words, defines, elf)
        if (fault >> 16) & 15 == FAIL_LEVEL_SECOND:
            result['board_failure']['after_application_failure'] = True
    elif fault >> 24 == defines['FAULT_MAGIC']:
        level = (fault >> 16) & 15
        result['fault'] = {'core': (fault >> 20) & 15, 'level': level, 'vector_slot': (fault >> 8) & 255,
                           'x0': fault & 255, 'esr' if level == 3 or fault & 255 == 4 else 'detail': hex(words['esr']),
                           'elr': hex(words['elr']), 'far': hex(words['far'])}
    if fault >> 24 != defines['FAULT_MAGIC'] and words['elr'] == defines['PROBE_POLICY_MAGIC']:
        inherited, normalized = words['esr'], words['far']
        if not (inherited | normalized) & ~0xfe:
            result['probe_policy'] = {
                'inherited_dynamic_mask': inherited, 'normalized_mask': normalized,
                'inherited_policy': {str(c): 0x100 if inherited & (1 << c) else 0 for c in range(1, 8)},
                'normalized_policy': {str(c): 0 for c in range(1, 8) if normalized & (1 << c)}}
    parts = []
    if 'last' in result:
        r = result['last']
        parts.append(f"last stage {r['name']} ({r['stage']:#06x}) value {r['value']:#x} on core {r['core']}")
        if 'gate_progress' in r:
            parts.append('gate '+r['gate_progress'])
    else:
        parts.append('tagged at EL3 entry; no later stage')
    if result.get('last', {}).get('admission_refusal'):
        parts.append('EL3 admission refused the session: ' + result['last']['admission_refusal']['meaning'])
    if result.get('last', {}).get('clock_return'):
        r = result['last']['clock_return']
        parts.append('CPU clock return ' + r['step'] + ', owner %s, cluster %d, driver state %d' %
                     (r['owner'], r['cluster'], r['driver_state']))
    if 'board_failure' in result:
        f = result['board_failure']
        text = (f"{'device failure after an application failure' if f.get('after_application_failure') else 'first board failure'} on core {f['core']} at {f['exception_level']}: {f['group']} "
                f"{f['site_kind']} {f['site']:#x}, value {f['value']}, caller {f['caller']}")
        if f.get('caller_source'):
            text += f" ({f['caller_source']})"
        if f['group'] == 'quiesce':
            text += f", owner {f['owner']}, source group {f['source_group']}"
        if 'session' in f:
            text += f", SESSION {f['session']:#x}, phase {f['storage_phase']}"
        if 'usb0_guard' in f:
            text += (f"; USB0 guard {f['usb0_guard']}, provider step {f['provider_step']}, "
                     f"rail index {f['rail_index']}, result {f['provider_result']}")
        if 'register_offset' in f:
            text += f"; register offset {f['register_offset']}"
        if 'startup_stage' in f:
            text += f"; startup stage {f['startup_stage']}"
        if 'measurement_stage' in f:
            text += (f"; NPU measurement stage {f['measurement_stage']}, "
                     f"job {f['job_index'] if f['job_index'] is not None else 'global'}, "
                     f"observed {'high' if f['observed_word_index'] else 'low'} word {f['observed_word']}")
        if 'cancel_sample' in f:
            text += (f"; cancel sample {f['cancel_sample'] if f['cancel_sample'] is not None else 'unavailable'}, "
                     f"ticket low {f['ticket_bits']} bits {f['ticket_low_bits']}, "
                     f"observed {'high' if f['observed_word_index'] else 'low'} word {f['observed_word']}")
        if 'output_byte_index' in f:text += f"; output byte {f['output_byte_index']} was {f['output_byte']:#x}; ticket low {f['ticket_bits']} bits {f['ticket_low_bits']}"
        if 'output_bytes' in f:text += f"; output bytes {f['output_bytes']}; ticket low {f['ticket_bits']} bits {f['ticket_low_bits']}"
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
