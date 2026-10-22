#!/usr/bin/env python3
"""Host side of the returning memory qualification image.

prepare   choose the next run's contract from the session run log, run the
          image's own admission (apps/memtest/plan.c, compiled for the host)
          and write the upload bundle: image, contract and a zero record page.
verify    check a returned result record (magic, checksum, sequence and
          contract digest) and print its decoded fields as JSON, with the
          blocks to exclude after an abort.

execute() performs one run through the transaction and wait_return callbacks
of tools/a733-returning-host.py. Physical admission and the root-owned claim
belong to the coordinator; no command here opens a USB device.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import secrets
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
K = 0xa5c35a3c00000000
DRAM_BASE = 0x40000000
BLOCK_SHIFT = 21
FIRST_BLOCK, END_BLOCK = 32, 6144
COUNTER_HZ = 24000000
BUDGET_LIMIT = 20 * COUNTER_HZ
IMAGE_ADDRESS = 0x41000000
RESULT_ADDRESS, RESULT_BYTES = 0x42000000, 16384
RECORD_ADDRESS, RECORD_BYTES = 0x42003000, 4096
CONTRACT_MAGIC = 0x314e4f4354534d4d
RECORD_MAGIC = 0x3144524f4345524d
WRITERS_MAGIC = 0x57525452
HASH_SEED, HASH_PRIME = 0xcbf29ce484222325, 0x100000001b3
MASK = {'data': 1, 'address': 2, 'signature': 4, 'march': 8, 'harness': 16, 'dma-cache': 32, 'dma-reach': 64}
REFUSALS = ['admitted', 'contract', 'mask', 'mask-unbuilt', 'slice-empty', 'slice-outside', 'slice-excluded',
            'excluded-count', 'excluded-empty', 'excluded-order', 'excluded-outside', 'fixed-outside',
            'fixed-excluded', 'grouping', 'seed', 'budget', 'dczid', 'dcache', 'writers-unsampled', 'writers',
            'mapping', 'record']
TESTS = ['scan', 'data', 'address', 'signature', 'march', 'harness', 'dma-cache', 'dma-reach']
RESULTS = ['not-run', 'pass', 'fail', 'insensitive', 'stopped']
RUN_STATUS = ['running', 'pass', 'fail', 'refused', 'stopped']
STOPS = ['none', 'scan', 'decode', 'digest', 'budget']
# Refusals that depend on the CPU, the monitor or the live translation tables;
# only the image can decide them.
IMAGE_ONLY = {'dczid', 'dcache', 'writers-unsampled', 'writers', 'mapping', 'record'}
# Residuals the user accepts, in the run log, before any run: a write by
# OR100, CE or USB0 during a run; a denied access that stalls the bus with no
# watchdog; a partial-page or multi-stuck-bit alias that the scan misses and a
# later write sends into the monitor.
RESIDUALS = ('remote-writers-or100-ce-usb0', 'bus-stall', 'partial-page-alias')
# Stops after which the recovery pages or the address decode are in doubt.
REVIEW_STOPS = {'scan', 'decode', 'digest'}
# Run statuses a verified record can carry; any other entry has no trusted record.
TRUSTED_STATUS = {'pass', 'fail', 'refused', 'stopped'}


class Range(ctypes.Structure):
    _fields_ = [('start', ctypes.c_uint32), ('end', ctypes.c_uint32)]


class Contract(ctypes.Structure):
    _fields_ = [('magic', ctypes.c_uint64), ('sequence', ctypes.c_uint64), ('slice_start', ctypes.c_uint32),
                ('slice_end', ctypes.c_uint32), ('mask', ctypes.c_uint32), ('excluded_count', ctypes.c_uint32),
                ('budget_ticks', ctypes.c_uint64), ('seed', ctypes.c_uint64), ('excluded', Range * 10)]


class Facts(ctypes.Structure):
    _fields_ = [('dczid', ctypes.c_uint64), ('dcache_bytes', ctypes.c_uint64), ('writers_sample', ctypes.c_uint64)]


class Extent(ctypes.Structure):
    _fields_ = [('address', ctypes.c_uint64), ('bytes', ctypes.c_uint64)]


class Geometry(ctypes.Structure):
    _fields_ = [('first_block', ctypes.c_uint32), ('end_block', ctypes.c_uint32), ('piece_blocks', ctypes.c_uint32),
                ('others_blocks', ctypes.c_uint32), ('fixed', Extent * 4)]


class Piece(ctypes.Structure):
    _fields_ = [('first', ctypes.c_uint32), ('blocks', ctypes.c_uint32)]


class Group(ctypes.Structure):
    _fields_ = [('first_piece', ctypes.c_uint32), ('pieces', ctypes.c_uint32)]


class Plan(ctypes.Structure):
    _fields_ = [('pieces', ctypes.c_uint32), ('groups', ctypes.c_uint32), ('budget_ticks', ctypes.c_uint64),
                ('piece', Piece * 64), ('group', Group * 32)]


class Result(ctypes.Structure):
    _fields_ = [('status', ctypes.c_uint32), ('reserved', ctypes.c_uint32), ('words', ctypes.c_uint64),
                ('ticks', ctypes.c_uint64)]


class Mismatch(ctypes.Structure):
    _fields_ = [('address', ctypes.c_uint64), ('expected', ctypes.c_uint64), ('observed', ctypes.c_uint64),
                ('test', ctypes.c_uint32), ('element', ctypes.c_uint32)]


class Sample(ctypes.Structure):
    _fields_ = [('bytes', ctypes.c_uint64), ('ticks', ctypes.c_uint64 * 3)]


class ChasePoint(ctypes.Structure):
    _fields_ = [('bytes', ctypes.c_uint64), ('loads', ctypes.c_uint64), ('ticks', ctypes.c_uint64)]


_RECORD_WORDS = ['magic', 'version', 'writers_sample', 'sequence', 'contract_digest', 'status', 'refusal', 'stop',
                 'phase', 'progress', 'resume', 'remote_writers', 'daif', 'dcache_bytes', 'dczid', 'address_tested',
                 'address_failed', 'scan_page', 'scan_value', 'digest_admission', 'digest_last', 'groups',
                 'slowest_per_mib', 'mismatch_count', 'unconfirmed', 'renewals', 'budget_ticks', 'start_ticks',
                 'end_ticks', 'dma_phase']


class Record(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in _RECORD_WORDS] + [
        ('dma_polls', ctypes.c_uint64 * 8), ('clock', ctypes.c_uint32 * 8), ('test', Result * 8),
        ('sample', Sample * 4), ('chase', ChasePoint * 8), ('mismatch', Mismatch * 32), ('checksum', ctypes.c_uint64)]


assert ctypes.sizeof(Contract) == 128 and ctypes.sizeof(Record) <= RECORD_BYTES


def board_geometry():
    return Geometry(FIRST_BLOCK, END_BLOCK, 128, 128, (Extent * 4)(
        Extent(0x44000000, 64), Extent(0x4c000000, 1 << 26), Extent(0x100000000, 192 << 20),
        Extent(0x120000000, 256 << 20)))


def library(directory):
    """Compile the image's admission and planner for the host and load it."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    output = directory / 'libmemtest-plan.so'
    subprocess.run(['clang-21', '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', f'-I{ROOT}/apps/memtest',
                    f'-I{ROOT}/include', str(ROOT / 'apps/memtest/plan.c'), '-o', str(output)], check=True)
    lib = ctypes.CDLL(str(output))
    lib.memtest_admit.restype = ctypes.c_int
    lib.memtest_admit.argtypes = [ctypes.POINTER(Geometry), ctypes.POINTER(Contract), ctypes.POINTER(Facts),
                                  ctypes.POINTER(Plan)]
    lib.memtest_dcache_bytes.restype = ctypes.c_uint64
    lib.memtest_dcache_bytes.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint]
    return lib


