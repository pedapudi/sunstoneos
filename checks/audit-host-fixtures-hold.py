#!/usr/bin/env python3
"""A closed console cannot end the shared ownership hold or skip quarantine; host only."""
import errno
import importlib.util
from pathlib import Path
import sys
from unittest.mock import patch
assert __debug__, 'Run without -O; this check relies on assert statements.'
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('smp_hold_check', ROOT / 'tools/check-a733-smp.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


class ClosedConsole:
    def write(self, text): raise BrokenPipeError(errno.EPIPE, 'closed console')
    def flush(self): raise BrokenPipeError(errno.EPIPE, 'closed console')


class Released(Exception):
    """Stands in for the process ending; only the modeled sleep raises it."""


def run(console):
    sleeps = []
    def sleep(seconds):
        sleeps.append(seconds)
        # One operator interrupt is answered with a report, then the hold continues.
        if len(sleeps) == 2: raise KeyboardInterrupt
        if len(sleeps) == 4: raise Released
    events = []
    record = {'launch_attempted': True}
    with patch.object(base.time, 'sleep', sleep), patch.object(sys, 'stderr', console):
        try:
            base.stop_after_failure(record, lambda: events.append('save'), lambda: events.append('quarantine'),
                                    RuntimeError('modeled launch failure'))
        except Released:
            pass
        else:
            raise AssertionError('Hold returned')
    assert events == ['save', 'quarantine'] and record['failure'] == 'modeled launch failure'
    assert sleeps == [1, 1, 1, 1], sleeps


run(ClosedConsole())
closed = open(Path(__file__), encoding='utf-8')
closed.close()
run(closed)
print('PASS closed and broken consoles keep quarantine and the ownership hold')
