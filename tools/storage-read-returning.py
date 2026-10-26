#!/usr/bin/env python3
"""Host side of the returning SD0 read image (apps/storage-read-returning).

  storage-read-returning.py sign --approval APPROVAL --ddr DDR_DIR --survey SURVEY_DIR --signer NAME
  storage-read-returning.py input --approval APPROVAL --ddr DDR_DIR --survey SURVEY_DIR --config CONFIG
                                  --profile 2|3 --output INPUT.bin
  storage-read-returning.py decode --run RUN_DIR --input INPUT.bin --output RESULT.json

sign records, in the coordinator's approval of one lifetime, the SHA-256 of
that lifetime's cold DDR start and returning survey and the signing time. It
refuses a DDR start or survey that did not pass, a survey on another port than
3-3 or on another node or ROM identity than the DDR start, and an approval that
is already signed, so no approval carries into the next lifetime.

input writes the 2192-byte input that the image reads at 0x43000000
(apps/storage-read-returning/sdrt.h): SD profile 2 or 3, fresh owner, handoff
and topology identifiers, the fabric words, the PMIC and PF pin configurations
of CONFIG, and the read plan of check-a733-storage-read-supervised.py's
throughput_plan(0, 4194304): 128 sequential and 96 random eight-sector reads
and 32 single sectors. It refuses unless the approval is signed for this DDR
start and survey, newer than the DDR record, names the profile's bundle and
sets every field that approval-request.json lists for it to true.

decode reads the runner's kernel.log and result record, checks every read line
against the input's plan, and writes the card registers, the selected mode and
the throughput summary that the supervised checker computes from the same
per-read times (throughput_summary). No board, USB or queue access.
"""
import argparse
import hashlib
import importlib.util
import json
from datetime import datetime, timezone
from pathlib import Path
import re
import secrets
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MAGIC = 0x314e525445524453  # "SDRETRN1", apps/storage-read-returning/sdrt.h
VERSION = 1
INPUT_BYTES = 2192
PLAN = 256
PORT = '3-3'
COMPLETE = 'COMPLETE SD0 returning read'
INCOMPLETE = 'INCOMPLETE SD0 returning read: '
BUNDLES = {2: 'A', 3: 'B'}


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def require(ok, message):
    if not ok: raise ValueError(message)


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def throughput():
    """The supervised checker's read plan and summary, unchanged."""
    return module('storage_read_returning_checker', ROOT / 'tools/check-a733-storage-read-supervised.py')


def lifetime(ddr, survey):
    """(identity, node) of a passed cold DDR start and a passed survey after it on port 3-3, same node and identity."""
    ddr_path, survey_path = Path(ddr) / 'session.json', Path(survey) / 'session.json'
    d = json.loads(ddr_path.read_text()); s = json.loads(survey_path.read_text())
    result = d.get('result') or {}
    require(d.get('complete') is True and result.get('stage') == 5 and result.get('board_result') == 0 and
            isinstance(result.get('dram_mib'), int) and result['dram_mib'] >= 64, 'DDR start did not pass')
    identity, node = d.get('node_identity'), d.get('device')
    require(isinstance(identity, list) and len(identity) == 5 and isinstance(node, str) and
            node.startswith('/dev/bus/usb/'), 'DDR start names no USB instance')
    gates = (s.get('result') or {}).get('launcher_gates') or {}
    require(s.get('complete') is True and s.get('sram_restored') is True and s.get('rom_usable') is True and
            gates and all(v is True for v in gates.values()), 'survey did not pass')
    require(s.get('port') == PORT, 'survey ran on another port than 3-3')
    seen = s.get('identity'); seen = json.loads(seen) if isinstance(seen, str) else seen
    require(s.get('node') == node and seen == identity, 'survey ran on another node or ROM identity than the DDR start')
    require(survey_path.stat().st_mtime > ddr_path.stat().st_mtime, 'survey is older than the DDR start')
    return identity, node


def sign(approval, ddr, survey, signer):
    lifetime(ddr, survey)
    value = json.loads(Path(approval).read_text())
    require(signer and value.get('reviewer') and value.get('reviewed_at') and value.get('bundle') in BUNDLES.values(),
            'approval needs its bundle, reviewer, reviewed_at and a signer')
    require(not value.get('signature'), 'approval is already signed for a lifetime')
    value['signature'] = {'signer': signer, 'signed_at': datetime.now(timezone.utc).isoformat(),
                          'ddr_session_sha256': digest(Path(ddr) / 'session.json'),
                          'survey_session_sha256': digest(Path(survey) / 'session.json')}
    Path(approval).write_text(json.dumps(value, indent=1) + '\n')
    return value['signature']