def admit(lib, contract, facts=None, geometry=None):
    """Return (refusal name, plan) from the image's own admission function."""
    facts = facts or Facts(4, 1, WRITERS_MAGIC << 32)
    plan = Plan()
    code = lib.memtest_admit(ctypes.byref(geometry or board_geometry()), ctypes.byref(contract),
                             ctypes.byref(facts), ctypes.byref(plan))
    return REFUSALS[code], plan


def make_contract(sequence, mask, seed, budget_ticks, slice_blocks=(0, 0), excluded=()):
    c = Contract(CONTRACT_MAGIC, sequence, slice_blocks[0], slice_blocks[1], mask, len(excluded), budget_ticks, seed)
    for i, (start, end) in enumerate(excluded):
        c.excluded[i] = Range(start, end)
    return c


def fnv(words, value=HASH_SEED):
    for word in words:
        value = ((value ^ word) * HASH_PRIME) & (2**64 - 1)
    return value


def near_k(value):
    """Bits 34-63 of value, or of its complement, lie within one of K's."""
    def close(v):
        return ((K >> 34) - (v >> 34) + 1) & ((1 << 30) - 1) <= 2
    return close(value) or close(~value & (2**64 - 1))


def choose_seed():
    while True:
        seed = secrets.randbits(64)
        if not near_k(seed):
            return seed


