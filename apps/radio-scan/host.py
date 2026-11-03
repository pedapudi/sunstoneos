#!/usr/bin/env python3
"""Host side of radio-scan: the 16-byte result that ss_finish returns (apps/radio-scan/main.c).

The full scan result is not in this payload: the runner reads it after the return from the descriptor
at the image symbol os_radio_result_descriptor (tools/radio_result_channel.py)."""
import struct

RESULT = struct.Struct('<4I')
INPUT_BYTES = 0


def inputs():
    return []


def decode(payload):
    """{'stage', 'failed', 'done'} from the result payload; ValueError if malformed."""
    if len(payload) != RESULT.size:
        raise ValueError(f'radio-scan result is {len(payload)} bytes, not {RESULT.size}.')
    stage, failed, done, reserved = RESULT.unpack(payload)
    if reserved or failed > 1 or done != 1:
        raise ValueError('radio-scan result has an invalid field.')
    return {'stage': stage, 'failed': bool(failed), 'done': bool(done)}


def expected(values):
    return {'done': True}