def approved(approval, request, ddr, survey, bundle):
    """The approval's signature binds this lifetime and every requested field is true."""
    value = json.loads(Path(approval).read_text())
    fields = json.loads(Path(request).read_text())['fields'][bundle]
    require(value.get('bundle') == bundle and value.get('reviewer') and value.get('reviewed_at'),
            'approval names another bundle or no reviewer')
    signature = value.get('signature') or {}
    ddr_path = Path(ddr) / 'session.json'
    require(signature.get('ddr_session_sha256') == digest(ddr_path) and
            signature.get('survey_session_sha256') == digest(Path(survey) / 'session.json'),
            'approval is not signed for this lifetime\'s DDR start and survey')
    started = ddr_path.stat().st_mtime
    require(datetime.fromisoformat(signature['signed_at']).timestamp() > started and
            Path(approval).stat().st_mtime > started, 'approval is older than this lifetime\'s DDR start')
    for record, names in fields.items():
        for name in names:
            require(value.get('fields', {}).get(record, {}).get(name) is True, f'{record}.{name} not approved')
    return value


def pins(values, count):
    require(len(values) == count and all(len(v) == 3 and all(type(x) is int and 0 <= x <= 15 for x in v)
                                         for v in values), 'invalid pin configuration')
    return bytes(x for v in values for x in v)


def input_bytes(profile, generation, config, plan, ids=None):
    """The image's input (sdrt.h): header, configuration and plan."""
    require(profile in BUNDLES and type(generation) is int and 0 < generation < 2**64, 'invalid profile or generation')
    require(0 < len(plan) <= PLAN and all(0 <= f < 2**32 and 1 <= b <= 8 for f, b in plan), 'invalid plan')
    owner, handoff, topology = ids or [secrets.randbits(63) | 1 for _ in range(3)]
    for name, count in (('fabric', 3), ('fabric_hz', 3)):
        require(len(config[name]) == count and all(type(v) is int and 0 <= v < 2**32 for v in config[name]),
                'invalid ' + name)
    raw = struct.pack('<4Q3Q4I3I3I', MAGIC, VERSION, INPUT_BYTES, generation, owner, handoff, topology,
                      profile, 0x00300000, len(plan), 0, *config['fabric'], *config['fabric_hz'])
    raw += pins(config['pmic_pins'], 2) + pins(config['expected_pins'], 7) + pins(config['desired_pins'], 7)
    require(len(raw) == 144, 'input header size')
    raw += b''.join(struct.pack('<2I', f, b) for f, b in plan) + bytes(8 * (PLAN - len(plan)))
    require(len(raw) == INPUT_BYTES, 'input size')
    return raw


def parse_input(raw):
    require(len(raw) == INPUT_BYTES, 'input size')
    magic, version, size, generation, owner, handoff, topology, profile, mask, reads, _ = struct.unpack_from('<4Q3Q4I', raw)
    require(magic == MAGIC and version == VERSION and size == INPUT_BYTES and 0 < reads <= PLAN, 'not an SD0 returning input')
    plan = [struct.unpack_from('<2I', raw, 144 + 8 * i) for i in range(reads)]
    return {'generation': generation, 'profile': profile, 'voltage_mask': mask, 'plan': plan}


LINE = re.compile(r'R ([0-9a-f]{3}) ([0-9a-f]{8}) ([0-9a-f]{2}) ([0-9a-f]{8}) ([0-9a-f]{8})')
CARD = re.compile(r'CARD ([0-9a-f]{32}) ([0-9a-f]{32}) ([0-9a-f]{8}) ([0-9a-f]{4}) ([0-9a-f]{16}) ([0-9a-f]{16}) '
                  r'([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{16}) ([0-9a-f]{8})')


