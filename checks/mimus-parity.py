#!/usr/bin/env python3
"""Compare one recorded-audio FreeRTOS result with its host transcription reference."""
import argparse
import hashlib
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference', type=Path, required=True)
parser.add_argument('--candidate', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
reference = json.loads(args.reference.read_text())
candidate = json.loads(args.candidate.read_text())
if reference['exit'] != 0: raise SystemExit('The host reference failed.')
rows = [json.loads(line) for line in reference['stdout'].splitlines()]
if len(rows) != 2 or rows[1]['id'] != '#end':
    raise SystemExit('Expected one reference recording followed by its formatting flush.')
if 'complete' in candidate:
    if not candidate['complete']: raise SystemExit('Physical execution did not complete.')
    log = args.candidate.with_name('kernel.log').read_text()
else:
    if candidate['exit'] != 0: raise SystemExit('Emulated execution failed.')
    log = candidate['stderr']
if 'PASS mimus:' not in log: raise SystemExit('Candidate lacks its completion marker.')
if candidate.get('ram_input_sha256', candidate.get('bundle_sha256')) != reference['bundle_sha256']:
    raise SystemExit('Candidate input bundle differs from the reference.')
metrics = re.search(r'^RESULT (.*)$', log, re.M)
if metrics is None: raise SystemExit('Missing recognition metrics.')
fields = dict(item.split('=') for item in metrics[1].split())
expected = rows[0]
for key in ['feat_crc', 'lp_crc', 'frames']:
    if fields[key] != str(expected[key]): raise SystemExit(f'{key} differs.')
for name, key in [('TEXT', 'text'), ('MARKS', 'marks'), ('CAPS', 'caps')]:
    found = re.search(r'^' + name + r' (.*)$', log, re.M)
    if not found or found[1] != expected[key]: raise SystemExit(f'{name} differs.')
formatted = re.search(r'^FORMATTED (.*)$', log, re.M)
if not formatted or formatted[1] != expected['typed'] + rows[1]['typed']:
    raise SystemExit('Formatted text differs.')
record = dict(passed=True, scope='one recorded bench fixture', fields=fields,
              reference_sha256=hashlib.sha256(args.reference.read_bytes()).hexdigest(),
              candidate_sha256=hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
              image_sha256=candidate['image_sha256'], bundle_sha256=reference['bundle_sha256'])
args.output.write_text(json.dumps(record, indent=2) + '\n')
print('PASS: features, log probabilities, frame count, words, punctuation, capitals and formatted text match.')
