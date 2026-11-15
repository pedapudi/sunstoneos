#!/usr/bin/env python3
"""Check that admission refuses a radio image whose result record version the runner's decoder does not read."""
import importlib.util
from pathlib import Path
import struct
import types

import harness

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


images = load('radio_record_images', ROOT / 'tools/board_images.py')
decoder = load('radio_record_decoder', ROOT / 'tools/radio_result.py')
header = (ROOT / 'include/a733_radio_result.h').read_text()
written = int(header.split('#define A733_RADIO_RESULT_VERSION ')[1].split('u')[0])
assert written in decoder.VERSIONS, 'the decoder must read the version this tree writes'

names = {'os_radio_result_descriptor': (0x1000, 24), 'os_radio_result_version': (0x1018, 4)}
exported = {}
images.symbol_bytes = lambda image, name: struct.pack('<I', exported[image])

# An image from before the version symbol writes version 2, which this decoder reads.
assert images.radio_record_admission('old.elf', {'os_radio_result_descriptor': (0x1000, 24)}) == 2
# The version this tree writes is admitted.
exported['current.elf'] = written
assert images.radio_record_admission('current.elf', names) == written
# A runner whose decoder predates it refuses the image and names both versions.
exported['newer.elf'] = written
try:
    images.radio_record_admission('newer.elf', names, decoder=types.SimpleNamespace(VERSIONS=(2,)))
except ValueError as error:
    assert 'version %d' % written in str(error) and 'decodes versions 2' in str(error), error
else:
    raise AssertionError('an image with a record version the decoder lacks was admitted')
# A version no decoder knows is refused by this one too.
exported['future.elf'] = max(decoder.VERSIONS) + 1
try:
    images.radio_record_admission('future.elf', names)
except ValueError as error:
    assert 'version %d' % (max(decoder.VERSIONS) + 1) in str(error), error
else:
    raise AssertionError('an unknown record version was admitted')
print('PASS: admission refuses a radio image whose record version the runner cannot decode, naming both versions')
