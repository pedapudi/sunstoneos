#!/usr/bin/env python3
"""Decode the short read-only region workload and its SD owner wait snapshots."""
import struct

TAG_DONE = 0x4C52544F4E45
SD_MAGIC = 0x53444F57414B4531
SOURCE_BYTES = 512 << 20
STATS = struct.Struct('<3Q13I4x')


def stats(payload, offset):
    words = STATS.unpack_from(payload, offset)
    return dict(elapsed_us=words[0], asleep_us=words[1], wakes=words[2],
                irq_cores=words[3], owner_cores=words[4], caller_cores=words[5],
                irq_entries=list(words[6:14]), irq_stray=words[14], irq_recheck=words[15])


def decode(payload):
    if len(payload) != 512:
        raise ValueError('The region result must contain 512 bytes.')
    words = struct.unpack_from('<27Q', payload)
    magic, idle_us = struct.unpack_from('<2Q', payload, 216)
    if magic != SD_MAGIC or not idle_us:
        raise ValueError('The result lacks the SD wait observations.')
    before, after = stats(payload, 232), stats(payload, 312)
    if after['wakes'] < before['wakes'] or after['elapsed_us'] < before['elapsed_us']:
        raise ValueError('The SD observations move backward.')
    wakes = after['wakes'] - before['wakes']
    return dict(tag=words[0], state=words[1], error=words[2], file_bytes=words[3], load_us=words[4],
                switch_rounds=words[5], switch_same_ticks=words[6], switch_different_ticks=words[7],
                control_ticks=list(words[8:18]), control_bytes=words[18],
                full_ticks=list(words[19:22]), full_bytes=words[22],
                copy_us=words[25], digest_us=words[26], sd_before=before, sd_after=after,
                retired_idle_us=idle_us, retired_idle_wakes=wakes,
                retired_idle_wakes_per_second=wakes * 1e6 / idle_us)


def failures(decoded):
    errors = []
    if (decoded['tag'] != TAG_DONE or decoded['state'] != 2 or decoded['error'] or
            decoded['file_bytes'] != SOURCE_BYTES):
        errors.append('The complete card file did not pass its digest check.')
    if decoded['retired_idle_us'] < 1000000 or decoded['retired_idle_wakes']:
        errors.append('The retired loader wakes during its one-second idle observation.')
    after = decoded['sd_after']
    if after['irq_cores'] != 1 or sum(after['irq_entries']) < SOURCE_BYTES // 131072:
        errors.append('The SD completion interrupt count or routing differs.')
    if after['owner_cores'] != 1 or after['caller_cores'] != 1 or after['irq_stray'] or after['irq_recheck']:
        errors.append('SD owner placement, stray interrupts or completion deadlines differ.')
    if after['asleep_us'] > after['elapsed_us']:
        errors.append('The SD sleep duration exceeds the observation.')
    return errors
