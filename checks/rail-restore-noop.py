#!/usr/bin/env python3
"""Check that restoring rails that are already at the target is a verified no-op, and that a real refusal is still caught.

The post-ROM rail helper takes DCDC5 from the 1000 mV an image left it at to 800 mV, and refuses ("CPU supplies") a board whose rails
are not at that entry point. NPU loader images never raise the CPU rails, so on their runs the helper found 800 mV and refused: the
runner turned a valid application result into host-error at stage restore-rails (entries 1793658232392271857-b57f9e54,
1793658132997079733-4f56db5e and 1793658100400649402-10e2ca3c, among seven since 2026-11-01), while 111 npu runs that did raise the
rails restored normally. checks/data/rail-restore-logs.json holds the three refused logs and one restored log from the queue's
evidence. Cases:
  logs      each refused log is an already-at-target no-op (the helper's own readbacks: refused before any VOLT operation, DCDC5 and DCDC3
            at 800 mV, the three boot clock rows) and the restored log is a restore;
  mutations a log whose supplies are not 800/800, one with an operation, one with another refusal reason, one with a wrong clock row,
            one that also says COMPLETE and one with the wrong magic are refused (RailRestoreRefused), not accepted as a no-op;
  stage     the runner's restore stage with a refused log at 800 mV completes, records the helper outcome and restores the DRAM it
            saved; with supplies at 1050/1000 mV it raises RailRestoreRefused after restoring the DRAM;
  port      the queue keeps the application's outcome and pauses the port only when the result says the rails were not restored.
Usage: rail-restore-noop.py --output DIR
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
import types
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import board
import board_images as images

CHECK_INPUTS = ['tools/board.py', 'tools/board_images.py', 'apps/os/cpu-return-followup.py', 'checks/data/rail-restore-logs.json']
LOGS = json.loads((ROOT / 'checks/data/rail-restore-logs.json').read_text())


def runner_check():
    spec = importlib.util.spec_from_file_location('rail_restore_runner_check', ROOT / 'checks/board-runner.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def logs_cases():
    for name, entry in LOGS['refused'].items():
        outcome = images.post_rail_outcome(entry['values'], entry['log'])
        assert outcome['outcome'] == 'already-at-target' and outcome['dcdc5_uv'] == 800000 and not outcome['written'], (name, outcome)
    restored = images.post_rail_outcome(LOGS['restored']['values'], LOGS['restored']['log'])
    assert restored['outcome'] == 'restored', restored
    entry = next(iter(LOGS['refused'].values()))
    log, values = entry['log'], entry['values']
    mutations = {
        'dcdc5 above target': log.replace('dcdc5_uv=800000', 'dcdc5_uv=1050000'),
        'dcdc3 above target': log.replace('dcdc3_uv=800000', 'dcdc3_uv=1000000'),
        'an operation was made': log + 'VOLT dcdc5_selector=0x9e dcdc5_uv=800000\n',
        'another refusal': log.replace('CPU supplies', 'boot clock words'),
        'wrong clock row': log.replace('CPU-B before pll=0xf8802700 mux=0x3000000 mhz=1014', 'CPU-B before pll=0xf8802700 mux=0x3000000 mhz=1500'),
        'also complete': log + LOGS['restored']['log'].splitlines()[-1] + '\n',
        'no supply line': log.replace('SUPPLY', 'SUPPLX'),
    }
    for name, text in mutations.items():
        try:
            outcome = images.post_rail_outcome(values, text)
        except images.RailRestoreRefused as error:
            assert error.record['outcome'] == 'refused', (name, error.record)
        else:
            raise AssertionError(f'{name}: accepted as {outcome["outcome"]}')
    try:
        images.post_rail_outcome([0, 5, 1], log)
    except images.RailRestoreRefused:
        pass
    else:
        raise AssertionError('wrong magic accepted')
    return len(LOGS['refused']), len(mutations) + 1


def stage_case(runner, out, name, log, values):
    root = out / name
    root.mkdir()
    args = runner.arguments(root)
    helper = root / 'post-helper.bin'
    helper.write_bytes(b'MODEL POST HELPER')
    raw = bytearray(16384)
    encoded = log.encode()
    struct.pack_into('<4Q', raw, 0, values[0], values[1], values[2], len(encoded))
    raw[256:256 + len(encoded)] = encoded

    class Model(runner.Model):
        def process(self, command, **kwargs):
            if command[3:] == ['reset64', '0x41000000']:
                self.pending = True
                self.memory.update({0x42000000 + n: b for n, b in enumerate(raw)})
                return types.SimpleNamespace(returncode=0, stdout='', stderr='')
            return super().process(command, **kwargs)

    model = Model('normal')
    model.registers[0x07090160] = 0x10008002
    record = {'normal_return': True, 'native_retirement_validated': True, 'dram_unsaved': True}
    owner = board.Live(args, root, record, access=model, process=model.process, now=model.now, sleep=model.sleep,
                       elapsed_now=lambda: model.clock, wall_now=lambda: model.clock, instance_created=lambda node: model.clock)
    error = None
    with patch.object(board, 'POST_RAIL_HELPER', helper), patch.object(board, 'POST_RAIL_HELPER_SHA256', images.sha(helper)), \
            patch.object(board.os, 'access', return_value=True), owner.locked():
        try:
            owner.rails('restore-rails')
        except images.RailRestoreRefused as exc:
            error = exc
    return owner, error


def port_case(out, rails_unrestored):
    root = out / ('port-' + ('unrestored' if rails_unrestored else 'noop'))
    root.mkdir()
    queue = board.Queue(root / 'state')
    runner = runner_check()
    job = queue.submit(runner.arguments(root, submit=True))
    with queue.locked() as state:
        entry = next(j for j in state['jobs'] if j['id'] == job['id'])
        entry.update(state='running', assigned_port='3-1')
    result = {'outcome': 'returned', 'first_failure': None, 'normal_return': True, 'rom_returned': True,
              'rails_restored': not rails_unrestored, 'rail_restore': {'outcome': 'x'}}
    if rails_unrestored:
        result['rails_unrestored'] = True
    queue.complete(job['id'], result)
    status = queue.status()['boards']['3-1']
    with queue.locked() as state:
        done = next(j for j in state['jobs'] if j['id'] == job['id'])
    assert done['result']['outcome'] == 'returned'
    return bool(status['pause'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    replayed, mutated = logs_cases()
    runner = runner_check()
    entry = next(iter(LOGS['refused'].values()))
    owner, error = stage_case(runner, out, 'stage-noop', entry['log'], entry['values'])
    assert error is None and owner.record['rails']['outcome'] == 'already-at-target' and not owner.armed, (error, owner.record.get('rails'))
    assert owner.record['rail_runs']['restore-rails']['rails']['outcome'] == 'already-at-target'
    wrong = entry['log'].replace('dcdc5_uv=800000', 'dcdc5_uv=1050000')
    owner, error = stage_case(runner, out, 'stage-wrong', wrong, entry['values'])
    assert error is not None and owner.record['rails']['outcome'] == 'refused' and not owner.armed
    assert port_case(out, False) is False, 'a verified no-op paused the port'
    assert port_case(out, True) is True, 'unrestored rails did not pause the port'
    print(f'PASS rail restore no-op: {replayed} refused logs from the queue are verified no-ops and the restored log restores; '
          f'{mutated} mutated logs are refused; the stage records the helper outcome; the port pauses only for unrestored rails')


if __name__ == '__main__':
    main()
