"""Validate the returning CPU profile's native session and clock grant.

The caller establishes the live target and owns the transport. Historical
qualification and review documents do not admit the current session.
"""
import importlib.util
from pathlib import Path
import struct

# Retain the public names used by older preparation callers.
SCHEMA = 'a733-os-cpu-return-clock-under-test-v1'
COMPATIBILITY_SCHEMA = 'a733-os-cpu-return-clock-compatibility-under-test-v1'
UNDER_TEST_SCHEMAS = (SCHEMA, COMPATIBILITY_SCHEMA)
MARKER = (0x31544e4552505543, 1, 4, 8)


def validate(a, data, record, image_hash, raw, profile, grant, evidence, require, Elf, *, input_reservation):
    """Check concrete inputs; legacy review arguments remain call-compatible."""
    identity, port = record.get('identity'), record.get('port')
    require(isinstance(identity, list) and len(identity) == 5 and
            all(type(v) is int and v >= 0 for v in identity) and
            isinstance(port, str) and bool(port), 'CPU return target identity absent.')
    raw = bytes(raw)
    require(len(raw) >= 904 and struct.unpack_from('<I', raw, 400)[0] == 2,
            'CPU return requires the staged workload session.')
    if len(raw) == 904 and struct.unpack_from('<Q', raw, 808)[0] == 0x4153373343505531:
        wire_grant, thermal = raw[808:904], raw[592:808]
    else:
        spec = importlib.util.spec_from_file_location('cpu_clock_session_records', Path(__file__).with_name('host.py'))
        host = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(host)
        session = host.session_decode(raw)
        require(host.CPU_GRANT in session['extents'], 'CPU return clock grant absent.')
        wire_grant, thermal = session['extents'][host.CPU_GRANT], session['extents'][host.THERMAL]
    require(any(raw[48:80]), 'CPU return native build identity absent.')
    ids = struct.unpack_from('<3Q', raw, 24)
    require(all(ids) and ids == tuple(profile[k] for k in ('epoch', 'generation', 'lease')) and
            all(record.get(k, profile[k]) == profile[k] for k in ('epoch', 'generation', 'lease')),
            'CPU return session identity differs.')
    require(struct.unpack_from('<3Q', wire_grant) == (0x4153373343505531, 1, 96),
            'CPU return clock grant geometry differs.')
    owner, epoch, generation = struct.unpack_from('<3Q', wire_grant, 24)
    clusters, little, big, changes = struct.unpack_from('<4I', wire_grant, 48)
    require(owner and (epoch, generation) == ids[:2] and clusters == 3 and
            (little, big) == (1794, 2002) and 4 <= changes <= 64 and not any(wire_grant[64:96]),
            'CPU return clock grant limits or owner differ.')
    require(grant['owner'] == owner and grant['clusters'] == clusters and
            tuple(grant['max_mhz']) == (little, big) and grant['changes'] == changes,
            'CPU return clock grant differs from session.')
    expected = {'version': 1, 'workers': 8, 'owner_core': 6, 'core_mask': 255,
                'owner_priority': 3, 'a55_priority': 2, 'a76_priority': 3}
    require(record.get('inference') == expected, 'CPU return inference profile differs.')
    require(len(thermal) == 216 and struct.unpack_from('<I', thermal, 212)[0] == 70000,
            'CPU return thermal configuration differs.')
