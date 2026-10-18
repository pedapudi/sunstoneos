#!/usr/bin/env python3
"""Exercise the actual returning runner with simulated FEL and no device access."""
import argparse
from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


host = module('startup_host', ROOT/'tools/run-native-startup-probe.py')
primary = module('primary_host_check', ROOT/'tools/primary-publication-host.py')


class Retained(Exception):
    pass


def run_case(args, case, software, report_transform=None):
    directory = args.output/case
    directory.mkdir()
    survey = json.loads(args.survey.read_text())
    initial_identity = survey['identity']
    old = directory/'003'/'103'
    new = directory/'003'/'104'
    old.parent.mkdir()
    old.touch()
    new.touch()
    port = directory/'3-1'
    port.mkdir()
    state = {'returned': False, 'commands': [], 'quarantined': False, 'retained': False, 'clock': 0}
    sram = bytearray(b'\x61'*0x100000)
    dram = bytearray(b'\x73'*0x1200000)
    originals = bytes(dram)

    def memory(address, size):
        if address < len(sram):
            assert address+size <= len(sram)
            return sram, address
        assert 0x41000000 <= address and address+size <= 0x42200000
        return dram, address-0x41000000

    def fake_run(command, **unused):
        words = list(map(str, command[3:]))
        state['commands'].append(words.copy())
        result = SimpleNamespace(returncode=0, stdout='', stderr='')
        if words[0] == 'version':
            result.stdout = 'soc=00001903(A733)'
        elif words[0] == 'readl':
            assert words == ['readl', '0x20', 'readl', '0x24', 'readl', '0x28', 'readl', '0x3c', 'readl', '0x40']
            result.stdout = 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
        elif words[0] == 'exe':
            assert words == ['exe', '0x52000']
            nonce = bytes(dram[primary.probe.REPORT[0]-0x41000000+32:primary.probe.REPORT[0]-0x41000000+48])
            r = [0]*32
            r[:4] = [primary.probe.MAGIC, 1, 256, 4]
            r[4:6] = struct.unpack('<2Q', nonce)
            r[6:14] = [12, 0x81000000, 0x412fd050, 0x41, 0x073fc237, 24000000, 24, 48]
            r[14:20] = [0xc50838, 0, 0xc50838, 0x81234560, 0xabc000, 0x12345678]
            r[20:28] = [0xc50839, 0x800080, *r[16:20], *r[14:16]]
            r[28] = 0x42013f50
            if case == 'invalid-report-restored':
                r[3] = 3
            data = struct.pack('<32Q', *r)
            if report_transform is not None:
                data = report_transform(data, dram, case)
            array, offset = memory(primary.probe.REPORT[0], len(data))
            array[offset:offset+len(data)] = data
            sram[0x52000:0x52008] = b'NEW-ROM!'
            state['returned'] = True
        else:
            while words:
                op = words.pop(0)
                address = int(words.pop(0), 0)
                if op == 'write':
                    data = Path(words.pop(0)).read_bytes()
                    if case == 'transport-failure-retained' and address == primary.probe.OVERLAY[0]:
                        result.returncode = 1
                        return result
                    array, offset = memory(address, len(data))
                    array[offset:offset+len(data)] = data
                else:
                    assert op == 'read'
                    size = int(words.pop(0))
                    array, offset = memory(address, size)
                    Path(words.pop(0)).write_bytes(array[offset:offset+size])
        return result

    @contextmanager
    def locked():
        yield old

    @contextmanager
    def reenumeration(*unused):
        yield

    def held():
        state['retained'] = True
        raise Retained()

    def quarantine(*unused):
        state['quarantined'] = True

    def monotonic():
        state['clock'] += 2
        return state['clock']

    access = SimpleNamespace(board_lock=locked, require_session=lambda n: None, require_rom=lambda n: None,
        identity=lambda n: initial_identity if n == old else [7, 9999, 99999, 0, 0], target=lambda n: port,
        session_node=lambda config: new if state['returned'] and case != 'missing-return-retained' else None,
        quarantine_instance=quarantine)
    guard = SimpleNamespace(check_health=lambda: None, reenumeration=reenumeration)
    original_module, stop = host.module, host.base.stop_after_failure
    a = SimpleNamespace(build=args.build, bundle=args.bundle, software=software, survey=args.survey,
        database=directory/'MODEL-unused.sqlite3', fel=args.fel, evidence=directory/'runtime', claim='MODEL', owner='MODEL')
    with patch.object(host, 'module', lambda n, p: access if p.name == 'usb-access.py' else original_module(n, p)), \
         patch.object(host, 'claimed', lambda a, e: ({'identity': initial_identity, 'port': '3-1'}, 'MODEL')), \
         patch.object(host.subprocess, 'run', fake_run), patch.dict(sys.modules, {'usb_guard': guard}), \
         patch.object(host.time, 'monotonic', monotonic), patch.object(host.time, 'sleep', lambda n: None), \
         patch.object(host.base, 'hold_ownership', held), \
         patch.object(host.base, 'stop_after_failure', lambda r, s, q, e: stop(r, s, q, e, hold=held)):
        try:
            host.run(a)
        except (ValueError, Retained):
            assert case != 'complete-return'
        else:
            assert case == 'complete-return'
    record = json.loads((a.evidence/'session.json').read_text())
    if case in ('complete-return', 'invalid-report-restored'):
        assert bytes(dram) == originals and sram[0x52000:0x52008] == b'NEW-ROM!'
        assert record['dram_restored'] and record['rom_usable']
        assert record['complete'] == (case == 'complete-return')
        assert not state['quarantined'] and not state['retained']
    else:
        assert state['quarantined'] and state['retained'] and not record['complete']
        if case == 'missing-return-retained':
            assert state['commands'][-1] == ['exe', '0x52000']
        else:
            assert state['commands'][-1][:2] == ['write', hex(primary.probe.OVERLAY[0])]
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bundle', 'model', 'survey', 'fel', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--case', action='append', help='Run only named affected cases; preserve accepted cases.')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=bool(args.case))
    names = ('complete-return', 'transport-failure-retained', 'missing-return-retained',
             'invalid-report-restored', 'changed-input-refused')
    proof = {'schema': 'a733-primary-publication-software-v1', 'physical_access': False,
        'manifest_sha256': primary.digest(args.build/'manifest.json'),
        'host_sources': {n: primary.digest(ROOT/n) for n in primary.HOST_SOURCES},
        'instruction_evidence': {'path': str(args.model.resolve()), 'sha256': primary.digest(args.model)},
        'host_cases': dict.fromkeys(names, True)}
    fixture = args.output/'MODEL-never-deploy.json'
    fixture.write_text(json.dumps(proof, indent=2)+'\n')
    accepted_path = args.output/'accepted-cases.json'
    accepted = json.loads(accepted_path.read_text()) if accepted_path.exists() else {
        'manifest_sha256': proof['manifest_sha256'], 'host_sources': proof['host_sources'], 'cases': {}}
    assert accepted['manifest_sha256'] == proof['manifest_sha256'] and accepted['host_sources'] == proof['host_sources']
    selected = args.case or names
    assert set(selected) <= set(names)
    for name in selected:
        if name == 'changed-input-refused':
            manifest = json.loads((args.build/'manifest.json').read_text())
            manifest['native_reservations'][0]['bytes'] -= 4
            try:
                primary.load(args.build, args.bundle/'image.elf', manifest)
            except ValueError:
                pass
            else:
                raise AssertionError('Changed reservation accepted')
        else:
            assert run_case(args, name, fixture)
        accepted['cases'][name] = {'pass': True, 'check_source_sha256': primary.digest(Path(__file__)), 'reused': False}
        accepted_path.write_text(json.dumps(accepted, indent=2)+'\n')
        print('PASS '+name, flush=True)
    assert set(accepted['cases']) == set(names) and all(c['pass'] for c in accepted['cases'].values())
    proof['case_evidence'] = accepted['cases']
    proof['check_source_sha256'] = primary.digest(Path(__file__))
    proof['limits'] = 'Actual shared runner and validation; FEL, USB instance, queue authority and report are simulated. No hardware, reset or cache qualification.'
    (args.output/'software.json').write_text(json.dumps(proof, indent=2)+'\n')


if __name__ == '__main__':
    main()
