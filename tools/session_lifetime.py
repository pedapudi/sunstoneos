#!/usr/bin/env python3
"""Session lifetime of a returning OS image: derivation from the manifest, bounds and the refusal terms.

An image's session lifetime (A733_BOARD_SUPERVISED_SECONDS) is the time from the launcher arming the
board to the diagnostic deadline, after which the watchdog returns the board to FEL. The lifetime is a
property of the image's own application manifest:

    lifetime = max(FLOOR_SECONDS, ceil(STARTUP + SETUP + run_us + return_reserve_us))

The OS session decoder (apps/os/session.c) refuses a manifest whose run deadline ends later than
lifetime - return_reserve_us. The decoder runs in EL3 admission, so a refusal there stops the board with
no application output. This module is the one host-side statement of the rule: the CMake configuration
derives the compiled lifetime from it, the image link check and the runner pre-flight apply it.

Bounds. Nothing in the board's hardware limits a session to 60 s or to 600 s:
  * the 24 MHz counter and the contract's deadline fields are 64 bits wide;
  * the host-armed WDT0 interval is 16 s (mode 0xb1) and the OS supervisor restarts it
    (apps/os/watchdog.c), so the lifetime does not depend on the watchdog interval;
  * the DRAM backup and restore and the RTC breadcrumbs do not depend on elapsed time.
What does scale with the lifetime is the thermal reading buffer (56 bytes per 100 ms in
apps/os/thermal.c) and the time the runner holds the USB port. MAX_SECONDS is a policy limit
chosen from those two: 3600 s gives a 2.0 MB buffer in the 10 MB cached image RAM and holds a
board for one hour at most. The breadcrumb timestamp (include/sunstone_crumb.h, 29 bits of 1024
counter ticks) wraps at 22,906 s, which is the hardware encoding limit above it. The probe
contract's separate 600 s limit (docs/board-probe-contract.md) governs raw probe images only.
"""
import argparse
import json
from pathlib import Path
import sys

US = 1_000_000
# A733_BOARD_STARTUP_SECONDS (include/a733_board_application.h): the EL3 startup window after arming.
STARTUP_SECONDS = 1
# Time from the end of EL3 startup to the application's main(): OS service start, application admission and the input
# digests. Measured with tools/session_setup_time.py on the 2026-11-02 runs in the board queue's evidence
# (~/.cache/sunstoneos/board-queue/entries/*/run/{return-report.bin,breadcrumbs.json}, images built from c10eb74e and
# bbb2d08a): the arm-to-ROM-return time bounds startup plus setup from above for an application that does little work.
# Those applications returned within 0.09 s (hw-facts), 0.11 s (heap-test), 0.23 s (queue-add, queue-gemm), 0.53 s at the
# most (latency), and mimus, which hashes its multi-MiB inputs and recognizes, returned within 0.73 s in its fastest of
# 176 runs. The worst measured figure is therefore 0.73 s for startup, setup, the application's work and the return
# together. The allowance below, with the 1 s EL3 startup bound, is 3 s: four times that figure. A run longer than the
# allowance is clipped by the session bound (grant->finish_by_us), never extended.
SETUP_SECONDS = 2
# The lifetime of an image whose manifest needs less. It leaves the services' own start and the
# runner's failure recovery as they were; it is a floor, not a limit.
FLOOR_SECONDS = 60
MAX_SECONDS = 3600
# The thermal reading buffer: 10 rows per second plus an initial and a terminal row, 56 bytes each.
THERMAL_ROWS_PER_SECOND, THERMAL_EXTRA_ROWS, THERMAL_ROW_BYTES = 10, 2, 56
CRUMB_WRAP_SECONDS = (1 << 29) * 1024 / 24_000_000

# Names of the terms, shared with tools/a733-crumbs.py through include/a733_admission_refusal.h.
TERMS = {
    'return_reserve': 'return_reserve_us is not shorter than the session lifetime',
    'run_deadline': 'run_us ends later than the lifetime minus return_reserve_us',
    'lifetime_range': 'the session lifetime is outside 1..MAX_SECONDS',
}


class LifetimeError(ValueError):
    def __init__(self, term, message):
        super().__init__(message)
        self.term = term


def declared(manifest):
    """(run_us, return_reserve_us) of a manifest dictionary: JSON deadlines or the decoded binary fields."""
    d = manifest.get('deadlines')
    if d is not None:
        return int(d.get('run_us', 0)), int(d.get('return_reserve_us', 0))
    return int(manifest['run_deadline_us']), int(manifest['return_reserve_us'])


def required_seconds(run_us, reserve_us):
    """Whole seconds the manifest needs: startup, setup, the run and the return reserve."""
    need_us = (STARTUP_SECONDS + SETUP_SECONDS) * US + run_us + reserve_us
    return -(-need_us // US)


def derive(manifest):
    """The lifetime in seconds for an application manifest (dictionary)."""
    run_us, reserve_us = declared(manifest)
    seconds = max(FLOOR_SECONDS, required_seconds(run_us, reserve_us))
    if seconds > MAX_SECONDS:
        raise LifetimeError('lifetime_range', 'the manifest needs a %d s session (startup %d s, setup %d s, run_us %d, '
                            'return_reserve_us %d); the longest session is %d s (tools/session_lifetime.py)' % (
                                seconds, STARTUP_SECONDS, SETUP_SECONDS, run_us, reserve_us, MAX_SECONDS))
    return seconds


def check(run_us, reserve_us, seconds):
    """Raise LifetimeError naming the term the OS session decoder would refuse, as apps/os/session.c does."""
    if not 1 <= seconds <= MAX_SECONDS:
        raise LifetimeError('lifetime_range', 'the image lifetime of %d s is outside 1..%d s' % (seconds, MAX_SECONDS))
    lifetime_us = seconds * US
    if reserve_us >= lifetime_us:
        raise LifetimeError('return_reserve', 'manifest return_reserve_us %d is not shorter than the %d s session lifetime' % (
            reserve_us, seconds))
    if run_us > lifetime_us - reserve_us:
        raise LifetimeError('run_deadline', 'manifest run_us %d plus return_reserve_us %d exceeds the %d s session lifetime '
                            'of this image by %d us; the decoder needs a lifetime of at least %d s, and the build derives %d s '
                            'with the %d s startup and %d s setup allowance' % (
                                run_us, reserve_us, seconds, run_us + reserve_us - lifetime_us,
                                -(-(run_us + reserve_us) // US), max(FLOOR_SECONDS, required_seconds(run_us, reserve_us)),
                                STARTUP_SECONDS, SETUP_SECONDS))


def thermal_rows(seconds):
    return seconds * THERMAL_ROWS_PER_SECOND + THERMAL_EXTRA_ROWS


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--manifest', type=Path, help='application manifest JSON; print the derived lifetime in seconds')
    parser.add_argument('--get', choices=('floor', 'max', 'startup', 'setup'))
    args = parser.parse_args(argv)
    if args.get:
        print({'floor': FLOOR_SECONDS, 'max': MAX_SECONDS, 'startup': STARTUP_SECONDS, 'setup': SETUP_SECONDS}[args.get])
        return 0
    if args.manifest:
        try:
            print(derive(json.loads(args.manifest.read_text())))
        except LifetimeError as error:
            print('error: %s: %s' % (args.manifest, error), file=sys.stderr)
            return 1
        return 0
    parser.error('give --manifest or --get')


if __name__ == '__main__':
    sys.exit(main())
