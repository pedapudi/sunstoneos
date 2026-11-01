#!/usr/bin/env python3
"""Decode complete read-only supply logs without granting electrical admission."""
import argparse
import json
from pathlib import Path
import re

if not __debug__:
    raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE')

RAILS = ('dcdc1', 'dcdc8', 'dcdc9', 'bldo5', 'cldo1', 'cldo3')
# include/a733_rail_leases.h enum a733_rail; linked producer checks bind these IDs.
RAIL_IDS = (0, 7, 8, 19, 20, 22)
FIELDS = ('rail', 'page before', 'chip', 'revision', 'enable', 'selector',
          'page after', 'decoded', 'enabled', 'programmed microvolts')


def decode(log):
    lines = log.splitlines()
    if (lines.count('BEGIN native supply observation') != 1 or
            lines.count('COMPLETE native supply observations') != 1 or
            any(line.startswith('INCOMPLETE ') for line in lines)):
        raise ValueError('complete supply observation required')
    cursor = lines.index('BEGIN native supply observation') + 1
    records = []
    for name, rail_id in zip(RAILS, RAIL_IDS):
        begin = 'SUPPLY_BEGIN=' + name
        if begin not in lines[cursor:]:
            raise ValueError('missing supply block: ' + name)
        at = lines.index(begin, cursor)
        if any(line.startswith(('SUPPLY_BEGIN=', 'SUPPLY_END=')) for line in lines[cursor:at]):
            raise ValueError('unexpected supply block')
        fields = {}
        for offset, key in enumerate(FIELDS, 1):
            text = lines[at + offset] if at + offset < len(lines) else ''
            match = re.fullmatch(re.escape(key) + r'=0x([0-9a-fA-F]{16})', text)
            if not match:
                raise ValueError('invalid supply field: ' + key)
            fields[key] = int(match[1], 16)
        cursor = at + len(FIELDS) + 1
        if cursor >= len(lines) or lines[cursor] != 'SUPPLY_END=' + name:
            raise ValueError('incomplete supply block: ' + name)
        cursor += 1
        if fields['rail'] != rail_id:
            raise ValueError('invalid supply rail identity: ' + name)
        if (fields['page before'] or fields['page after'] or fields['chip'] != 3 or fields['revision'] or
                any(fields[k] > 255 for k in ('enable', 'selector')) or
                any(fields[k] > 1 for k in ('decoded', 'enabled')) or
                (not fields['decoded'] and fields['programmed microvolts'])):
            raise ValueError('invalid supply identity or value')
        records.append({'name': name, **fields})
    if (any(line.startswith(('SUPPLY_BEGIN=', 'SUPPLY_END=')) for line in lines[cursor:]) or
            lines[cursor:].count('completed supplies=0x0000000000000006') != 1 or
            'COMPLETE native supply observations' not in lines[cursor:]):
        raise ValueError('invalid supply completion')
    return {'schema': 'a733-supply-observations-v1', 'observations': records,
            'electrical_admission': False,
            'limits': 'Programmed bytes only; physical voltage and fitted rail routing remain unproved.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(decode(args.log.read_text()), indent=2))


if __name__ == '__main__':
    main()