def contract_digest(contract):
    return fnv(struct.unpack('<16Q', bytes(contract)))


def decode(raw, contract=None):
    """Verify and decode a result record; raise ValueError when it cannot be trusted."""
    if len(raw) < ctypes.sizeof(Record):
        raise ValueError('short record')
    r = Record.from_buffer_copy(raw[:ctypes.sizeof(Record)])
    words = struct.unpack_from(f'<{ctypes.sizeof(Record) // 8}Q', raw)
    if r.magic != RECORD_MAGIC:
        raise ValueError('record magic differs')
    if r.version != 1 | (ctypes.sizeof(Record) << 16):
        raise ValueError('record version or size differs')
    if fnv(words[:-1]) != r.checksum:
        raise ValueError('record checksum differs')
    if contract is not None and (r.sequence != contract.sequence or r.contract_digest != contract_digest(contract)):
        raise ValueError('record names another contract')
    out = {name: getattr(r, name) for name in _RECORD_WORDS}
    out['status'] = RUN_STATUS[r.status] if r.status < len(RUN_STATUS) else r.status
    out['refusal'] = REFUSALS[r.refusal] if r.refusal < len(REFUSALS) else r.refusal
    out['stop'] = STOPS[r.stop] if r.stop < len(STOPS) else r.stop
    out['tests'] = {TESTS[i]: {'status': RESULTS[t.status] if t.status < len(RESULTS) else t.status,
                               'words': t.words, 'ticks': t.ticks} for i, t in enumerate(r.test)}
    out['mismatches'] = [{'address': m.address, 'expected': m.expected, 'observed': m.observed,
                          'test': TESTS[m.test], 'element': m.element}
                         for m in r.mismatch[:min(r.mismatch_count, 32)]]
    out['samples'] = [{'bytes': s.bytes, 'ticks': list(s.ticks)} for s in r.sample]
    out['chase'] = [{'bytes': c.bytes, 'loads': c.loads, 'ticks': c.ticks} for c in r.chase if c.bytes]
    out['clock'] = list(r.clock)
    out['dma_polls'] = list(r.dma_polls)
    return out


def abort_exclusion(progress, phase):
    """Blocks to exclude after an abort or SError: the block the progress word
    names and the one before it in the pass's travel order. A march walk may
    run either way through a member, so for the march both neighbors are
    excluded."""
    block = (progress - DRAM_BASE) >> BLOCK_SHIFT
    near = {block - 1, block, block + 1} if (phase >> 4) == TESTS.index('march') else {block - 1, block}
    return sorted(near & set(range(FIRST_BLOCK, END_BLOCK)))


