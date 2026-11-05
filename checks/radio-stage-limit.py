#!/usr/bin/env python3
"""The radio stage limit is a submit-time option: parsed and refused by board.py, frozen in the queue entry, and written into
the prepared radio profile (tools/radio_scan_prepare.py), with no constant in a daemon file.

Run it with the venv python that board.py needs (pyelftools)."""
import argparse
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
import board
import radio_scan_prepare as prep


def parse(*arguments):
    parser = argparse.ArgumentParser()
    board.run_options(parser, True)
    return parser.parse_args(['/dev/null', '--port', 'any', '--kind', 'cpu-return', *arguments])


# The default is a full run; each defined stage parses.
assert parse().radio_stage_limit == 0
for limit in prep.STAGE_LIMITS:
    assert parse('--radio-stage-limit', str(limit)).radio_stage_limit == limit
# Values the profile does not define are refused at the command line.
for bad in ('5', '-1', 'x', '1.5', ''):
    try:
        parse('--radio-stage-limit', bad)
    except SystemExit:
        continue
    raise SystemExit('accepted --radio-stage-limit ' + repr(bad))
# The queue entry carries the option, and the preparation receives it.
assert 'radio_stage_limit' in board.FROZEN_OPTIONS
images_source = (root / 'tools/board_images.py').read_text()
assert "stage_limit=getattr(args, 'radio_stage_limit', 0)" in images_source and 'scan.apply(' in images_source
assert 'STAGE_LIMIT =' not in (root / 'tools/radio_scan_prepare.py').read_text()
# The prepared profile has the limit, and refuses one it does not define.
firmware = [(0x43010000 + 0x10000 * i, name, bytes(size)) for i, (name, size, _) in enumerate(prep.FIRMWARE)]
prefix = bytearray(592)
prefix[prep.SESSION['topology']:prep.SESSION['topology'] + 8] = (101).to_bytes(8, 'little')
for limit in prep.STAGE_LIMITS:
    profile = prep.profile_bytes(bytearray(prefix), bytes(range(1, 33)), firmware, limit)
    assert profile[prep.STAGE_LIMIT_OFFSET] == limit and len(profile) == prep.PROFILE_BYTES
assert prep.profile_bytes(bytearray(prefix), bytes(range(1, 33)), firmware)[prep.STAGE_LIMIT_OFFSET] == 0
for bad in (5, -1, True, 'a', None):
    try:
        prep.profile_bytes(bytearray(prefix), bytes(range(1, 33)), firmware, bad)
    except ValueError:
        continue
    raise SystemExit('profile accepted stage limit ' + repr(bad))
print('PASS radio stage limit: option parsed and refused, frozen in the entry, written into the prepared profile')
