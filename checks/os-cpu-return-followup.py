#!/usr/bin/env python3
"""Focused compound admission and SRAM/helper proof checks; no physical access.

SQL uses a temporary modeled queue. SRAM/FEL callbacks are synthetic. Exact
helper artifacts and the existing survey decoder are reused; no device opens.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import tempfile
from types import SimpleNamespace
from harness import refused, save
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('followup_check', ROOT / 'apps/os/cpu-return-followup.py')
f = importlib.util.module_from_spec(spec); spec.loader.exec_module(f)


def helper_log():
    lines = ['SUPPLY dcdc5_uv=1000000 dcdc3_uv=800000 sid=0x496 bin=4 dcxo_hz=26000000']
    for phase in ('before', 'after'):
        for name, pll, mux, mhz in [('CPU-L', 0xf8802600, 0x03010000, 494),
                                  ('CPU-B', 0xf8802700, 0x03000000, 1014),
                                  ('DSU', 0xf8802600, 0x03010105, 494)]:
            lines.append(f'{name} {phase} pll={hex(pll)} mux={hex(mux)} mhz={mhz} pat0=0x0 pat1=0x0 lfm=0x0')
    lines += ['VOLT dcdc5_selector=0x9e dcdc5_uv=800000', f.COMPLETE]
    return '\n'.join(lines) + '\n'


def memory_tx(memory, fail=False, bad_gate=False):
    transactions = []
    def tx(*args, pass_fds=()):
        transactions.append(tuple(map(str, args))); i = 0
        while i < len(args):
            op = args[i]; i += 1
            if op == 'read':
                address, size, path = int(args[i], 0), int(args[i+1]), Path(args[i+2]); i += 3
                if fail and address == 0x86000 and len(transactions) > 4:
                    raise ValueError('modeled report transport loss')
                path.write_bytes(memory.get(address, bytes([0xa5])*size)[:size])
            elif op == 'write':
                address, path = int(args[i], 0), Path(args[i+1]); i += 2
                memory[address] = path.read_bytes()
            elif op == 'exe':
                address = int(args[i], 0); i += 1
                if address == 0x85000:
                    raw = bytearray(128)
                    struct.pack_into('<4I', raw, 0, f.survey.MAGIC, 1, 128, 1)
                    struct.pack_into('<12I', raw, 16, 0x13, int(bad_gate), 0, 0x81000000,
                                     24000000, 0, 1000, 0, 1001, 0, 0xf2200, 0x1a000)
                    raw[120:128] = memory[0x86000][120:128]; memory[0x86000] = raw
            else:
                raise AssertionError(op)
        return ''
    return tx, transactions


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=False); rows = []
    record = {'complete': True, 'transport_failed': False, 'automatic_access': True, 'mode': 'kernel',
              'result': {'magic': 0x534b4552, 'stage': 5, 'status': 0}}
    log = helper_log(); proof = f.helper_result(record, log)
    assert proof['dcdc3_uv'] == proof['dcdc5_uv'] == 800000 and not proof['dram_restored']
    rows.append({'name': 'strict-entry800-helper-proof-and-retired-RAM', 'passed': True})
    for name, changed, reason in [
            ('wrong-final-clock', log.replace('CPU-B after pll=0xf8802700', 'CPU-B after pll=0xf8804d00'), 'boot clock'),
            ('pattern-enabled', log.replace('DSU after pll=0xf8802600 mux=0x3010105 mhz=494 pat0=0x0',
                                           'DSU after pll=0xf8802600 mux=0x3010105 mhz=494 pat0=0x20000000'), 'boot clock'),
            ('wrong-entry-rail', log.replace('dcdc3_uv=800000', 'dcdc3_uv=1050000'), 'entry supply'),
            ('wrong-readback', log.replace('dcdc5_selector=0x9e', 'dcdc5_selector=0xb2'), 'readback'),
            ('extra-DCDC3-write', log.replace('VOLT ', 'VOLT dcdc3_uv=800000 '), 'readback'),
            ('missing-complete', log.replace(f.COMPLETE, 'REFUSED CPU supplies'), 'complete normally'),
            ('duplicate-complete', log + f.COMPLETE + '\n', 'complete normally'),
            ('missing-after', log.replace('CPU-L after ', 'OTHER after '), 'inspection absent')]:
        refused(lambda: f.helper_result(record, changed), reason); rows.append({'name': name, 'passed': True})
    changed = copy.deepcopy(record); changed['result']['status'] = 2
    refused(lambda: f.helper_result(changed, log), 'complete normally')
    changed = dict(record, controlled={})
    refused(lambda: f.helper_result(changed, log), 'complete normally')
    rows.extend({'name': name, 'passed': True} for name in ('status-nonzero-refused', 'controlled-return-refused'))
    with tempfile.TemporaryDirectory(prefix='MODEL-compound-sram-') as temporary:
        root = Path(temporary); memory = {0x73000: b'\x5a'*40}; tx, trace = memory_tx(memory)
        result = f.cache_capture(tx, b'\x00'*40, root/'cache')
        assert result['sram_restored'] and memory[0x73000] == b'\x5a'*40
        rows.append({'name': 'cache-helper-40B-backup-restore', 'passed': True})
        for name, bad, expected in [('survey', False, True), ('bad-gate', True, False)]:
            memory = {0x85000: b'\x3c'*0x400, 0x86000: b'\x7e'*128}; tx, trace = memory_tx(memory, bad_gate=bad)
            if expected:
                result = f.survey_capture(tx, b'\x00'*128, root/name); assert all(result['result']['launcher_gates'].values())
            else:
                refused(lambda: f.survey_capture(tx, b'\x00'*128, root/name), 'launcher gate refused')
            assert memory[0x85000] == b'\x3c'*0x400 and memory[0x86000] == b'\x7e'*128
            rows.append({'name': name+'-exact-regions-restored', 'passed': True})
        memory = {0x85000: b'\x3c'*0x400, 0x86000: b'\x7e'*128}; tx, trace = memory_tx(memory, fail=True)
        refused(lambda: f.survey_capture(tx, b'\x00'*128, root/'loss'), 'modeled report transport loss')
        assert not any(x[0] == 'write' for x in trace[5:])
        rows.append({'name': 'report-transport-loss-no-restoration-request', 'passed': True})
    with tempfile.TemporaryDirectory(prefix='MODEL-compound-queue-') as temporary:
        root = Path(temporary); review = root/'review.json'; review.write_text('{"MODEL ONLY":true}\n')
        ref = {'path': str(review), 'sha256': f.digest(review)}; dbpath = root/'queue.sqlite3'
        db = sqlite3.connect(dbpath); db.executescript('CREATE TABLE settings(key TEXT,value);CREATE TABLE tests(id,state,owner,submission,review);')
        db.executemany('INSERT INTO settings VALUES(?,?)', [('halted', 0), ('port', 'MODEL-PORT')])
        submission = {'prerequisites': [f.PREREQUISITE]}; queue_review = {'prerequisites': {f.PREREQUISITE: ref}}
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)', ('MODEL-ID', 'claimed', 'MODEL-OWNER', json.dumps(submission), json.dumps(queue_review))); db.commit()
        a = SimpleNamespace(database=dbpath, claim='MODEL-ID', owner='MODEL-OWNER', review=review)
        snapshot = f.claim(a); assert snapshot['port'] == 'MODEL-PORT'
        before = db.execute('SELECT * FROM tests').fetchall(); f.claim(a, snapshot); assert db.execute('SELECT * FROM tests').fetchall() == before
        rows.append({'name': 'same-original-SQL-claim-read-only', 'passed': True})
        wrong = copy.copy(a); wrong.owner = 'OTHER'
        refused(lambda: f.claim(wrong), 'original CPU claim'); rows.append({'name': 'wrong-owner-refused', 'passed': True})
        db.execute('UPDATE tests SET state="passed"'); db.commit()
        refused(lambda: f.claim(a), 'original CPU claim'); rows.append({'name': 'finished-claim-refused', 'passed': True})
        db.execute('UPDATE tests SET state="claimed"'); db.execute('UPDATE settings SET value=1 WHERE key="halted"'); db.commit()
        refused(lambda: f.claim(a), 'halted'); rows.append({'name': 'halted-queue-refused', 'passed': True})
        db.execute('UPDATE settings SET value=0 WHERE key="halted"'); changed = json.loads(json.dumps(queue_review)); changed['extra'] = 'changed'
        db.execute('UPDATE tests SET review=?', (json.dumps(changed),)); db.commit()
        refused(lambda: f.claim(a, snapshot), 'changed during continuation'); rows.append({'name': 'mutated-review-refused', 'passed': True})
        db.close()
    save(args.out/'results.json', {'physical_access': False, 'scope': 'Actual compound claim/proof and bounded SRAM capture functions; synthetic temporary SQL and transport data, no hardware/queue authority', 'cases': rows})
    print('PASS', len(rows), 'focused compound cases')


if __name__ == '__main__': main()