def reviewed(run):
    """True when the run carries a review entry naming who reviewed it and when."""
    review = run.get('reviewed')
    return isinstance(review, dict) and bool(review.get('by')) and bool(review.get('date'))


def review_reason(run, session):
    """Why a run blocks further uploads until it is reviewed, or None.

    Any session: a scan hit, decode fault or digest mismatch; a DMA0 phase
    marker left set; an untrusted or missing record, unless an abort exclusion
    explains it and a later boot0 upload started a new session. Current
    session: any other run that did not pass."""
    if reviewed(run):
        return None
    status = run.get('status')
    if status == 'stopped' and run.get('stop') in REVIEW_STOPS:
        return f"stopped: {run.get('stop')}"
    if run.get('dma_phase'):
        return 'DMA0 phase marker set'
    if status not in TRUSTED_STATUS:
        if run.get('abort_excluded') and run['session'] < session:
            return None
        return 'untrusted or missing record'
    if run['session'] == session and status != 'pass':
        return f'{status} in the current session'
    return None


def acceptance(log):
    """The user's acceptance of every residual, from the run log."""
    record = log.get('acceptance')
    if not isinstance(record, dict) or not record.get('by') or not record.get('date'):
        raise ValueError('the run log holds no acceptance record with "by" and "date"')
    missing = [name for name in RESIDUALS if name not in record.get('residuals', [])]
    if missing:
        raise ValueError(f'the acceptance record does not name: {", ".join(missing)}')
    return {'by': record['by'], 'date': record['date'], 'residuals': list(RESIDUALS)}


def next_contract(log, lib, mask_names, budget_seconds):
    """Contract for the next run from the session run log.

    The log holds 'session' (boot0 upload count), 'acceptance' (see
    acceptance()), 'runs' (each with 'session', 'sequence', 'status', and when
    the record was trusted 'stop', 'resume' and 'dma_phase'; optionally
    'abort_excluded' and 'reviewed': {'by', 'date'}) and 'excluded' ranges.
    Every run checks the data and address lines and marches from the last
    resume address up to the next excluded range or the end of the map; the
    first run of a session adds the page signature and the harness.
    """
    acceptance(log)
    blocked = [(r['sequence'], reason) for r in log.get('runs', [])
               if (reason := review_reason(r, log['session']))]
    if blocked:
        raise ValueError('review the record of run ' + ', '.join(f'{s} ({why})' for s, why in blocked) +
                         ' and add a "reviewed" entry before another upload')
    runs = [r for r in log.get('runs', []) if r['session'] == log['session']]
    resumes = [r['resume'] for r in runs if r.get('resume')]
    sequence = 1 + max((r['sequence'] for r in log.get('runs', [])), default=0)
    excluded = [tuple(x) for x in log.get('excluded', [])]
    mask = sum(MASK[name] for name in mask_names) | MASK['march'] | MASK['data'] | MASK['address']
    if not runs:
        mask |= MASK['signature'] | MASK['harness']
    resume = (resumes[-1] - DRAM_BASE) >> BLOCK_SHIFT if resumes else FIRST_BLOCK
    end = min([s for s, _ in excluded if s >= resume] + [END_BLOCK])
    for start, stop in excluded:
        if start <= resume < stop:
            resume, end = stop, min([s for s, _ in excluded if s >= stop] + [END_BLOCK])
    if resume >= end:
        mask &= ~MASK['march']
    contract = make_contract(sequence, mask, choose_seed(), int(budget_seconds * COUNTER_HZ),
                             (resume, end) if mask & MASK['march'] else (0, 0), excluded)
    refusal, plan = admit(lib, contract)
    if refusal != 'admitted':
        raise ValueError(f'contract refused: {refusal}')
    return contract, plan


def image_layout(elf):
    """Contract and recovery addresses from the linked image's symbol table."""
    from elftools.elf.elffile import ELFFile
    with Path(elf).open('rb') as f:
        symbols = {s.name: int(s['st_value']) for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()}
    return {name: symbols[name] for name in ('memtest_contract', '__memtest_recovery_start', '__memtest_recovery_end',
                                             'monitor_entry')}


