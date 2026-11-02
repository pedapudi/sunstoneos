#!/usr/bin/env python3
"""Check that host load cannot refuse a healthy setup stage, and that a real watchdog reset is still detected.

Entry 1793638219249052649-1574707e on port 3-3 ran the rail helper normally, then the runner measured 16.78 s of host time
from the arm against a 14 s cutoff, called it a possible watchdog reset, refused before launch and quarantined a healthy
instance (host load average 48 on 24 cores; the same measurement is 2.1 s in a typical run). checks/data/setup-timing-records.json
holds that result and a recorded watchdog-reset result (checks/data/ fields trimmed from the queue's result.json). Cases:
  loaded host     the rail helper stage replays the entry's timings with the board returning after the typical 2.11 s and the host
                  noticing 14.67 s later (16.78 s in all): the stage proceeds, reads the helper result and quarantines nothing;
  real expiry     the board itself returns after the cutoff (the countdown ran out, so the kernel dates the new instance late):
                  the stage refuses, reads no DRAM and quarantines;
  undated         a node the kernel cannot date falls back to the host's elapsed time, as before;
  recorded reset  the recorded watchdog-reset result's return record carries WDT0 still enabled, which the runner classifies
                  as a watchdog reset, and the normal returns in the same records do not;
  enumeration     a new instance whose node permissions arrive after the wait's deadline is still adopted;
  release         a quarantine left by the refused entry is cleared when no image was launched, the helper's disarm read back
                  clear, the runner reset to a fresh instance and the instance's ROM descriptor is the A733 service; it is kept
                  when an image was launched, the disarm is unverified, the descriptor is not the service, a transport failure
                  was recorded, or another instance is enumerated; the earlier watchdog-reset release is unchanged.
Usage: board-setup-timing.py --output DIR (BOARD_SETUP_TIMING_TOOLS names another copy of tools/ to run the cases against)
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import types
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
import os
TOOLS = Path(os.environ.get('BOARD_SETUP_TIMING_TOOLS', ROOT / 'tools')).resolve()   # another copy of tools/: the code before the fix
sys.path.insert(0, str(TOOLS))
import board
import board_images as images

CHECK_INPUTS = ['tools/board.py', 'checks/data/setup-timing-records.json', 'checks/board-runner.py']
RECORDS = json.loads((ROOT / 'checks/data/setup-timing-records.json').read_text())


def runner_check():
    spec = importlib.util.spec_from_file_location('board_runner_check', ROOT / 'checks/board-runner.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rails(runner, out, name, board_seconds, host_extra, created=True):
    """Run the pre-launch rail helper stage against the recorded model with the given board and host timings."""
    import struct
    root = out / name
    root.mkdir()
    args = runner.arguments(root)
    args.rail_helper = root / 'pinned-model.bin'
    args.rail_helper.write_bytes(b'MODEL PINNED HELPER')
    log = ''.join(f'{n} {ph} pll={pll:#x} mux={mux:#x} mhz={mhz} pat0=0 pat1=0 lfm=0\n'
                  for n, pll, mux, mhz in (('CPU-L', 0xf8802600, 0x03010000, 494), ('CPU-B', 0xf8802700, 0x03000000, 1014),
                                           ('DSU', 0xf8802600, 0x03010105, 494)) for ph in ('before', 'after'))
    log += 'SUPPLY dcdc5_uv=1000000 dcdc3_uv=800000 bin=4 dcxo_hz=26000000\nVOLT dcdc5_selector=30 dcdc5_uv=800000\n' + board.state.RAIL_COMPLETE + '\n'
    raw = bytearray(16384)
    encoded = log.encode()
    struct.pack_into('<4Q', raw, 0, 0x534b4552, 5, 0, len(encoded))
    raw[256:256 + len(encoded)] = encoded

    class Model(runner.Model):
        stamp = None

        def discover(self, port):
            if self.pending:
                self.stamp = self.clock + board_seconds          # the kernel creates the node when the board returns
                self.clock += board_seconds + host_extra          # the loaded host notices much later
            return super().discover(port)

        def process(self, command, **kwargs):
            if command[3:] == ['reset64', '0x41000000']:
                self.pending = True
                self.memory.update({0x42000000 + n: b for n, b in enumerate(raw)})
                return types.SimpleNamespace(returncode=0, stdout='', stderr='')
            return super().process(command, **kwargs)

    model = Model('normal')
    model.registers[0x07090160] = 0x10008002
    extra = {'wall_now': lambda: model.clock, 'instance_created': lambda node: model.stamp if created else None}
    accepted = board.Live.__init__.__code__.co_varnames
    owner = board.Live(args, root, {}, access=model, process=model.process, now=model.now, sleep=model.sleep,
                       elapsed_now=lambda: model.clock, **{k: v for k, v in extra.items() if k in accepted})
    error = None
    with patch.object(board.state, 'RAIL_HELPER_SHA256', images.sha(args.rail_helper)), patch.object(board.os, 'access', return_value=True), owner.locked():
        try:
            owner.rails()
        except ValueError as exc:
            error = str(exc)
    return owner, model, root, error


def watchdog_reset_cases():
    reset = RECORDS['watchdog_reset']['watchdog_return']
    assert reset['before'] & 1 and reset['after'] == 0
    assert board.Live.watchdog_reset_seen(reset['before'])
    for normal in (0, 0x0):
        assert not board.Live.watchdog_reset_seen(normal)
    assert RECORDS['watchdog_reset']['outcome'] == 'watchdog-reset' and RECORDS['watchdog_reset']['rom_returned']


def release_cases(out):
    class Access:
        def __init__(self, root, identity, descriptor='02b3'):
            self.RECOVERY_REQUIRED = root / 'recovery-required.json'
            self.current = identity
            self.sysfs = root / '3-3'
            self.sysfs.mkdir(exist_ok=True)
            (self.sysfs / 'bcdDevice').write_text(descriptor + '\n')

        def discover(self, port):
            return Path('/MODEL/003/084')

        def identity(self, node):
            return self.current

        def target(self, node):
            return self.sysfs

    refused = RECORDS['refused_on_host_load']
    last = refused['instances'][-1]['identity']

    def attempt(name, edit=None, current=None, descriptor='02b3', **fields):
        root = out / ('release-' + name)
        root.mkdir()
        queue = board.Queue(root / 'state')
        args = harness_args(root)
        job = queue.submit(args)
        result = {k: v for k, v in refused.items() if k != 'entry'}
        result.update(fields)
        if edit:
            edit(result)
        with queue.locked() as state:
            entry = next(j for j in state['jobs'] if j['id'] == job['id'])
            entry.update(state='running', assigned_port='3-3')
        queue.complete(job['id'], result)
        access = Access(root, list(current or last), descriptor)
        record = access.RECOVERY_REQUIRED.with_name('recovery-required-3-3-1.json')
        record.write_text(json.dumps({'identity': last, 'reason': board.DRAM_UNRESOLVED}))
        with patch.object(board, 'CACHE', root / 'cache'):
            try:
                queue.release('3-3', 'the refused entry left a live FEL instance', access)
            except ValueError as error:
                return record, str(error)
        return record, None

    def harness_args(root):
        runner = runner_check()
        return runner.arguments(root, submit=True)

    record, error = attempt('accepted')
    assert error is None and not record.exists(), error
    for name, kwargs in (('image-launched', dict(edit=lambda r: r.update(launch_attempted=True))),
                         ('disarm-unverified', dict(edit=lambda r: r['rail_return'].update(mode_after_disarm=1))),
                         ('not-fresh', dict(edit=lambda r: r.update(fresh_fel=False))),
                         ('not-the-service', dict(descriptor='0100')),
                         ('transport-failure', dict(cleanup_failure='vbus')),
                         ('other-instance', dict(current=[7, 8399, 1, 0, 0]))):
        record, error = attempt(name, **kwargs)
        assert error and record.exists(), (name, error)


def wait_case(runner, out):
    """A new instance whose permissions arrive late is adopted, not reported as no return."""
    root = out / 'wait'
    root.mkdir()
    model = runner.Model('normal')
    args = runner.arguments(root)
    owner = board.Live(args, root, {}, access=model, process=model.process, now=model.now, sleep=model.sleep)
    model.pending = True
    calls = {'n': 0}

    def access(node, mode):
        calls['n'] += 1
        return calls['n'] > 400            # about 20 s of 50 ms polls after the enumeration: past a 14 s deadline
    with patch.object(board.os, 'access', side_effect=access), owner.locked():
        owner.identity = [1, 1, 3, 4, 5]
        owner.wait(14, [1, 1, 3, 4, 5])
    assert owner.node == model.node


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    runner = runner_check()
    typical = RECORDS['normal_rail_return_seconds_median']
    recorded = RECORDS['refused_on_host_load']['rail_return']['seconds']
    # loaded host: the recorded 16.78 s of host time with the board's own 2.11 s
    owner, model, root, error = rails(runner, out, 'loaded', typical, recorded - typical)
    assert error is None and not model.quarantined, error
    assert abs(owner.record['rail_return']['seconds'] - recorded) < 1e-6 and owner.record['rail_return']['decided_by'] == 'kernel', owner.record['rail_return']
    assert owner.record['rails']['dcdc5_uv'] == 800000 and not owner.armed
    # a real expiry: the board returned after the cutoff
    owner, model, root, error = rails(runner, out, 'expired', 20, 0)
    assert error and 'watchdog' in error and bool(model.quarantined) and not (root / 'rails/rail-result.bin').exists(), error
    # undated: the host's time decides, as before
    owner, model, root, error = rails(runner, out, 'undated', typical, recorded - typical, created=False)
    assert error and 'watchdog' in error and owner.record['rail_return']['decided_by'] == 'host', error
    watchdog_reset_cases()
    wait_case(runner, out)
    release_cases(out)
    print('PASS board setup timing: the recorded 16.78 s host measurement proceeds (board return %.2f s); a return after the cutoff '
          'is refused and quarantined; an undated node falls back to host time; the recorded watchdog reset is classified; '
          'a late permission grant is adopted; release clears the refused entry\'s quarantine and keeps six unsafe variants' % typical)


if __name__ == '__main__':
    main()
