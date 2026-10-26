"""Validate the return-deadline workload profiles and decode a deadline return.

Both profiles run the complete returning workload. CPU0 also returns to the
ROM at a fixed deadline, or at its first stop of any other kind, with a
record of its state (docs/designs/workload-bisect.md). A completed run is
validated as the workload. A deadline return is a failed attempt whose
secondaries may still run: the runner reads the record and the owned
writable memory, restores nothing and keeps the board for power removal.
"""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('task_workload_host', ROOT/'tools/all-core-task-workload-host.py')
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
require, digest = base.require, base.digest
PROFILES = {False: 'all-core-task-workload-deadline-v1', True: 'all-core-task-workload-quiet-deadline-v1'}
SOFTWARE_SCHEMA = 'a733-workload-bisect-software-v1'
HOST_SOURCES = base.HOST_SOURCES+('tools/workload-bisect-host.py', 'tools/secondary-identity-host.py')
MODEL_SOURCES = base.MODEL_SOURCES+('checks/workload-bisect-probe.py',)
CASES = ('complete-return', 'transport-failure-retained', 'invalid-report-restored',
         'deadline-return-retained', 'damaged-deadline-retained', 'deadline-quarantine-failure-held',
         'deadline-record-checked',
         'changed-input-refused')
RESTORE_CASES = ('restore-rwp', 'restore-rwp-secondary', 'restore-rwp-late', 'restore-rwp-late-secondary',
             'restore-pwrr', 'restore-pwrr-secondary')
INSTRUCTION_CASES = {False: ('success', 'stalled-core', 'stalled-no-tick', *RESTORE_CASES),
                     True: ('success', *RESTORE_CASES)}
builder = base.base.base.cache.local.identity.builder
RECORD = builder.DEADLINE_RECORD
RECORD_OFFSET = RECORD[0]-0x42018000
RECORD_MAGIC = builder.DEADLINE_RECORD_MAGIC
DEADLINE_TICKS = builder.DEADLINE_TICKS
# Owned writable interval read after a deadline return: data, BSS, task
# stacks, kernel state, the workload results and CPU0's EL1 record page.
SNAPSHOT = ('owned-writable-after', 0x41200000, 0x91000)
REASONS = {1: 'the CPU0 deadline interrupt',
           2: 'a CPU0 exit SMC before completion',
           3: 'a failed survey at EL3',
           4: 'a refused CPU0 completion SMC'}
FIELDS = ('magic', 'version', 'reason', 'stage', 'counter', 'deadline', 'elr_el3', 'spsr_el3', 'esr_el3',
          'x0', 'x1', 'elr_el1', 'spsr_el1', 'esr_el1', 'reserved_112', 'sp_el0', 'sp_el1', 'x29', 'x30',
          'fiq_intid', 'cntp_ctl', 'cntp_cval', 'reserved_176', 'reserved_184', 'unused', 'cleaned_lines', 'cleaned_counter',
          'survey_state', 'survey_completed')


class DeadlineRetained(ValueError):
    """Raised by the runner after a deadline return has been read and the
    instance quarantined: the outcome is known, so the runner exits instead of
    holding the board lock, and the queue claim stays held for power removal."""


class DeadlineReturn(ValueError):
    """CPU0 returned through the deadline unit; record holds the decoded fields.

    Raised for every report that carries the record's magic number, including
    a damaged or foreign one: the secondaries may still run from the owned
    memory, so the runner must never restore DRAM after such a report."""
    def __init__(self, record):
        self.record = record
        reason = REASONS.get(record.get('reason'), 'an unknown reason {}'.format(record.get('reason')))
        text = 'CPU0 returned through the deadline unit after {}; stage {:#x}.'.format(reason, record.get('stage', 0))
        if record.get('cpu0_wait') == 'identity_park':
            text += (' CPU0 was waiting in identity_park after its completion barrier timed out'
                     ' (ELR_EL3 {:#x}).'.format(record['elr_el3']))
        elif record.get('cpu0_wait') == 'elsewhere':
            text += (' CPU0 was interrupted outside identity_park, at ELR_EL3 {:#x}; it may be wedged'
                     ' there.'.format(record['elr_el3']))
        if record.get('inconsistent'):
            text += ' The record is inconsistent: '+record['inconsistent']
        super().__init__(text)


