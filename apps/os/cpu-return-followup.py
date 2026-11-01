#!/usr/bin/env python3
"""Verify a CPU return's result and its rail helper before a caller-owned continuation.

After a normal CPU return, CPU-B runs at 1014 MHz with DCDC3 at 800 mV and
DCDC5 at 1000 mV. Board bundle 33, the rail helper, verifies every boot clock
word and restores DCDC5 to 800 mV; helper_result checks its kernel log.
"""
import hashlib
import importlib.util
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('followup_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def returned_result(record, image, raw, epoch, generation):
    host.require(record.get('complete') is True and record.get('dram_restored') is True and
                 record.get('transport_failed') is False, 'Successful restored return receipt required.')
    host.require(record.get('output_sha256') == hashlib.sha256(raw).hexdigest(), 'Copied result differs from receipt.')
    info = host.image_manifest(image)
    return host.decode_result(raw, epoch, generation, info['manifest']['output_limit'])


COMPLETE = ('COMPLETE board bundle 33: DCDC5 restored to 800 mV with DCDC3 '
            'already at 800 mV and all boot clock words')


def helper_result(record, log):
    """The rail helper's kernel-runner record and log of one normal completion."""
    require = host.require
    require(record.get('complete') is True and record.get('transport_failed') is False and
            record.get('automatic_access') is True and record.get('mode') == 'kernel' and
            record.get('result', {}).get('magic') == 0x534b4552 and
            record['result'].get('stage') == 5 and record['result'].get('status') == 0 and
            'controlled' not in record and log.splitlines().count(COMPLETE) == 1 and
            not re.search(r'^(?:FAIL|REFUSED|STOPPED)\b', log, re.M), 'Rail helper did not complete normally.')
    supply = re.findall(r'^SUPPLY (.+)$', log, re.M)
    require(len(supply) == 1, 'Helper entry supplies absent or ambiguous.')
    fields = dict(x.split('=') for x in supply[0].split())
    require(int(fields['dcdc3_uv'], 0) == 800000 and int(fields['dcdc5_uv'], 0) == 1000000 and
            int(fields['bin'], 0) == 4 and int(fields['dcxo_hz'], 0) == 26000000,
            'Helper entry supply/bin/reference differs.')
    volt = re.findall(r'^VOLT (.+)$', log, re.M)
    require(len(volt) == 1, 'Helper must have exactly one rail operation.')
    fields = dict(x.split('=') for x in volt[0].split())
    require(set(fields) == {'dcdc5_selector', 'dcdc5_uv'} and int(fields['dcdc5_uv'], 0) == 800000 and
            int(fields['dcdc5_selector'], 0) & 127 == 30, 'Helper rail readback differs.')
    for name, pll, mux, mhz in [('CPU-L', 0xf8802600, 0x03010000, 494),
                              ('CPU-B', 0xf8802700, 0x03000000, 1014),
                              ('DSU', 0xf8802600, 0x03010105, 494)]:
        for phase in ('before', 'after'):
            rows = re.findall(r'^' + name + ' ' + phase + r' (.+)$', log, re.M)
            require(len(rows) == 1, 'Helper clock inspection absent or ambiguous.')
            f = {k: int(v, 0) for k, v in (x.split('=') for x in rows[0].split())}
            require(f['pll'] == pll and f['mux'] == mux and f['mhz'] == mhz and
                    not (f['pat0'] & (3 << 29) or f['pat1'] >> 31 or f['lfm'] >> 31),
                    'Helper boot clock or pattern/LFM differs.')
    return {'boot_clock_domains': 3, 'dcdc3_uv': 800000, 'dcdc5_uv': 800000,
            'proof_scope': 'Exact helper pre-return inspections/readback and status-zero completion; no post-ROM analog measurement.',
            'helper_ram': 'retired-overwritten', 'dram_restored': False}
