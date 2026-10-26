#!/usr/bin/env python3
"""The host tool of the returning SD0 read image: signing, input and decoding.

usage: storage-read-returning-host.py --read-record SESSION_JSON --output NEW_DIRECTORY

SESSION_JSON is a session record that tools/board-bundles-next-read.py wrote
on a board, such as board-session-20261025-24/port-3-3/clock-audit-b; the
fabric-read fixtures are copies of it with their node, identity and words
replaced, so their schema and keys are the read tool's own.

Runs tools/storage-read-returning.py on model DDR and survey records, an
approval request and approvals written here, and logs built from the input's
plan, and requires:

- sign: refuses a survey that failed, ran on another port, or ran on another
  node or ROM identity than the DDR start, and an approval already signed;
- input: refuses an unsigned approval, an approval signed for another
  lifetime, an approval older than the DDR record, an approval for the other
  bundle and an approval with one requested field not true; accepts the signed
  approval and writes a 2192-byte input whose header and plan decode back;
- decode: accepts a complete log and computes the throughput summary;
  refuses a read line whose sector differs from the plan, a missing read count
  and a card line for another profile; reports a window stop as incomplete;
- apps/storage-read-returning/controlled.json: the kernel runner's parser
  accepts it, and a stopped log matches only the sd-stopped outcome;
- fabric: the tool's masks and parent rates equal those of
  drivers/storage/a733_mmc_platform.c, its read schema equals the schema
  the read tool's main() writes and the real record's, and its MBUS rule
  uses the mask, ROM value and boot0 value of include/a733_el3_mbus.h, which
  a733_el3_mbus_restore() compares with: a word whose fields hold the ROM's
  value gets boot0's fields and keeps its other bits, and a word with any
  other field value, such as DFS enabled, stays; port 3-3's pre-launch words
  0x03000002, 0xc3000000 and 0x84000000 give 0x03000002, 0x83000000 and,
  after the monitor's restore, 0x81000000 at 200, 600 and 600 MHz, and a word
  of 0x81000000 stays; the input command takes the words from a read record
  on the DDR start's node and identity and refuses a read from another node,
  a failed read and a configuration that carries its own fabric words.

No board, USB or queue access.
"""
import argparse
import binascii
import importlib.util
import json
import os
import re
from pathlib import Path
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sd_returning_tool', ROOT / 'tools/storage-read-returning.py')
tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(tool)
CONFIG = {'fabric': [0x03000002, 0x83000000, 0x84000000], 'fabric_hz': [200000000, 600000000, 400000000],
          'pmic_pins': [[2, 1, 1], [2, 1, 1]], 'expected_pins': [[3, 0, 1], [3, 0, 1], [15, 0, 1], [3, 0, 1],
                                                                  [15, 0, 1], [3, 0, 1], [15, 0, 1]],
          'desired_pins': [[2, 1, 0], [2, 1, 0], [2, 1, 2], [2, 1, 0], [2, 1, 0], [2, 1, 0], [0, 1, 0]]}
REQUEST = {'fields': {'A': {'initial': ['authorize_one_cmd0_initialization']}, 'B': {'topology': ['pf_bank_voltage_writer_exclusive']}}}


def refused(call, text):
    try: call()
    except ValueError as error:
        assert text in str(error), (text, str(error)); return str(error)
    raise AssertionError('accepted: ' + text)


def lifetime(root, name, node='/dev/bus/usb/003/051', identity=(7, 2142, 1097010, 0, 0), port='3-3', passed=True):
    ddr, survey = root / f'{name}-ddr', root / f'{name}-survey'
    ddr.mkdir(); survey.mkdir()
    (ddr / 'session.json').write_text(json.dumps({'complete': True, 'device': node, 'node_identity': list(identity),
        'result': {'magic': 0x53524444, 'stage': 5, 'board_result': 0, 'dram_mib': 4096}, 'model': 'MODEL-NOT-PHYSICAL'}))
    past = time.time() - 60; os.utime(ddr / 'session.json', (past, past))
    gates = {k: True for k in ('svc', 'little_endian', 'mmu_caches_endian_off', 'primary_mpidr', 'frequency',
                               'counter_monotonic')}
    (survey / 'session.json').write_text(json.dumps({'complete': passed, 'sram_restored': True, 'rom_usable': True,
        'node': node, 'identity': list(identity), 'port': port, 'result': {'launcher_gates': gates},
        'model': 'MODEL-NOT-PHYSICAL'}))
    return ddr, survey