def check_record(raw, nonce, record, words):
    base.base.base.cache.local.identity.primary.probe.decode(raw[:256], nonce)
    require(record['version'] == 1 and record['reason'] in REASONS and record['returned'] == 1 and
            record['stage'] in ((3,) if record['reason'] in (1, 2) else (1,)) and
            not words[24] and not words[61] and not words[63],
            'Deadline record is inconsistent.')
    require(record['reason'] != 1 or record['fiq_intid'] == 29, 'Deadline interrupt differs.')
    require(record['stage'] != 3 or record['cleaned_lines'] == 9280, 'Deadline cleaning incomplete.')
    require(record['reason'] == 3 or record['deadline'] == record['survey_deadline']+DEADLINE_TICKS,
            'Deadline differs from the startup contract.')


def deadline_record(raw, nonce, park=None):
    """Return the decoded deadline record of a returned report, or None.

    A record whose checks fail is still returned, with the failed check in
    'inconsistent'. ESR_EL3 is meaningful only for an SMC (reasons 2 and 4).
    PARK is the overlay's identity_park address: for reason 1, 'cpu0_wait'
    says whether the deadline interrupt found CPU0 there (a timed-out
    barrier, interrupts masked) or elsewhere (possibly wedged)."""
    if len(raw) != 0x5200 or struct.unpack_from('<Q', raw, RECORD_OFFSET)[0] != RECORD_MAGIC:
        return None
    words = struct.unpack_from('<64Q', raw, RECORD_OFFSET)
    record = dict(zip(FIELDS, words))
    record['cores'] = [dict(zip(('pending', 'active', 'enabled', 'power'), words[29+4*c:33+4*c])) for c in range(8)]
    record['returned'] = words[62]
    record['epoch'], record['survey_deadline'] = struct.unpack_from('<2Q', raw, 0x100+24)
    record['cpu0_wait'] = None
    if park is not None and record['reason'] == 1:
        # identity_park is WFI; B identity_park.
        record['cpu0_wait'] = 'identity_park' if park <= record['elr_el3'] < park+8 else 'elsewhere'
    try:
        check_record(raw, nonce, record, words)
        record['inconsistent'] = None
    except ValueError as error:
        record['inconsistent'] = str(error)
    return record


# The top-operating-point profile (docs/designs/cpu-top-opp.md, step 6): CPU0's
# relock record in identity header words 16 to 28 (boot/a733/top-opp-relock.h),
# at this offset of the combined report, which starts 0x100 before the header.
TOP_OPP_OFFSET, TOP_OPP_WORDS, TOP_OPP_MAGIC = 0x180, 13, 0x3130505054504f54
TOP_OPP_FINAL = ((0xf8804d00, 0x03000000), (0xf8804500, 0x03000000), (0xf8803100, 0x03000105))
TOP_OPP_DOMAINS = ('cpu_b', 'cpu_l', 'dsu')
TOP_OPP_SCOPE = {'record': {'address': 0x42018180, 'words': 13},
                 'targets': {'cpu_b': {'pll': 0x08872000, 'n': 77, 'mhz': 2002},
                             'cpu_l': {'pll': 0x08871000, 'n': 69, 'mhz': 1794},
                             'dsu': {'pll': 0x08873000, 'n': 49, 'mhz': 1274}},
                 'voltages': 'DCDC3 1050 mV and DCDC5 1000 mV, set by bundle 25 and read back by bundle 15 after a ROM '
                             'return in the same lifetime; the overlay reads no PMIC register',
                 'pending': 'Instruction cases, host validation and root review required before physical use.'}


def top_opp_record(raw):
    """The relock record: magic, result, start and end counters, entry and
    final PLL << 32 | mux words, and park-to-select ticks << 32 | lock reads,
    each in the order CPU-B, CPU-L, DSU."""
    words = struct.unpack_from('<13Q', raw, TOP_OPP_OFFSET)
    return {'magic': words[0], 'result': words[1], 'start': words[2], 'end': words[3],
            'entry': [[w >> 32, w & 0xffffffff] for w in words[4:7]],
            'final': [[w >> 32, w & 0xffffffff] for w in words[7:10]],
            'parked_ticks': [w >> 32 for w in words[10:13]], 'lock_reads': [w & 0xffffffff for w in words[10:13]]}


def decode(raw, nonce, manifest):
    record = deadline_record(raw, nonce, manifest['hooks']['identity_park'])
    if record:
        # The saved report keeps the relock record (top_opp_record) for review.
        raise DeadlineReturn(record)
    relock = top_opp_record(raw) if 'top_opp' in manifest else None
    if relock:
        require(relock['magic'] == TOP_OPP_MAGIC and relock['result'] == 1 and relock['start'] < relock['end'] and
                relock['final'] == [list(f) for f in TOP_OPP_FINAL] and
                all(n >= 3 for n in relock['lock_reads']) and all(t < 24000 for t in relock['parked_ticks']),
                'Top-operating-point relock record differs.')
        # The identity header's other reserved words stay checked as zero.
        raw = raw[:TOP_OPP_OFFSET]+bytes(8*TOP_OPP_WORDS)+raw[TOP_OPP_OFFSET+8*TOP_OPP_WORDS:]
    result = base.decode(raw, nonce, manifest)
    result.update(deadline_disarmed=True, quiet_background=manifest['workload_bisect']['quiet_background'])
    if relock:
        result['top_opp'] = relock
    return result


