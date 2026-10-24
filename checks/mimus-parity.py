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
if 'complete' in candidate:
    if not candidate['complete']: raise SystemExit('Physical execution did not complete.')
    log = args.candidate.with_name('kernel.log').read_text()
else:
    if candidate['exit'] != 0: raise SystemExit('Emulated execution failed.')
    log = candidate.get('stdout', '') + candidate['stderr']
if 'PASS mimus:' not in log: raise SystemExit('Candidate lacks its completion marker.')
if candidate.get('ram_input_sha256', candidate.get('bundle_sha256')) != reference['bundle_sha256']:
    raise SystemExit('Candidate input bundle differs from the reference.')
if bool(reference.get('paced', False)) != bool(candidate.get('paced', False)):
    raise SystemExit('Reference and candidate recognition modes differ.')
if reference.get('paced'):
    def stable_records(text):
        if 'ERROR ' in text or 'PASS mimus: paced utterance replay completed' not in text:
            raise SystemExit('Paced execution did not complete successfully.')
        records = []
        fields_by_kind = {
            'SEGMENT': ('id', 'begin', 'end', 'speech_begin', 'speech_end'),
            'RESULT': ('feat_crc', 'lp_crc', 'frames', 'samples'),
            'REPLAY': ('samples', 'segments', 'consumed', 'state'),
        }
        for line in text.splitlines():
            kind, _, value = line.partition(' ')
            if kind in fields_by_kind:
                fields = dict(item.split('=') for item in value.split())
                records.append((kind, tuple(fields[key] for key in fields_by_kind[kind])))
            elif kind in ('TEXT', 'MARKS', 'CAPS', 'FORMATTED', 'FLUSH'):
                records.append((kind, value))
        if sum(kind == 'REPLAY' for kind, _ in records) != 1 or sum(kind == 'FLUSH' for kind, _ in records) != 1:
            raise SystemExit('Paced output lacks its unique summary or formatting flush.')
        return records
    expected = stable_records(reference['stdout'])
    actual = stable_records(log)
    if actual != expected:
        raise SystemExit('Paced boundaries, recognition, formatting, or completion differs.')
    record = dict(passed=True, scope='paced segmentation and per-utterance reference parity; timing excluded',
                  segments=sum(kind == 'SEGMENT' for kind, _ in actual),
                  reference_sha256=hashlib.sha256(args.reference.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
                  image_sha256=candidate['image_sha256'], bundle_sha256=reference['bundle_sha256'])
    args.output.write_text(json.dumps(record, indent=2) + '\n')
    print('PASS: paced segment boundaries, features, log probabilities, words, punctuation and formatted output match.')
    raise SystemExit(0)
rows = [json.loads(line) for line in reference['stdout'].splitlines()]
if len(rows) != 2 or rows[1]['id'] != '#end':
    raise SystemExit('Expected one reference recording followed by its formatting flush.')
metrics = re.search(r'^RESULT (.*)$', log, re.M)
if metrics is None: raise SystemExit('Missing recognition metrics.')
fields = dict(item.split('=') for item in metrics[1].split())
expected = rows[0]
for key in ['feat_crc', 'lp_crc', 'frames']:
    if fields[key] != str(expected[key]): raise SystemExit(f'{key} differs.')
# A model without a punctuation head has no marks or capitals in the host
# reference, and the candidate must then print neither line.
for name, key in [('TEXT', 'text'), ('MARKS', 'marks'), ('CAPS', 'caps')]:
    found = re.search(r'^' + name + r' (.*)$', log, re.M)
    if key in expected:
        if not found or found[1] != expected[key]: raise SystemExit(f'{name} differs.')
    elif key == 'text' or found:
        raise SystemExit(f'{name} differs from a reference without it.')
formatted = re.search(r'^FORMATTED (.*)$', log, re.M)
if not formatted or formatted[1] != expected['typed'] + rows[1]['typed']:
    raise SystemExit('Formatted text differs.')
record = dict(passed=True, scope='one recorded bench fixture', fields=fields,
              reference_sha256=hashlib.sha256(args.reference.read_bytes()).hexdigest(),
              candidate_sha256=hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
              image_sha256=candidate['image_sha256'], bundle_sha256=reference['bundle_sha256'])
args.output.write_text(json.dumps(record, indent=2) + '\n')
print('PASS: features, log probabilities, frame count, words, punctuation, capitals and formatted text match.')
