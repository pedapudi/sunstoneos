#!/usr/bin/env python3
"""Host side of hello-compute: its control input, expected result and decoder.

tools/dev-run.py uses this module as the result decoder of the hello-compute
CPU-return control image. decode() reads the 16-byte result that ss_finish
returns (apps/hello-compute/main.c), expected() computes the same result on
the host, and inputs() supplies the input when the command line names none.
"""
import hashlib
import struct
import zlib

RESULT = struct.Struct('<IIQ')
INPUT_BYTES = 1048576   # the manifest's input region


def inputs():
    """One full input region of SHA-256 counter blocks.

    Every byte value occurs, and the sums pass the modular reduction in every
    5552-byte block of the native loop."""
    return [b''.join(hashlib.sha256(i.to_bytes(4, 'little')).digest() for i in range(INPUT_BYTES // 32))]


def decode(payload):
    """{'adler32', 'bytes'} from the result payload; ValueError if malformed."""
    if len(payload) != RESULT.size:
        raise ValueError(f'hello-compute result is {len(payload)} bytes, not {RESULT.size}.')
    adler32, reserved, size = RESULT.unpack(payload)
    if reserved:
        raise ValueError('hello-compute result has a nonzero reserved word.')
    return {'adler32': adler32, 'bytes': size}


def expected(values):
    """The result for these inputs: the Adler-32 (RFC 1950) of the one input."""
    (data,) = values
    return {'adler32': zlib.adler32(data), 'bytes': len(data)}
