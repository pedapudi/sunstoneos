#!/usr/bin/env python3
"""The radio stage limit is a submit-time option: parsed and refused by board.py, frozen in the queue entry, and written into
the prepared radio profile (tools/radio_scan_prepare.py), with no constant in a daemon file.

Run it with the venv python that board.py needs (pyelftools)."""
import argparse
import pathlib
import sys
import struct

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
for bad in ('7', '-1', 'x', '1.5', ''):
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
for bad in (7, -1, True, 'a', None):
    try:
        prep.profile_bytes(bytearray(prefix), bytes(range(1, 33)), firmware, bad)
    except ValueError:
        continue
    raise SystemExit('profile accepted stage limit ' + repr(bad))
# The association and address stages declare their entropy unqualified and carry the association's time budget; no other
# stage does. Only the address stage asks for DHCP and carries the network's bounds.
for limit in prep.STAGE_LIMITS:
    profile = prep.profile_bytes(bytearray(prefix), bytes(range(1, 33)), firmware, limit)
    associating = limit in prep.ASSOCIATING_STAGES
    assert profile[prep.ENTROPY_UNQUALIFIED_OFFSET] == (1 if associating else 0)
    budget = [int.from_bytes(profile[prep.PROFILE[name]:prep.PROFILE[name] + 8], 'little')
              for name in ('association_timeout_us', 'refill_timeout_us', 'security_timeout_us', 'command_timeout_us')]
    assert associating == all(budget[1:]), (limit, budget)
    dhcp = profile[prep.PROFILE['network_dhcp']]
    acquisition, lease_cap = struct.unpack_from('<II', profile, prep.PROFILE['address_timeout_ticks'])
    tx = struct.unpack_from('<Q', profile, prep.PROFILE['tx_timeout_us'])[0]
    queue, resolve = struct.unpack_from('<II', profile, prep.PROFILE['queue_timeout_ticks'])
    assert (limit == prep.ADDRESS_STAGE) == bool(dhcp) == bool(acquisition) == bool(lease_cap) == bool(tx) == bool(queue) == \
        bool(resolve), (limit, dhcp, acquisition, lease_cap, tx, queue, resolve)
    if limit == prep.ADDRESS_STAGE:
        # Ticks are milliseconds. DHCP holds two discovers, the first at 250 ms with a 5 s wait for the offer, the second
        # with 10 s; or one discover and a request with its full 5 s wait. The resolve waits for the lease and then one DNS
        # answer, at least 4 s; each frame's transmission and queueing end well inside the resolve.
        assert acquisition >= 250 + 5000 + 5000 and resolve >= acquisition + 4000 and lease_cap > resolve, (acquisition, resolve)
        assert tx < resolve * 1000 and queue < resolve, (tx, queue)
    if associating:
        # Firmware is confirmed near 16.7 s of session time; scan, join and handshake must fit the 50 s startup deadline.
        assert 17_000_000 + budget[0] + budget[1] + budget[2] < 50_000_000
        # The stages nest: the security stage holds an 8 s handshake and one full command wait, and the runtime lease
        # renews before a command's wait could reach its end.
        lease, margin = struct.unpack_from('<QQ', profile, prep.PROFILE['runtime_lease_us'])
        assert budget[3] == 6_000_000 and budget[2] >= 8_000_000 + budget[3] and budget[3] < margin < lease, (budget, margin)
# Submit refuses the association stage without a credential name, before any file or device is touched.
try:
    board.check_wifi_stage(parse('--radio-stage-limit', '5'))
except ValueError:
    pass
else:
    raise SystemExit('association stage accepted without --wifi')
board.check_wifi_stage(parse('--radio-stage-limit', '5', '--wifi', 'name'))
try:
    board.check_wifi_stage(parse('--radio-stage-limit', '6'))
except ValueError:
    pass
else:
    raise SystemExit('address stage accepted without --wifi')
board.check_wifi_stage(parse('--radio-stage-limit', '6', '--wifi', 'name'))
board.check_wifi_stage(parse('--radio-stage-limit', '4'))
print('PASS radio stage limit: option parsed and refused, frozen in the entry, written into the prepared profile')
