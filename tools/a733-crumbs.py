#!/usr/bin/env python3
"""Decode returning-image breadcrumbs and arm or read WDT0 over FEL.

The layout and stage names come from include/sunstone_crumb.h, so the image
and this decoder cannot disagree silently. No device access happens here:
callers pass the FEL transaction function they already own.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'include/sunstone_crumb.h'
RTC = 0x07090000
WORDS = {'tag': 0x100, 'last': 0x104, 'time': 0x10c, 'fault': 0x110, 'esr': 0x114, 'elr': 0x118, 'far': 0x11c}
WDT_MODE, WDT_CONFIG, WDT_OUTPUT, WDT_RESTART = 0x02050014, 0x02050010, 0x02050018, 0x0205000c
KEY = 0x16aa0000
RETURN_ROM = 0x0503  # SUNSTONE_CRUMB_RETURN_ROM: value is the WDT0 mode after the disarm
# Watchdog interval for mode bits 7:4 (drivers/watchdog/a733_watchdog.c).
INTERVALS = {0: 0.5, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 8, 8: 10, 9: 12, 10: 14, 11: 16}


def header():
    """Word offsets, magic values and stage names from the C header."""
    text = HEADER.read_text()
    defines = {m[1]: int(m[2], 16) for m in re.finditer(r'#define SUNSTONE_CRUMB_(\w+) (0x[0-9a-f]+)u?\b', text)}
    stages = {int(m[2], 16): m[1] for m in re.finditer(r'SUNSTONE_CRUMB_(\w+) = (0x[0-9a-f]+),', text)}
    return defines, stages


def interval(mode):
    return INTERVALS[(mode >> 4) & 15]


def arm_arguments(mode):
    """One FEL invocation: clear the crumb words (never 0x108, the FEL flag), then
    the measured WDT0 order: reset output, system reset, mode, restart."""
    args = []
    for offset in WORDS.values():
        args += ['writel', hex(RTC + offset), '0x0']
    for address, value in ((WDT_OUTPUT, KEY | 2), (WDT_CONFIG, KEY | 1), (WDT_MODE, KEY | mode),
                           (WDT_RESTART, 0x14af)):
        args += ['writel', hex(address), hex(value)]
    return args


def return_arguments():
    """One FEL invocation on the returned instance: mode, disarm, mode again, crumbs."""
    args = ['readl', hex(WDT_MODE), 'writel', hex(WDT_MODE), hex(KEY), 'readl', hex(WDT_MODE)]
    for offset in WORDS.values():
        args += ['readl', hex(RTC + offset)]
    return args


def parse_return(stdout):
    values = [int(x, 16) for x in stdout.split()]
    if len(values) != 2 + len(WORDS):
        raise ValueError('Unexpected watchdog return readback.')
    return values[0], values[1], dict(zip(WORDS, values[2:]))


def decode(words, epoch):
    """Summarize the seven words; a tag of another session yields no stages."""
    defines, stages = header()
    def stage(word):
        number = word >> 16
        return {'stage': number, 'name': stages.get(number, 'UNKNOWN'), 'value': word & 0xffff}
    result = {'words': {k: hex(v) for k, v in words.items()}}
    tag = words['tag']
    result['session'] = (tag >> 16 == defines['TAG_MAGIC'] and (tag & 0xffff) == (epoch & 0xffff))
    if not result['session']:
        result['summary'] = ('no breadcrumb of this session: the image stopped before its EL3 entry '
                             'or did not write RTC words')
        return result
    last, time, fault = words['last'], words['time'], words['fault']
    if last:
        result['last'] = stage(last)
        result['last']['core'] = time >> 29
        result['last']['seconds_since_power_on'] = round((time & 0x1fffffff) * 1024 / 24e6, 4)
    if fault >> 24 == defines['FAULT_MAGIC']:
        level = (fault >> 16) & 15
        result['fault'] = {'core': (fault >> 20) & 15, 'level': level, 'vector_slot': (fault >> 8) & 255,
                           'x0': fault & 255, 'esr' if level == 3 or fault & 255 == 4 else 'detail': hex(words['esr']),
                           'elr': hex(words['elr']), 'far': hex(words['far'])}
    parts = []
    if 'last' in result:
        r = result['last']
        parts.append(f"last stage {r['name']} ({r['stage']:#06x}) value {r['value']:#x} on core {r['core']}")
    else:
        parts.append('tagged at EL3 entry; no later stage')
    if 'fault' in result:
        f = result['fault']
        kind = 'EL1 fault' if f['level'] == 1 and f['x0'] == 4 else (
            f"exit status {f['x0']}" if f['level'] == 1 else 'EL3 exception')
        parts.append(f"first {kind} on core {f['core']}: " +
                     ', '.join(f'{k} {f[k]}' for k in ('esr', 'detail', 'elr', 'far') if k in f))
    result['summary'] = '; '.join(parts)
    return result
