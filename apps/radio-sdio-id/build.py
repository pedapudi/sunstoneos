#!/usr/bin/env python3
"""Build the returning SDIO identity diagnostic from supplied hardware facts."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from returning_build import ROOT, PROVIDER_UNITS, platform_arguments, platform_from
from host_evidence import new_directory, unique_directory

SCHEMA = 'a733-radio-sdio-id-v1'
FACTS = {
    'topology': ('fitted_board_routes_verified', 'enabled_3v3_vbat_1v8_io'),
    'pins': ('complete_pm_latch_authority', 'other_pm_data_writers_excluded', 'pm2_reset_lpo_stable',
             'pg0_pg5_pm0_pm1_exclusive'),
    'startup': ('fabric_parent_gates_stable', 'sdc1_gate_off_reset_asserted',
                'store_ahb_mbus_msi_lite1_ready', 'sd_monitor_autogating_disabled',
                'device_mappings_security_verified', 'sdc1_irq_and_other_owners_excluded'),
    'bootstrap': ('awake_bootstrap', 'no_outstanding_debug_or_stale_confirmations'),
}


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def integer(value, low, high, name):
    require(type(value) is int and low <= value <= high, 'invalid ' + name)
    return value


def profile_source(path):
    """Encode explicit hardware inputs; a build does not measure the supplied facts."""
    raw = path.read_bytes()
    record = json.loads(raw)
    require(record.get('schema') == SCHEMA, 'radio authority schema')
    four = integer(record.get('four_bit'), 0, 1, 'four_bit')
    facts, inputs = {}, {str(path.resolve()): raw}
    for group, required in FACTS.items():
        entry = record.get(group, {})
        require(isinstance(entry, dict), 'missing ' + group + ' hardware inputs')
        if 'path' in entry:
            # Older profiles may keep values in separate files. Recorded review
            # flags and hashes are provenance, not permission to build.
            source = Path(entry['path'])
            data = source.read_bytes()
            fact = json.loads(data)
            inputs[str(source.resolve())] = data
        else:
            fact = entry
        require(all(fact.get(k) is True for k in required), group + ' required fact absent')
        facts[group] = fact
    routes = facts['topology'].get('rails', {})
    require(routes.get('vbat') == 'dcdc1', 'VBAT must use observed DCDC1')
    rail = {'dcdc1': 0, 'dcdc8': 1, 'bldo5': 2, 'cldo1': 3}
    require(all(routes.get(k) in ('dcdc8', 'bldo5', 'cldo1') for k in ('pg', 'pm', 'vddio')),
            'exact PG/PM/VDDIO topology absent')
    pins = facts['pins']
    require(pins.get('latch_origin') in ('verified-writer-history', 'verified-reset-contract'),
            'PM input DATA sampling cannot establish output latch')
    latch = integer(pins.get('pm_latch'), 0, 63, 'complete PM latch')
    arrays = []
    for name in ('expected', 'desired'):
        entries = pins.get(name)
        require(isinstance(entries, list) and len(entries) == 8, 'eight ' + name + ' pin configurations required')
        values = []
        for i, item in enumerate(entries):
            require(isinstance(item, list) and len(item) == 3, 'pin configuration must have function/pull/drive')
            f, p, d = [integer(x, 0, n, name) for x, n in zip(item, (15, 2, 3))]
            require(f in (0, 15) if name == 'expected' else f == (2 if i < 6 else 0 if i == 6 else 1),
                    name + ' pin function')
            values.append('{%d,%d,%d}' % (f, p, d))
        arrays.append('{' + ','.join(values) + '}')
    startup = facts['startup']
    fabric, rates = startup.get('fabric'), startup.get('fabric_hz')
    require(isinstance(fabric, list) and len(fabric) == 3 and isinstance(rates, list) and len(rates) == 3,
            'AHB/NSI/MBUS snapshots and rates required')
    parents = ((24, 0, 0, 600, 0, 0, 0, 0), (24, 0, 800, 600, 480, 0, 0, 0),
               (24, 600, 0, 480, 400, 0, 0, 0))
    for i, (f, hz) in enumerate(zip(fabric, rates)):
        integer(f, 0, 0xffffffff, 'fabric'); integer(hz, 1, 800000000, 'fabric rate')
        mask = 0x0300001f if i == 0 else 0x8700001f
        parent, div = parents[i][(f >> 24) & 7] * 1000000, (f & 31) + 1
        require(not f & ~mask and (not i or f & 0x80000000) and parent and parent % div == 0 and hz == parent // div,
                'fabric encoding/rate differs')
    require(not four or startup.get('peri0_400mhz_branch_verified') is True, 'four-bit parent evidence absent')
    pad = integer(facts['bootstrap'].get('pad'), 1, 2, 'source pad policy')
    authority = int.from_bytes(hashlib.sha256(raw).digest()[:8], 'little')
    require(authority != 0, 'zero authority digest')
    init = f'''#include "profile.h"
const struct radio_id_profile radio_id_profile = {{
 .magic=RADIO_ID_MAGIC,.authority={authority}ULL,.four_bit={four},
 .pm_latch={{{authority}ULL,RADIO_ID_OWNER,63,{latch}}},
 .platform={{.owner=RADIO_ID_OWNER,.handoff={authority}ULL,
  .topology={{{authority}ULL,{','.join(str(rail[routes[k]]) for k in ('vbat','pg','pm','vddio'))}}},
  .fabric={{{','.join(hex(v) for v in fabric)}}},.fabric_hz={{{','.join(str(v) for v in rates)}}},
  .expected={arrays[0]},.desired={arrays[1]},.card={{0x00300000,100000,1000}}}},
 .transport={{AIC8800_TRANSPORT_D80_BOOTSTRAP,{pad},1000,100000,1000}},
 .bootstrap={{{authority}ULL,0x40500058,0x40,1,1}}
}};
'''
    return init, inputs, record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', type=Path, required=True)
    select = p.add_mutually_exclusive_group()
    select.add_argument('--profile', '--authority', dest='profile', type=Path, help='explicit wiring, latch, fabric and bootstrap inputs')
    select.add_argument('--refusal-image', action='store_true', help='zero authority; refuses before provider initialization')
    platform_arguments(p)
    a = p.parse_args()
    # Complete validation precedes build configuration and provider generation.
    source, inputs, record = ('#include "profile.h"\nconst struct radio_id_profile radio_id_profile = {0};\n', {}, None)
    if a.profile:
        source, inputs, record = profile_source(a.profile)
    e = a.evidence.resolve()
    require(ROOT not in e.parents and e != ROOT, 'external evidence directory required')
    e = new_directory(e)
    (e / 'profile.c').write_text(source)
    for i, (name, data) in enumerate(inputs.items()):
        (e / f'authority-{i}.json').write_bytes(data)
    platform = platform_from(a, e)
    units = ['apps/radio-sdio-id/main.c', 'apps/board-bundles-power/power.c', 'apps/board-bundles-power/trace.c',
             'drivers/power/a733_power_domains.c', 'drivers/power/a733_power_domains_native.c',
             'drivers/net/a733_radio_platform.c', 'drivers/net/a733_radio_platform_native.c',
             'arch/aarch64/device_memory.c',
             'drivers/storage/sunxi_sdio.c', 'drivers/storage/sdio_card.c', 'drivers/storage/storage_digest.c',
             'drivers/net/aic8800_sdio_transport.c', 'drivers/net/aic8800_sdio_codec.c',
             'drivers/net/aic8800_firmware.c', 'drivers/net/aic8800_runtime.c', 'drivers/net/aic8800_wifi.c',
             'drivers/net/aic8800_security.c', 'drivers/net/aic8800_data.c']
    platform.compile([ROOT / x for x in units] + [e / 'profile.c'],
                     ['-I' + str(ROOT / 'apps/radio-sdio-id'), '-I' + str(ROOT / 'apps/board-bundles-power')])
    platform.reuse(list(PROVIDER_UNITS))
    platform.link(); platform.binary(); platform.run()
    platform.record(__file__, {'execution_admitted': False, 'refusal_image': not bool(a.profile),
        'authority': record, 'authority_inputs': {name: hashlib.sha256(data).hexdigest() for name, data in inputs.items()},
        'profile_supplied': bool(a.profile)})
    print(f'Built radio SDIO diagnostic: {e}')


if __name__ == '__main__':
    main()
