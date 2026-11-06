#!/usr/bin/env python3
"""The runner zeroes the Wi-Fi credential region on every exit of a session that uploaded one, and proves it by a readback.

A modeled FEL board (sparse memory, a transaction log) stands in for the device; the fake credential lives in a temporary
config directory under the output directory, never in ~/.config. For each exit kind the check shows the zero write is planned
and read back as zero, or that the entry records that the credential may remain:

- the session wiped the region itself: the runner reads zero and writes nothing more;
- a normal return, a failure return, a host error after launch: the runner writes zeros and reads zero back;
- a watchdog return to FEL: the fresh instance's DRAM is brought up again first, then the zero write;
- no return to FEL, a failed transport, a zero write that does not read back as zero: the entry says the credential may
  remain, the marker stays, and the port's next credential run zeroes the region before it uploads anything, or is refused;
- no credential byte reaches the queue, an entry, a result or the log (only the address, the size and the readback verdict).

Mutations: removing the final zero step, the zero write itself, the DRAM bring-up after a watchdog return, the marker, or
the start-of-run clearing each make the check fail.
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import types
from unittest.mock import patch

if not __debug__:
    raise RuntimeError('Checks require assertions.')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import board
spec = importlib.util.spec_from_file_location('wifi_credential_check', ROOT/'checks/wifi-credential.py')
wc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wc)

rows = []


def passed(name):
    rows.append(name)
    print('PASS', name)


class Model(wc.Board):
    """The wc board plus a plain read, a transaction log, and a stuck mode in which zero writes do not take."""
    def __init__(self):
        super().__init__()
        self.log = []
        self.zero_writes_stick = False

    def process(self, command, **keywords):
        arguments = command[3:]
        if arguments[0] == 'read':
            address, size = int(arguments[1], 0), int(arguments[2])
            Path(arguments[3]).write_bytes(self.memory.get(address, bytes(size)))
            self.log.append(('read', address, size))
            return types.SimpleNamespace(returncode=0, stdout='', stderr='')
        assert arguments[0] == 'write' and arguments[3] == 'read' and arguments[1] == arguments[4]
        data = Path(arguments[2]).read_bytes()
        address = int(arguments[1], 0)
        self.log.append(('write', address, len(data), not any(data)))
        if not (self.zero_writes_stick and not any(data)):
            self.memory[address] = data
        assert int(arguments[5]) == len(data)
        Path(arguments[6]).write_bytes(self.memory[address])
        return types.SimpleNamespace(returncode=0, stdout='', stderr='')


class ZeroDry(wc.Dry):
    scenario = {}

    def dram(self, label='dram'):
        wc.Dry.model.log.append(('dram', label))
        self.reset_since_dram = False

    def launch(self, plan):
        self.deliver_credential(plan)
        self.record['launch_attempted'] = True
        scenario, model = ZeroDry.scenario, wc.Dry.model
        if scenario.get('session_wipes'):
            model.memory[wc.wifi_module().RECORD_ADDRESS] = bytes(wc.wifi_module().RECORD_BYTES)
        kind = scenario['kind']
        if kind == 'failure-return':
            self.record.update(breadcrumbs={'board_failure': {'site': 'x', 'group': 'board', 'caller': '0'}, 'summary': ''},
                               failure_return=True)
        elif kind == 'watchdog-reset':
            self.record['dram_lost'] = True
            self.reset_since_dram = True
            return 'watchdog-reset'
        elif kind == 'host-error':
            raise RuntimeError('host error after launch')
        elif kind == 'no-return':
            self.record['no_return'] = True
            raise board.NoReturn('no return')
        elif kind == 'transport':
            self.failed = True
            raise board.TransportFailure('transport lost')
        return 'returned'


def case(work, label, shared, wifi, entry, kind='returned', session_wipes=False, stuck=False):
    """One credential run on the shared modeled board; returns (result, queue job)."""
    root = work/label
    root.mkdir()
    queue = board.Queue(work/'queue')  # one queue root: its credential-residue directory is the marker directory
    queue.resume('3-1')  # a no-return result pauses the port, as the daemon does; the next case resumes it
    job = queue.submit(wc.arguments(root, '--wifi', 'home'))
    ZeroDry.scenario = {'kind': kind, 'session_wipes': session_wipes}
    shared.zero_writes_stick = stuck
    shared.log = []
    result, _ = wc.dry_run(queue, job, wifi, entry)
    return result, queue.job(job['id']), root


def zeros(model, record):
    return not any(model.memory.get(record, b''))


def cases(work):
    wifi = board.wifi_module()
    config = wc.tool_cases(wifi, work)
    os.environ[wc.CONFIG_ENVIRONMENT] = str(config)
    entry = wifi.load('home', config)
    address, size = wifi.RECORD_ADDRESS, wifi.RECORD_BYTES
    needles = wc.forms(wifi, entry)
    shared = Model()
    roots = []
    residue = work/'queue/credential-residue'
    with patch.object(wc, 'Board', lambda: shared), patch.object(wc, 'Dry', ZeroDry):
        marker = residue/'3-1.json'

        def wipe_info(result, state):
            info = result['credential_wipe']
            assert info['state'] == state and info['address'] == hex(address) and info['bytes'] == size, info
            return info

        def uploaded(model):
            return [e for e in model.log if e[0] == 'write' and e[1] == address and not e[3]]

        def zero_writes(model):
            return [e for e in model.log if e[0] == 'write' and e[1] == address and e[3]]

        # The session wiped it: the runner reads zero and writes nothing.
        result, job, root = case(work, 'wiped', shared, wifi, entry, session_wipes=True)
        roots.append(root)
        assert result['outcome'] == 'returned' and wipe_info(result, 'wiped-by-session')['readback_zero'] is True
        assert len(uploaded(shared)) == 1 and not zero_writes(shared) and not marker.exists() and zeros(shared, address)
        assert not result.get('credential_may_remain') and not job.get('credential_may_remain')
        passed('session-wiped-region-reads-zero-and-needs-no-write')
        # Normal return, failure return, host error: the runner's zero write and its readback.
        for kind, outcome in (('returned', 'returned'), ('failure-return', 'failure-return'), ('host-error', 'host-error')):
            result, job, root = case(work, kind, shared, wifi, entry, kind=kind)
            roots.append(root)
            assert result['outcome'] == outcome, (kind, result['outcome'])
            info = wipe_info(result, 'zeroed-by-runner')
            assert info['readback_zero'] is True and len(zero_writes(shared)) == 1 and zeros(shared, address), kind
            assert not marker.exists() and not result.get('credential_may_remain'), kind
            # The write came after the upload and was read back (two reads of the region around the write).
            order = [e[0] for e in shared.log if e[0] in ('read', 'write')]
            assert order == ['write', 'read', 'write', 'read'], (kind, order)
            passed('zero-write-planned-after-%s' % kind)
        # A watchdog return: the fresh instance's DRAM comes up again before anything is written to it.
        result, job, root = case(work, 'watchdog', shared, wifi, entry, kind='watchdog-reset')
        roots.append(root)
        assert result['outcome'] == 'watchdog-reset'
        wipe_info(result, 'zeroed-by-runner')
        steps = [e[0] if e[0] != 'write' else ('zero' if e[3] else 'upload') for e in shared.log if e[0] in ('dram', 'write')]
        assert steps == ['dram', 'upload', 'dram', 'zero'], steps  # the first bring-up is the run's own
        assert ('dram', 'dram-zero') in shared.log and zeros(shared, address) and not marker.exists()
        passed('zero-write-planned-after-watchdog-return-with-dram-bring-up-first')
        # No return to FEL: no write can run. The entry records it, the marker stays, nothing is zeroed.
        result, job, root = case(work, 'no-return', shared, wifi, entry, kind='no-return')
        roots.append(root)
        assert result['outcome'] == 'no-return'
        info = wipe_info(result, 'may-remain')
        assert info['readback_zero'] is False and result['credential_may_remain'] and job['credential_may_remain']
        assert marker.exists() and not zero_writes(shared) and not zeros(shared, address)
        record = json.loads(marker.read_text())
        assert record['address'] == address and record['bytes'] == size and record['port'] == '3-1'
        passed('no-return-records-that-the-credential-may-remain-and-keeps-the-marker')
        # The next credential run on the port zeroes the region first, before uploading anything, and clears the marker.
        result, job, root = case(work, 'after-no-return', shared, wifi, entry, session_wipes=True)
        roots.append(root)
        assert result['outcome'] == 'returned' and not marker.exists()
        first = [e for e in shared.log if e[0] == 'write'][0]
        assert first[3] is True, 'the first write of the run was not the zero write'
        cleared = result['credential_residue_cleared']
        assert cleared['state'] == 'zeroed-by-runner' and cleared['bytes'] == size
        passed('next-credential-run-zeroes-the-region-before-uploading')
        # A failed transport: same as no return.
        result, job, root = case(work, 'transport', shared, wifi, entry, kind='transport')
        roots.append(root)
        wipe_info(result, 'may-remain')
        assert marker.exists() and not zeros(shared, address) and job['credential_may_remain']
        passed('failed-transport-records-that-the-credential-may-remain')
        # A zero write that does not read back as zero is never reported as success, and a credential run is refused
        # while the board cannot be zeroed: nothing is uploaded.
        result, job, root = case(work, 'stuck-refused', shared, wifi, entry, stuck=True)
        roots.append(root)
        assert result['outcome'] == 'refused-before-launch', result['outcome']
        assert 'zero write failed' in result['first_failure']['detail'] and not uploaded(shared)
        assert marker.exists() and not zeros(shared, address) and result['launch_attempted'] is False
        passed('credential-run-refused-while-the-zero-write-fails')
        shared.zero_writes_stick = False
        result, job, root = case(work, 'recovered', shared, wifi, entry)
        roots.append(root)
        assert result['outcome'] == 'returned' and not marker.exists() and zeros(shared, address)
        passed('zero-write-success-reopens-credential-runs')
        # A zero write that does not take after a session that leaves the credential: the entry says so.
        result, job, root = case(work, 'stuck-after-run', shared, wifi, entry, stuck=True)
        roots.append(root)
        wipe_info(result, 'may-remain')
        assert result['credential_may_remain'] and marker.exists()
        shared.zero_writes_stick = False
        shared.memory[address] = bytes(size)
        marker.unlink()
        passed('zero-write-that-does-not-read-zero-is-recorded-as-may-remain')
    # No credential byte in any queue file, entry, result, session log or marker: only the address, size and verdict.
    found = 0
    for root in roots + [work/'queue']:  # the queue root holds the entries, state, ledger and the marker directory
        found += wc.scan(root, needles)
    assert found > 20, found
    passed('no-credential-byte-in-any-file (%d scanned)' % found)


def mutations(work):
    """Each change must make the cases fail."""
    live = board.Live
    originals = {name: getattr(live, name) for name in ('finish_credential', 'zero_credential', 'mark_residue', 'clear_stale_residue')}

    def no_write(self, region, label='dram-zero'):
        require = board.require
        require(not self.failed, 'failed')
        self.residue_marker().unlink(missing_ok=True)
        return 'zeroed-by-runner'

    def no_dram(self, region, label='dram-zero'):
        self.reset_since_dram = False
        return originals['zero_credential'](self, region, label)

    changes = {
        'no final zero step': ('finish_credential', lambda self, result: None),
        'no zero write': ('zero_credential', no_write),
        'no DRAM bring-up after a watchdog return': ('zero_credential', no_dram),
        'no marker': ('mark_residue', lambda self, region: None),
        'no clearing at the start of a run': ('clear_stale_residue', lambda self, required: None),
    }
    for label, (name, replacement) in changes.items():
        directory = work/('mutation-' + label.replace(' ', '-'))
        directory.mkdir()
        before = len(rows)
        with patch.object(live, name, replacement), contextlib.redirect_stdout(io.StringIO()):
            try:
                cases(directory)
            except (AssertionError, KeyError, ValueError):
                pass
            else:
                raise AssertionError('mutation not detected: ' + label)
        del rows[before:]
        passed('mutation-detected: ' + label)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    work = args.output.resolve()
    clean = work/'cases'
    clean.mkdir()
    real = wc.snapshot_real_residue()
    cases(clean)
    mutations(work)
    assert wc.snapshot_real_residue() == real, 'the check left a file in the live credential-residue directory'
    passed('no-file-left-in-the-live-credential-residue-directory')
    board.save(work/'results.json', rows)
    print('%d checks passed; fake credential only, no device access' % len(rows))


if __name__ == '__main__':
    main()