def prepare(args):
    lib = library(args.output / 'host-build')
    log = json.loads(args.run_log.read_text())
    contract, plan = next_contract(log, lib, args.mask, args.budget)
    accepted = acceptance(log)
    layout = image_layout(args.elf)
    image = args.image.read_bytes()
    (args.output / 'image.bin').write_bytes(image)
    (args.output / 'contract.bin').write_bytes(bytes(contract))
    (args.output / 'record-zero.bin').write_bytes(bytes(RECORD_BYTES))
    prepared = {'image_sha256': hashlib.sha256(image).hexdigest(), 'image_address': IMAGE_ADDRESS,
                'entry': layout['monitor_entry'], 'contract_address': layout['memtest_contract'],
                'record_address': RECORD_ADDRESS, 'sequence': contract.sequence, 'mask': contract.mask,
                'slice': [contract.slice_start, contract.slice_end], 'seed': contract.seed,
                'budget_ticks': plan.budget_ticks, 'groups': plan.groups,
                'contract_digest': contract_digest(contract), 'acceptance': accepted, 'physical_access': False}
    (args.output / 'prepared.json').write_text(json.dumps(prepared, indent=2) + '\n')
    print(json.dumps(prepared, indent=2))


def execute(transaction, wait_return, ready, p, record, journal, bundle, evidence):
    """One run: upload the image, the contract and a zero record page, start the
    monitor entry, wait for the returned ROM and read the result block."""
    ready()
    image, contract = bundle / 'image.bin', bundle / 'contract.bin'
    if hashlib.sha256(image.read_bytes()).hexdigest() != p['image_sha256']:
        raise ValueError('image differs from the prepared bundle')
    record['dirty'] = True
    journal()
    transaction('write', hex(p['image_address']), image, 'write', hex(p['contract_address']), contract,
                'write', hex(p['record_address']), bundle / 'record-zero.bin')
    record['launch_attempted'] = True
    journal()
    transaction('reset64', hex(p['entry']))
    wait_return()
    result = evidence / 'result.bin'
    transaction('read', hex(RESULT_ADDRESS), RESULT_BYTES, result)
    raw = result.read_bytes()
    decoded = decode(raw[RECORD_ADDRESS - RESULT_ADDRESS:], Contract.from_buffer_copy(contract.read_bytes()))
    record.update(memtest=decoded, monitor_status=struct.unpack_from('<Q', raw, 16)[0], complete=True)
    journal()
    return decoded


def verify(args):
    contract = Contract.from_buffer_copy(args.contract.read_bytes()) if args.contract else None
    raw = args.result.read_bytes()
    if len(raw) == RESULT_BYTES:
        raw = raw[RECORD_ADDRESS - RESULT_ADDRESS:]
    try:
        out = decode(raw, contract)
    except ValueError as error:
        # A record that fails verification never counts as a pass. The
        # progress word may still name the block an abort hit.
        progress, phase = struct.unpack_from('<Q', raw, 72)[0], struct.unpack_from('<Q', raw, 64)[0]
        print(json.dumps({'trusted': False, 'reason': str(error),
                          'exclude_blocks': abort_exclusion(progress, phase) if progress else []}, indent=2))
        raise SystemExit(1)
    out['trusted'] = True
    print(json.dumps(out, indent=2))
    raise SystemExit(0 if out['status'] == 'pass' else 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--run-log', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mask', action='append', default=[], choices=sorted(MASK))
    p.add_argument('--budget', type=float, default=20.0)
    v = sub.add_parser('verify')
    v.add_argument('--result', type=Path, required=True)
    v.add_argument('--contract', type=Path)
    args = parser.parse_args()
    if args.command == 'prepare':
        args.output.mkdir(parents=True, exist_ok=False)
        prepare(args)
    else:
        verify(args)


if __name__ == '__main__':
    main()
