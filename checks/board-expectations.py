#!/usr/bin/env python3
"""Check complete binary queue results with bounded hardware timing fields.

The word layouts follow apps/queue-add/main.c and apps/gpu-lifecycle/main.c.
Only host parsing and result comparison execute; no device is accessed.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import contextlib
import io
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('board_expect', ROOT / 'tools/board.py')
board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(board)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
rows = []


def load(name, words):
    path = args.output / (name + '.json')
    path.write_text(json.dumps({'schema': 'sunstone-board-expect-u32-v1', 'words': words}) + '\n')
    return board.expectation(str(path))


timing = {'min': 0, 'max': 199999}
profiles = {
    'one': [1, 65, 8840, 1, timing, *([0] * 7)],
    'eight': [8, 65, 8840, 8, *([timing] * 8)],
    'cycles': [0x314c5047, 1, 10, 10, 10, 10, 8840, 0, 0, 0, 1, 65, *([timing] * 10)],
    'recovery': [0x314c5047, 2, 2, 1, 1, 2, 8840, 4, 11, 1, 1, 65, timing, *([0] * 9)],
}
for name, words in profiles.items():
    expected = load(name, words)
    values = [word if type(word) is int else 199999 for word in words]
    payload = struct.pack('<' + 'I' * len(values), *values)
    assert board.matches_expectation(expected, payload)
    for index, word in enumerate(words):
        altered = values.copy()
        altered[index] = word + 1 if type(word) is int else 200000
        assert not board.matches_expectation(expected, struct.pack('<' + 'I' * len(altered), *altered))
    assert not board.matches_expectation(expected, payload[:-1])
    assert not board.matches_expectation(expected, payload + bytes(4))
    # Queue serialization freezes the parsed values, not a live file reference.
    assert board.matches_expectation(json.loads(json.dumps(expected)), payload)
    rows.append({'case': name, 'word_mutations': len(words), 'passed': True})

for index, words in enumerate(([True], [-1], [0x100000000], [], [{'min': 2, 'max': 1}],
                               [{'min': False, 'max': 1}], [{'min': 0, 'max': 0x100000000}],
                               [{'min': 0, 'max': 1, 'skip': True}], ['unchecked'])):
    try:
        load('refused-' + str(index), words)
    except ValueError:
        rows.append({'case': 'invalid-' + str(index), 'passed': True})
    else:
        raise AssertionError(('malformed expectation accepted', words))

raw = args.output / 'literal.bin'
raw.write_bytes(b'\x00\x01\xffnative output')
assert board.matches_expectation(board.expectation(str(raw)), raw.read_bytes())
assert not board.matches_expectation(board.expectation(str(raw)), raw.read_bytes() + b'x')
assert board.matches_expectation(board.expectation('PASS native'), b'prefix\nPASS native\n')
assert not board.matches_expectation(board.expectation('PASS native'), b'FAIL native\n')
raw.write_text('{"schema": "application-output"}\n')
assert board.matches_expectation(board.expectation(str(raw)), raw.read_bytes())
rows.append({'case': 'existing-exact-bytes-and-text', 'passed': True})

source = args.output / 'model-image.bin'
source.write_bytes(b'MODEL ONLY - never launch')
for index, expected in enumerate(({'mode': 'u32le', 'words': []},
                                 {'mode': 'u32le', 'words': [True]},
                                 {'mode': 'u32le', 'words': [{'min': False, 'max': 1}]},
                                 {'mode': 'u32le', 'words': [{'min': 0.5, 'max': 1.5}]},
                                 {'mode': 'u32le', 'words': [{'min': 0, 'max': 1, 'skip': True}]},
                                 {'mode': 'u32le', 'words': [0] * 16385},
                                 {'mode': 'u32le', 'words': [0], 'skip': True})):
    frozen = args.output / ('invalid-frozen-' + str(index) + '.json')
    frozen.write_text(json.dumps(expected) + '\n')
    launch = SimpleNamespace(image=source, port='3-3', kind='gpu-return',
                             evidence=args.output / ('preflight-' + str(index)),
                             descriptor=None, reference_inventory=None, host_tree=None,
                             expect_spec=frozen, expect=None)
    entered = []

    def forbidden_backend(*unused):
        entered.append(True)
        raise AssertionError('Malformed frozen expectation reached device construction.')

    with patch.object(board.images, 'inspect', return_value={'entry_guard': False}), \
            patch.object(board.images, 'policy'), contextlib.redirect_stdout(io.StringIO()):
        result = board.run_image(launch, backend=forbidden_backend)
    assert result['outcome'] == 'refused-before-launch' and not entered, result
    assert not result['launch_attempted'] and result['stage'] == 'preflight'
    rows.append({'case': 'frozen-preflight-' + str(index), 'passed': True})
(args.output / 'results.json').write_text(json.dumps({'passed': True, 'physical_access': False,
                                                   'cases': rows}, indent=2) + '\n')
print('PASS binary expectations:', len(rows), 'profiles/refusals and every field mutation')
