"""Decode the bounded eight-core diagnostic; no device access."""
import struct

CORES = 8
ROUNDS = 4096
WORDS = 64
MAGIC = 0x31594c5243505543
MASK = (1 << 64) - 1

def inputs():
    return []

def _digests(rounds):
    # Independent host arithmetic, including explicit uint64 wraparound.
    result = []
    for consumer in range(CORES):
        producer = (consumer - 1) % CORES
        total = 0
        for round in range(1, rounds + 1):
            for index in range(WORDS):
                value = ((producer + 1) << 48) ^ (round << 16) ^ index
                value = ((value ^ (value >> 30)) * 0xbf58476d1ce4e5b9) & MASK
                value = ((value ^ (value >> 27)) * 0x94d049bb133111eb) & MASK
                total = (total + (value ^ (value >> 31))) & MASK
        result.append(total)
    return result

def _decode(payload, expected_rounds):
    if len(payload) != 224:
        raise ValueError('CPU reliability result size differs from 224 bytes')
    words = struct.unpack('<28Q', payload)
    magic, rounds, mask, ticks = words[:4]
    if (magic, rounds, mask) != (MAGIC, expected_rounds, 0xff):
        raise ValueError('CPU reliability header differs')
    if ticks < rounds:
        raise ValueError('CPU reliability interval has too few ticks')
    counts, digests, waits = words[4::3], words[5::3], words[6::3]
    if any(count != rounds for count in counts):
        raise ValueError('CPU reliability has incomplete rounds')
    if list(digests) != _digests(rounds):
        raise ValueError('CPU reliability payload digest differs')
    return {'rounds': rounds, 'core_mask': mask, 'digests': list(digests)}

def decode(payload):
    return _decode(payload, ROUNDS)

def expected(values):
    if values:
        raise ValueError('CPU reliability takes no input regions')
    return {'rounds': ROUNDS, 'core_mask': 0xff, 'digests': _digests(ROUNDS)}