def approval(path, bundle, fields):
    path.write_text(json.dumps({'bundle': bundle, 'reviewer': 'MODEL', 'reviewed_at': '2026-10-25T00:00:00+00:00',
                                'fields': fields, 'signature': None}))
    return path


def log_for(raw, outcome='complete', reads=None, profile=2, first=None):
    given = tool.parse_input(raw); plan = given['plan']
    reads = len(plan) if reads is None else reads
    lines = ['SD0 returning start', f'SD_PROFILE=0x{profile:016x}',
             'CARD ' + '1' * 32 + ' ' + '2' * 32 + ' c0ff8000 aaaa 0000000001dacc00 0235800300000000 '
             f'{(profile | 4 << 8 | (1 if profile == 2 else 3) << 16):08x} {50000000 if profile == 2 else 150000000:08x} '
             '00000000 0000000000000000 0001e240']
    for i in range(reads):
        f, b = plan[i]
        if first is not None and i == 0: f = first
        lines.append(f'R {i:03x} {f:08x} {b:02x} {100 + 10 * b:08x} {binascii.crc32(bytes(512 * b)):08x}')
    lines += [f'D{r} ' + '00' * 64 for r in range(8)]
    lines += [f'SD_READS=0x{reads:016x}', f'SD_PLAN=0x{len(plan):016x}', 'SD_PMIC_PARKED=0x0000000000000001']
    lines.append('COMPLETE SD0 returning read' if outcome == 'complete' else 'INCOMPLETE SD0 returning read: ' + outcome)
    return '\n'.join(lines) + '\n'


