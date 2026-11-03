#!/usr/bin/env python3
"""Check `board.py submit --wait`: it waits for the result and prints the outcome, stage, first failure and decoded summary.

The queue's submit is replaced by a model that returns a job whose result file appears after a delay; everything else is the runner's own
argument parsing and main(). The check requires:
  waits       --wait does not return before the result exists, and prints the lines outcome, stage, first failure and decoded, then the path;
  exit        status 0 for a returned image without decoder failures, 1 for any other outcome and for a returned image whose decoder reports failures;
  unchanged   without --wait the path alone is printed after the result exists, and --no-wait prints it at once without waiting;
  exclusive   --wait and --no-wait together are refused.
Usage: submit-wait.py --output DIR
"""
import argparse
import contextlib
import importlib.util
import io
import itertools
import json
from pathlib import Path
import sys
import threading
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['tools/board.py']
sys.path.insert(0, str(ROOT / 'tools'))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    board = load('submit_wait_board', ROOT / 'tools/board.py')
    results = {
        'returned': {'outcome': 'returned', 'stage': 'restore-rails', 'first_failure': None,
                     'decoded': {'passed': True, 'graphs': [{'name': 'g0', 'status': 'match'}]}},
        'failed': {'outcome': 'failure-return', 'stage': 'capture', 'first_failure': {'site': 7, 'context': 'running'}},
        'decoder': {'outcome': 'returned', 'stage': 'capture', 'first_failure': None, 'decoded': {'passed': False},
                    'decoder_failures': ['graph g0 mismatch']},
    }

    runs, made = itertools.count(), []

    def submit(case, delay):
        path = out / f'{case}-{next(runs)}' / 'run/result.json'
        made.append(path)

        def write():
            time.sleep(delay)
            path.parent.mkdir(parents=True, exist_ok=True)
            board.save(path, results[case])
        threading.Thread(target=write).start()
        return {'id': '1-abcdef01', 'result_path': str(path)}

    def run(case, *flags, delay=0.6):
        board.Queue.submit = lambda self, args: submit(case, delay)
        arguments = ['--queue', str(out / 'queue'), 'submit', str(out / 'image.elf'), '--port', '3-1', '--kind', 'cpu-return',
                     '--worker', 'w', '--hypothesis', 'h', *flags]
        stream, started = io.StringIO(), time.monotonic()
        with contextlib.redirect_stdout(stream):
            code = board.main(arguments)
        return code, stream.getvalue().splitlines(), time.monotonic() - started

    passed = []
    code, lines, took = run('returned', '--wait')
    assert took >= 0.5 and code == 0, (code, took)
    assert lines[:3] == ['outcome: returned', 'stage: restore-rails', 'first failure: none'], lines
    assert 'passed: True' in lines and any(l.startswith('decoded: ') and '"graphs"' in l for l in lines), lines
    assert lines[-1] == str(made[-1]), lines
    passed.append('--wait waits for the result, prints outcome, stage, first failure and the decoded summary, then the path (exit 0)')
    code, lines, _ = run('failed', '--wait')
    assert code == 1 and lines[0] == 'outcome: failure-return' and lines[2].startswith('first failure: {') and '"site": 7' in lines[2], (code, lines)
    code, lines, _ = run('decoder', '--wait')
    assert code == 1 and any(l.startswith('decoder failures:') for l in lines), (code, lines)
    passed.append('exit status 1 for a failed outcome and for decoder failures')
    code, lines, took = run('returned')
    assert code == 0 and took >= 0.5 and lines == [str(made[-1])], (code, lines, took)
    code, lines, took = run('returned', '--no-wait', delay=2)
    assert code == 0 and took < 0.5 and lines == [str(made[-1])] and not made[-1].exists(), (code, lines, took)
    time.sleep(2.5)
    passed.append('without --wait the path alone is printed after the result exists; --no-wait prints it at once')
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            run('returned', '--wait', '--no-wait')
        raise AssertionError('--wait with --no-wait was accepted')
    except SystemExit as exit:
        assert exit.code == 2
    passed.append('--wait with --no-wait is refused')
    for line in passed:
        print('PASS ' + line)


if __name__ == '__main__':
    main()
