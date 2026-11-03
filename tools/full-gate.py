#!/usr/bin/env python3
"""Run the full gate on the bringup tip and record the result with the commit.

  full-gate.py [--cache DIR] [--bringup-worktree DIR] [--gate-command CMD]

tools/land.py starts this after a code landing, in its own process; the coordinator can run it by hand. The gate
runs in its own worktree, detached at the tip, so no landing waits for it. One run at a time: a run that finds
another running exits at once, and the running one is followed by the next landing's. The result is the file
CACHE/full-gate.txt and one line in CACHE/full-gate.jsonl. A failure names the failing steps and the commit range
since the last passing run. Exit status 0 when the gate passed, 1 when it failed. See docs/landing.md.
"""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_priority
import host_space
import land


def failing_steps(summary):
    text = summary.read_text(errors='replace') if summary.is_file() else ''
    named = re.search(r'^FAIL selected integration checks: (.*)$', text, re.M)
    steps = [s.strip() for s in named.group(1).split(',')] if named else []
    return steps or [l for l in text.splitlines() if re.match(r'(FAIL|SKIP|INCOMPLETE)\b', l)]


def run(args):
    bringup, cache = args.bringup_worktree, args.cache
    tip = land.out(bringup, 'rev-parse', 'bringup')
    tree = cache / 'full-gate-tree'
    if tree.exists():
        land.require_clean(tree, 'the full-gate worktree')
        land.git(tree, 'checkout', '-q', '--detach', tip)
    else:
        land.git(bringup, 'worktree', 'add', '-q', '--detach', str(tree), tip)
    label = 'full-%s-%s' % (tip[:8], datetime.datetime.now().strftime('%m%d-%H%M%S'))
    started = time.monotonic()
    subprocess.run(land.gate_argv(args, tree) + [str(tree), label, '--reason', 'full gate on bringup %s' % tip[:8], '--full'],
                   cwd=tree, capture_output=True, text=True)
    summary = cache / 'build' / ('gate-' + label) / 'summary.txt'
    passed = land.gate_passes(summary)
    history = [json.loads(l) for l in (cache / 'full-gate.jsonl').read_text().splitlines() if l.strip()] \
        if (cache / 'full-gate.jsonl').is_file() else []
    last_pass = next((r['commit'] for r in reversed(history) if r['passed']), None)
    record = {'time': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'), 'commit': tip,
              'label': label, 'seconds': round(time.monotonic() - started), 'passed': passed,
              'failing': [] if passed else failing_steps(summary), 'since': last_pass}
    with open(cache / 'full-gate.jsonl', 'a') as log:
        log.write(json.dumps(record) + '\n')
    if passed:
        text = 'full gate PASSED on %s in %d s (%s)\n' % (tip[:8], record['seconds'], label)
    else:
        text = ('full gate FAILED on %s (%s; read %s)\nfailing steps: %s\nthe first bad commit is in %s\n'
                % (tip[:8], label, summary, ', '.join(record['failing']) or 'none named; the gate did not finish',
                   '%s..%s' % (last_pass[:8], tip[:8]) if last_pass else 'the whole history'))
    (cache / 'full-gate.txt').write_text(text)
    print(text, end='')
    return 0 if passed else 1


def main(argv=None):
    host_priority.lower()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cache', type=Path, default=land.CACHE)
    parser.add_argument('--bringup-worktree', type=Path, default=land.BRINGUP_WORKTREE)
    parser.add_argument('--gate-command', default=os.environ.get('LAND_GATE_COMMAND'))
    args = parser.parse_args(argv)
    try:
        host_space.preflight(args.cache / 'build')
        host_space.use_disk_tmp(args.cache / 'build')
        args.gate_command = shlex.split(args.gate_command) if args.gate_command else None
        with open(args.cache / 'full-gate.lock', 'a+') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                print('another full gate is running; this tip is covered by the next run')
                return 0
            return run(args)
    except (land.Refused, host_space.SpaceShortage) as error:
        print('REFUSED: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