def decode_log(log, raw_input, status=None):
    """The reads, card and mode of one log, checked against the input that produced it."""
    given = parse_input(raw_input); plan = given['plan']
    lines = log.splitlines()
    value = {'status': status, 'profile': given['profile'], 'generation': given['generation'], 'complete': False,
             'outcome': None, 'reads': [], 'card': None, 'first_sector_hex': None}
    words = {}
    for line in lines:
        if line.startswith(('SD_', 'SD0 returning start')) and '=' in line:
            key, number = line.split('=', 1)
            words[key] = int(number, 16)
        elif line.startswith('R '):
            match = LINE.fullmatch(line); require(match is not None, 'malformed read line: ' + line)
            index, first, blocks, elapsed, crc = (int(x, 16) for x in match.groups())
            require(index == len(value['reads']) and index < len(plan) and (first, blocks) == tuple(plan[index]),
                    f'read line {index} differs from the input plan')
            value['reads'].append({'first': first, 'blocks': blocks, 'bytes': 512 * blocks, 'elapsed_us': elapsed,
                                   'crc32': crc})
        elif line.startswith('CARD '):
            match = CARD.fullmatch(line); require(match is not None, 'malformed card line')
            g = match.groups()
            mode, hz, uhs = int(g[6], 16), int(g[7], 16), int(g[8], 16)
            value['card'] = {'cid': g[0], 'csd': g[1], 'ocr': int(g[2], 16), 'rca': int(g[3], 16),
                             'sectors': int(g[4], 16), 'scr': g[5], 'profile': mode & 255, 'bus_width': (mode >> 8) & 255,
                             'timing': (mode >> 16) & 255, 'divisor_word': mode >> 24, 'card_hz': hz,
                             'uhs_outcome': uhs & 15, 'signal_1v8': bool(uhs >> 4 & 1), 'uhs_caps': uhs >> 5 & 7,
                             'sample_delay': uhs >> 8 & 255, 'tuning_start': uhs >> 16 & 255, 'tuning_width': uhs >> 24,
                             'tuning_map': g[9], 'identified_us': int(g[10], 16)}
            require(value['card']['profile'] == given['profile'], 'card line names another profile')
        elif line.startswith('D') and len(line) == 131 and line[2] == ' ':
            value['first_sector_hex'] = (value['first_sector_hex'] or '') + line[3:]
        elif line == COMPLETE:
            value['outcome'] = 'complete'
        elif line.startswith(INCOMPLETE):
            value['outcome'] = line[len(INCOMPLETE):]
    value['words'] = words
    require(words.get('SD_READS') == len(value['reads']) and words.get('SD_PLAN') == len(plan), 'read count lines')
    if value['first_sector_hex'] is not None:
        require(len(value['first_sector_hex']) == 1024 and value['reads'], 'first sector dump')
    value['complete'] = value['outcome'] == 'complete' and len(value['reads']) == len(plan) and status in (0, None)
    if value['complete']:
        value['throughput'] = throughput().throughput_summary(value['reads'])
    return value


def decode(run, raw_input):
    session = json.loads((Path(run) / 'session.json').read_text())
    status = (session.get('result') or {}).get('status')
    log = (Path(run) / 'kernel.log').read_text()
    value = decode_log(log, raw_input, status)
    value.update(run=str(Path(run).resolve()), image_sha256=session.get('image_sha256'),
                 returned_node=session.get('returned_node'), physical_access=True)
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='operation', required=True)
    s = sub.add_parser('sign')
    for name in ('approval', 'ddr', 'survey'): s.add_argument('--' + name, type=Path, required=True)
    s.add_argument('--signer', required=True)
    i = sub.add_parser('input')
    for name in ('approval', 'request', 'ddr', 'survey', 'config', 'output'): i.add_argument('--' + name, type=Path, required=True)
    i.add_argument('--profile', type=int, choices=(2, 3), required=True)
    d = sub.add_parser('decode')
    for name in ('run', 'input', 'output'): d.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args(argv)
    if args.operation == 'sign':
        print(json.dumps(sign(args.approval, args.ddr, args.survey, args.signer), indent=1)); return 0
    if args.operation == 'input':
        lifetime(args.ddr, args.survey)
        approved(args.approval, args.request, args.ddr, args.survey, BUNDLES[args.profile])
        require(not args.output.exists(), 'output exists')
        config = json.loads(args.config.read_text())
        raw = input_bytes(args.profile, secrets.randbits(63) | 1, config, throughput().throughput_plan(0, 4194304))
        args.output.write_bytes(raw)
        args.output.with_suffix('.json').write_text(json.dumps({
            'input_sha256': hashlib.sha256(raw).hexdigest(), 'profile': args.profile,
            'approval_sha256': digest(args.approval), 'config_sha256': digest(args.config),
            'ddr_session_sha256': digest(args.ddr / 'session.json'),
            'survey_session_sha256': digest(args.survey / 'session.json'), 'tool_sha256': digest(__file__)}, indent=1) + '\n')
        print(args.output); return 0
    value = decode(args.run, args.input.read_bytes())
    args.output.write_text(json.dumps(value, indent=1) + '\n')
    print(json.dumps({k: value[k] for k in ('status', 'outcome', 'complete')} |
                     ({'throughput': value['throughput']} if value.get('throughput') else {}), indent=1))
    return 0 if value['complete'] else 2


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError, KeyError) as error: sys.exit(f'Stopped: {error}')