probe = SimpleNamespace(NATIVE=base.probe.NATIVE, REPORT=base.probe.REPORT, decode=decode)
initial_report = base.initial_report


def profile(manifest):
    return PROFILES[manifest['workload_bisect']['quiet_background']]


def load(build, image, manifest):
    item = manifest.get('workload_bisect') or {}
    quiet = item.get('quiet_background')
    require(quiet in (False, True) and item == {
        'profile': PROFILES[quiet], 'quiet_background': quiet,
        'deadline_ticks_after_startup': DEADLINE_TICKS, 'deadline_interrupt': 29,
        'record': {'address': RECORD[0], 'bytes': RECORD[1]},
        'pending': 'Instruction cases, host validation and root review required before physical use.'},
        'Return-deadline profile scope changed.')
    top_opp = 'top_opp' in manifest
    if top_opp:
        require(not quiet and manifest['top_opp'] == dict(TOP_OPP_SCOPE, relock=manifest['hooks'].get('top_opp_relock')) and
                manifest['hooks'].get('top_opp_relock') == 0x41020800, 'Top-operating-point profile scope changed.')
    return base.load(build, image, manifest, return_deadline=True, quiet=quiet, top_opp=top_opp)


def validate_software(software, manifest_hash):
    require(software.get('schema') == SOFTWARE_SCHEMA and software.get('physical_access') is False and
            software.get('manifest_sha256') == manifest_hash and
            software.get('host_sources') == {n: digest(ROOT/n) for n in HOST_SOURCES} and
            software.get('host_cases') == dict.fromkeys(CASES, True), 'Return-deadline host evidence differs.')
    quiet = software.get('quiet_background')
    require(quiet in (False, True), 'Profile variant missing.')
    evidence = software['instruction_evidence']
    require(tuple(sorted(evidence)) == tuple(sorted(INSTRUCTION_CASES[quiet])),
            'Return-deadline instruction cases incomplete.')
    for case, item in evidence.items():
        path = Path(item['path'])
        require(path.is_absolute() and digest(path) == item['sha256'], 'Instruction evidence changed.')
        result = json.loads(path.read_text())
        require(result.get('physical_access') is False and result.get('pass') is True and
                result.get('case') == case and result.get('profile') == PROFILES[quiet] and
                result.get('native_sha256') == probe.NATIVE and result.get('manifest_sha256') == manifest_hash and
                result.get('sources') == {n: digest(ROOT/n) for n in MODEL_SOURCES},
                'Return-deadline instruction inputs or result differ.')
        report = path.with_suffix('.bin')
        require(digest(report) == result['report_sha256'], 'Instruction report changed.')
        raw = report.read_bytes()
        require(not case.startswith('restore-') or result.get('injected_reads') == 1,
                'The restoration case did not inject its status read.')
        if case == 'success' or case in RESTORE_CASES:
            require(result.get('off') == list(range(1, 8)) and result.get('clean_complete') == list(range(8)) and
                    not result.get('fiq_taken'), 'Completed workload or disarmed deadline missing.')
            actual = [list(struct.unpack_from('<5Q', raw, 0x4280+c*128)) for c in range(8)]
            require(result.get('workload_results') == actual and
                    all(r[0] == 32 and r[1] and r[2] and not r[3] for r in actual) and actual[0][4] >= 32,
                    'Completed workload results differ.')
        else:
            record = deadline_record(raw, raw[32:48], result.get('identity_park'))
            parked = record and record['cpu0_wait'] == 'identity_park'
            # stalled-core ends through the deadline interrupt too when CPU0 was
            # waiting in identity_park (see checks/workload-bisect-probe.py); the
            # record's ELR_EL3 and the model must agree on where CPU0 waited.
            require(record and not record['inconsistent'] and
                    record['reason'] == (1 if case == 'stalled-no-tick' or parked else 2) and
                    (record['reason'] != 1 or parked == bool(result.get('cpu0_parked'))) and
                    record['stage'] == 3 and result.get('off') == [], 'Deadline return case differs.')