def main(out, real):
    out.mkdir(parents=True, exist_ok=False)
    request = out / 'approval-request.json'; request.write_text(json.dumps(REQUEST))
    results = {}
    ddr, survey = lifetime(out, 'a')
    other_ddr, other_survey = lifetime(out, 'other', node='/dev/bus/usb/003/060', identity=(7, 9, 9, 0, 0))
    bad_port = lifetime(out, 'port', port='3-1')[1]
    failed = lifetime(out, 'failed', passed=False)[1]
    a = approval(out / 'approval-A.json', 'A', {'initial': {'authorize_one_cmd0_initialization': True}})
    results['sign_refusals'] = [
        refused(lambda: tool.sign(a, ddr, failed, 'MODEL'), 'survey did not pass'),
        refused(lambda: tool.sign(a, ddr, bad_port, 'MODEL'), 'another port'),
        refused(lambda: tool.sign(a, ddr, other_survey, 'MODEL'), 'another node or ROM identity')]
    plan = tool.throughput().throughput_plan(0, 4194304)
    input_refusals = [refused(lambda: tool.approved(a, request, ddr, survey, 'A'), 'not signed for this lifetime')]
    tool.sign(a, other_ddr, other_survey, 'MODEL')
    input_refusals.append(refused(lambda: tool.approved(a, request, ddr, survey, 'A'), 'not signed for this lifetime'))
    a = approval(out / 'approval-A2.json', 'A', {'initial': {'authorize_one_cmd0_initialization': True}})
    tool.sign(a, ddr, survey, 'MODEL')
    results['sign_refusals'].append(refused(lambda: tool.sign(a, ddr, survey, 'MODEL'), 'already signed'))
    tool.approved(a, request, ddr, survey, 'A')
    value = json.loads(a.read_text()); value['signature']['signed_at'] = '2026-01-01T00:00:00+00:00'
    old = out / 'approval-A3.json'; old.write_text(json.dumps(value))
    input_refusals.append(refused(lambda: tool.approved(old, request, ddr, survey, 'A'), 'older than'))
    input_refusals.append(refused(lambda: tool.approved(a, request, ddr, survey, 'B'), 'another bundle'))
    false = approval(out / 'approval-A4.json', 'A', {'initial': {'authorize_one_cmd0_initialization': False}})
    tool.sign(false, ddr, survey, 'MODEL')
    input_refusals.append(refused(lambda: tool.approved(false, request, ddr, survey, 'A'), 'not approved'))
    results['input_refusals'] = input_refusals
    raw = tool.input_bytes(2, 0x55, CONFIG, plan, ids=(3, 5, 7))
    assert len(raw) == 2192 and struct.unpack_from('<4I', raw, 56)[:3] == (2, 0x00300000, 256)
    given = tool.parse_input(raw)
    assert given['plan'] == [tuple(p) for p in plan] and given['profile'] == 2 and given['generation'] == 0x55
    assert struct.unpack_from('<3I', raw, 72) == tuple(CONFIG['fabric']) and raw[96:102] == bytes([2, 1, 1, 2, 1, 1])
    (out / 'input.bin').write_bytes(raw)
    complete = tool.decode_log(log_for(raw), raw, 0)
    assert complete['complete'] and len(complete['reads']) == 256 and complete['card']['card_hz'] == 50000000
    assert set(complete['throughput']) >= {'sequential', 'random', 'single', 'per_block_us', 'per_request_us'}
    window = tool.decode_log(log_for(raw, 'window', reads=10), raw, 1)
    assert not window['complete'] and window['outcome'] == 'window' and len(window['reads']) == 10
    results['decode_refusals'] = [
        refused(lambda: tool.decode_log(log_for(raw, first=99), raw, 0), 'differs from the input plan'),
        refused(lambda: tool.decode_log(log_for(raw).replace('SD_READS=0x0000000000000100\n', ''), raw, 0), 'read count'),
        refused(lambda: tool.decode_log(log_for(raw, profile=3), raw, 0), 'another profile')]
    results['complete_throughput'] = complete['throughput']
    # Fabric: the tool's tables are the provider's, and each lifetime's read binds the words.
    source = (ROOT / 'drivers/storage/a733_mmc_platform.c').read_text()
    def array(name):
        body = source.split(name, 1)[1].split(';', 1)[0]
        return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body.split('=', 1)[1])]
    assert array('fabric_off[]') == [a - 0x02002000 for a in tool.FABRIC]
    assert array('fabric_mask[]') == list(tool.FABRIC_MASK)
    assert array('parent_mhz[3][8]') == [x for row in tool.PARENT_MHZ for x in row], array('parent_mhz[3][8]')
    assert tool.fabric([0x03000002, 0xc3000000, 0x84000000]) == ([0x03000002, 0x83000000, 0x84000000],
                                                                 [200000000, 600000000, 400000000])
    assert tool.fabric([0x03000002, 0xc3000000, 0x81000000])[1] == [200000000, 600000000, 600000000]
    refused(lambda: tool.fabric([0x03000002, 0x43000000, 0x84000000]), 'no admitted parent or gate')
    def read_record(name, node='/dev/bus/usb/003/051', identity=(7, 2142, 1097010, 0, 0), complete=True):
        d = out / name; d.mkdir()
        record = dict(real, complete=complete, node=node, identity=list(identity), port='3-3', transactions=[],
                      words={'0x2002500': 0x03000002, '0x2002580': 0xc3000000, '0x2002588': 0x84000000},
                      model='MODEL-NOT-PHYSICAL: a copy of a real read record with node, identity and words replaced')
        (d / 'session.json').write_text(json.dumps(record))
        return d
    reader = (ROOT / 'tools/board-bundles-next-read.py').read_text()
    written = re.findall(r"r = \{'schema': '([^']+)'", reader)
    assert written == [tool.READ_SCHEMA] == [real['schema']], (written, real['schema'])
    mbus = (ROOT / 'include/a733_el3_mbus.h').read_text()
    for name, value in (('A733_MBUS_FIELDS', tool.MBUS_FIELDS), ('A733_MBUS_ROM', tool.MBUS_ROM),
                        ('A733_MBUS_BOOT0', tool.MBUS_RESTORED)):
        assert f'#define {name} UINT32_C(0x{value:08x})' in mbus, name
    restore = mbus.split('static inline unsigned a733_el3_mbus_restore(void)', 1)[1].split('#else', 1)[0]
    assert '(mbus & A733_MBUS_FIELDS) != A733_MBUS_ROM' in restore and 'A733_MBUS_BOOT0' in restore
    assert tool.restored([1, 2, 0xc4000000])[2] == 0xc1000000  # bit 30 is outside the fields
    assert tool.restored([1, 2, 0x94000000])[2] == 0x94000000  # DFS enable (bit 28) set: not the ROM's fields
    assert tool.restored([1, 2, 0x84000000]) == [1, 2, 0x81000000] and tool.restored([1, 2, 0x81000000])[2] == 0x81000000
    good = read_record('fabric-read')
    assert tool.fabric_read(good, ddr) == (([0x03000002, 0x83000000, 0x81000000], [200000000, 600000000, 600000000]),
                                           [0x03000002, 0xc3000000, 0x84000000])
    fabric_refusals = [refused(lambda: tool.fabric_read(read_record('fabric-other', node='/dev/bus/usb/003/060'), ddr),
                               'another node'),
                       refused(lambda: tool.fabric_read(read_record('fabric-failed', complete=False), ddr), 'did not pass')]
    board = {k: v for k, v in CONFIG.items() if not k.startswith('fabric')}
    (out / 'board.json').write_text(json.dumps(board)); (out / 'fabric-config.json').write_text(json.dumps(CONFIG))
    common = ['input', '--approval', str(a), '--request', str(request), '--ddr', str(ddr), '--survey', str(survey),
              '--fabric-read', str(good), '--profile', '2']
    fabric_refusals.append(refused(lambda: tool.main(common + ['--config', str(out / 'fabric-config.json'),
                                                               '--output', str(out / 'refused.bin')]), 'this lifetime'))
    assert tool.main(common + ['--config', str(out / 'board.json'), '--output', str(out / 'cli-input.bin')]) == 0
    made = json.loads((out / 'cli-input.json').read_text())
    assert made['fabric'] == ['0x03000002', '0x83000000', '0x81000000'] and made['fabric_hz'] == [200000000, 600000000, 600000000]
    assert made['fabric_registers'] == ['0x03000002', '0xc3000000', '0x84000000']
    assert struct.unpack_from('<3I', (out / 'cli-input.bin').read_bytes(), 72) == (0x03000002, 0x83000000, 0x81000000)
    results['fabric_refusals'] = fabric_refusals
    # The image's controlled outcomes, as the kernel runner reads them.
    spec = importlib.util.spec_from_file_location('sd_returning_runner', ROOT / 'tools/check-a733-kernel.py')
    runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
    outcomes = runner.controlled_outcomes((ROOT / 'apps/storage-read-returning/controlled.json').read_text())
    terminal = {'magic': 0x534b4552, 'stage': 5, 'status': 1, 'end_counter': 1}
    stopped = log_for(raw, 'stopped', reads=0)
    found = [t for o in outcomes for t in o['log_contains'] if t in stopped]
    assert [o['name'] for o in outcomes if runner.controlled_match(o, terminal, runner.fault_lines(stopped),
            [t for t in o['log_contains'] if t in stopped])] == ['sd-stopped'], found
    results['controlled_outcomes'] = [o['name'] for o in outcomes]
    (out / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'results': results,
        'scope': 'tools/storage-read-returning.py on model records and logs; no runner, image or board.'}, indent=1) + '\n')
    print('PASS returning SD0 host tool: signing, input and decoding')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--read-record', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    main(args.output, json.loads(args.read_record.read_text()))
