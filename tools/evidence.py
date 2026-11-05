#!/usr/bin/env python3
"""Machine-readable board evidence records for the subsystem status files.

One JSON Lines file per subsystem lives in docs/evidence/. See docs/evidence.md.

  evidence.py add ENTRY_ID --subsystem S --measure name=value[unit] ...
  evidence.py validate             check schema, queue agreement and status claims
  evidence.py sync [--check]       rewrite (or compare) the table in each status file
  evidence.py table S              print the table for one subsystem
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = 'docs/evidence'
SCHEMA = ROOT / EVIDENCE / 'schema.json'
DEFAULT_QUEUE = Path('~/.cache/sunstoneos/board-queue').expanduser()
ENTRY = re.compile(r'\b\d{19}-[0-9a-f]{8}\b')
BEGIN, END = '<!-- evidence-table:begin -->', '<!-- evidence-table:end -->'
FIELD_ORDER = ('id', 'subsystem', 'date', 'entry_id', 'port', 'image_sha256', 'git_revision',
               'design', 'image', 'hypothesis', 'prediction', 'reference', 'outcome', 'verdict', 'measurements', 'conditions', 'conclusion', 'narrative', 'evidence', 'note')


def load_schema():
    return json.loads(SCHEMA.read_text())


def check_value(value, schema, where, problems):
    """Validate the JSON Schema subset the record schema uses."""
    kinds = schema.get('type')
    if kinds is not None:
        kinds = kinds if isinstance(kinds, list) else [kinds]
        tests = {'string': lambda v: isinstance(v, str),
                 'number': lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
                 'object': lambda v: isinstance(v, dict), 'array': lambda v: isinstance(v, list),
                 'null': lambda v: v is None}
        if not any(tests[kind](value) for kind in kinds):
            problems.append(f'{where}: expected {"/".join(kinds)}')
            return
    if value is None:
        return
    if 'enum' in schema and value not in schema['enum']:
        problems.append(f'{where}: {value!r} is not one of {schema["enum"]}')
    if 'pattern' in schema and isinstance(value, str) and not re.fullmatch(schema['pattern'], value):
        problems.append(f'{where}: {value!r} does not match {schema["pattern"]}')
    if 'minLength' in schema and isinstance(value, str) and len(value) < schema['minLength']:
        problems.append(f'{where}: empty')
    if isinstance(value, dict):
        for name in schema.get('required', ()):
            if name not in value:
                problems.append(f'{where}: missing {name}')
        properties = schema.get('properties', {})
        for name, item in value.items():
            if name in properties:
                check_value(item, properties[name], f'{where}.{name}', problems)
            elif 'additionalProperties' in schema and isinstance(schema['additionalProperties'], dict):
                check_value(item, schema['additionalProperties'], f'{where}.{name}', problems)
            elif schema.get('additionalProperties') is False:
                problems.append(f'{where}: unknown field {name}')
    if isinstance(value, list) and 'items' in schema:
        for index, item in enumerate(value):
            check_value(item, schema['items'], f'{where}[{index}]', problems)


def read_records(path):
    records, problems = [], []
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip():
            continue
        try:
            records.append((number, json.loads(line)))
        except ValueError as error:
            problems.append(f'{path}:{number}: invalid JSON ({error})')
    return records, problems


def evidence_files(root):
    return sorted((Path(root) / EVIDENCE).glob('*.jsonl'))


def validate_records(root, queue=None):
    """Return problems in every record file: schema, uniqueness, queue agreement."""
    schema = load_schema()
    problems, seen = [], {}
    for path in evidence_files(root):
        records, bad = read_records(path)
        problems += bad
        for number, record in records:
            where = f'{path.name}:{number}'
            check_value(record, schema, where, problems)
            if not isinstance(record, dict):
                continue
            if record.get('subsystem') != path.stem:
                problems.append(f'{where}: subsystem {record.get("subsystem")!r} is not {path.stem!r}')
            if record.get('id') in seen:
                problems.append(f'{where}: duplicate id {record.get("id")} (also {seen[record["id"]]})')
            seen[record.get('id')] = where
            problems += extension_problems(root, where, record)
            if record.get('entry_id'):
                for need in ('port', 'image_sha256', 'git_revision'):
                    if not record.get(need):
                        problems.append(f'{where}: a queue record requires {need}')
                if queue and (Path(queue) / 'entries' / record['entry_id'] / 'run/result.json').exists():
                    problems += queue_disagreements(record, Path(queue), where)
    return problems


REFERENCE = re.compile(r'(docs/[^#\s]+\.(?:md|jsonl))(?:#(\S+))?')


def heading_anchors(path):
    """The anchors of a Markdown file: GitHub heading slugs and explicit <a id> tags."""
    text = Path(path).read_text()
    found = set(re.findall(r'<a id="([^"]+)"', text))
    for heading in re.findall(r'^#{1,6}\s+(.+?)\s*$', text, re.M):
        slug = re.sub(r'[^\w\s-]', '', re.sub(r'[`*]', '', heading).lower()).strip().replace(' ', '-')
        found.add(slug)
    return found


def reference_problems(root, where, record):
    """A reference of the form docs/PATH.md#ANCHOR (or PATH.jsonl#ID) must resolve."""
    match = REFERENCE.fullmatch(record.get('reference') or '')
    if not match:
        return []
    path, anchor = match.groups()
    target = Path(root) / path
    if not target.exists():
        return [f'{where}: reference {path} does not exist']
    if not anchor:
        return []
    if path.endswith('.jsonl'):
        known = {item.get('id') for _, item in read_records(target)[0]}
    else:
        known = heading_anchors(target)
    return [] if anchor in known else [f'{where}: reference anchor #{anchor} not found in {path}']


def extension_problems(root, where, record):
    """Rules for the prediction, conclusion and narrative fields."""
    problems = reference_problems(root, where, record)
    if record.get('prediction') and not record.get('conclusion'):
        problems.append(f'{where}: a record with a prediction requires a conclusion')
    if not record.get('entry_id') and not record.get('measurements') and not record.get('narrative'):
        problems.append(f'{where}: a record without a queue entry needs measurements or a narrative')
    design = record.get('design')
    if design and not (Path(root) / design.split('#')[0]).exists():
        problems.append(f'{where}: design {design} does not exist')
    return problems


def queue_disagreements(record, queue, where):
    result = json.loads((queue / 'entries' / record['entry_id'] / 'run/result.json').read_text())
    problems = []
    for mine, theirs in (('image_sha256', 'image_sha256'), ('git_revision', 'git_revision'),
                         ('port', 'port'), ('outcome', 'outcome')):
        if result.get(theirs) is not None and record.get(mine) != result[theirs]:
            problems.append(f'{where}: {mine} {record.get(mine)!r} differs from result.json {result[theirs]!r}')
    return problems


def split_value(text):
    match = re.fullmatch(r'\s*(-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\s*(.*?)\s*', text)
    if not match:
        raise SystemExit(f'Measurement value {text!r} must be a number followed by an optional unit.')
    number = float(match.group(1))
    return (int(number) if number.is_integer() and '.' not in match.group(1) and 'e' not in match.group(1).lower()
            else number), match.group(2)


def parse_measure(text):
    name, separator, value = text.partition('=')
    if not separator or not re.fullmatch(r'[a-z][a-z0-9_]*', name):
        raise SystemExit(f'Measurement {text!r} must read name=value[unit] with a lowercase snake_case name.')
    number, unit = split_value(value)
    return {'name': name, 'value': number, 'unit': unit}


def default_verdict(outcome, result):
    if outcome == 'returned':
        return 'passed' if result.get('normal_return') is not False else 'failed'
    if outcome == 'refused-before-launch':
        return 'inconclusive'
    return 'failed'


def queue_record(queue, entry_id, subsystem):
    directory = Path(queue) / 'entries' / entry_id
    entry = json.loads((directory / 'entry.json').read_text())
    result_path = directory / 'run/result.json'
    result = json.loads(result_path.read_text()) if result_path.exists() else {}
    stamp = result.get('time') or entry.get('submitted')
    outcome = result.get('outcome', 'not-run')
    reference = entry.get('reference')
    if isinstance(reference, dict):
        reference = f"none: {reference['reason']}" if reference.get('none') else f"{reference['path']}#{reference['anchor']}"
    conditions = {'kind': entry.get('kind')} if entry.get('kind') else {}
    if result.get('dram_mode'):
        conditions['dram_mode'] = result['dram_mode']
    measurements = []
    total = (result.get('durations') or {}).get('total')
    if isinstance(total, (int, float)):
        measurements.append({'name': 'run_total', 'value': round(total, 3), 'unit': 's'})
    return {'id': entry_id, 'subsystem': subsystem,
            'date': datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).strftime('%Y-%m-%d') if stamp else None,
            'entry_id': entry_id, 'port': result.get('port') or entry.get('port'),
            'image_sha256': result.get('image_sha256') or entry.get('image_sha256'),
            'git_revision': result.get('git_revision'),
            'hypothesis': entry.get('hypothesis') or '', 'outcome': outcome,
            **({'reference': reference} if reference else {}),
            'verdict': default_verdict(outcome, result), 'measurements': measurements,
            'conditions': conditions,
            'evidence': [str(directory / 'run').replace(str(Path.home()), '~')], 'note': ''}


def ordered(record):
    return {key: record[key] for key in FIELD_ORDER if key in record}


def append_record(root, record, replace=False):
    path = Path(root) / EVIDENCE / f'{record["subsystem"]}.jsonl'
    path.parent.mkdir(parents=True, exist_ok=True)
    records, _ = read_records(path) if path.exists() else ([], [])
    existing = [item for _, item in records]
    if any(item.get('id') == record['id'] for item in existing):
        if not replace:
            raise SystemExit(f'Record {record["id"]} already exists in {path.name}; pass --replace to overwrite it.')
        existing = [item for item in existing if item.get('id') != record['id']]
    existing.append(ordered(record))
    path.write_text(''.join(json.dumps(item, ensure_ascii=False) + '\n' for item in existing))
    return path


def cmd_add(args):
    root = Path(args.root)
    record = queue_record(args.queue, args.entry_id, args.subsystem)
    if args.hypothesis:
        record['hypothesis'] = args.hypothesis
    if args.verdict:
        record['verdict'] = args.verdict
    if args.note:
        record['note'] = args.note
    for field in ('prediction', 'conclusion', 'design'):
        if getattr(args, field):
            record[field] = getattr(args, field)
    if args.date:
        record['date'] = args.date
    record['measurements'] += [parse_measure(item) for item in args.measure]
    for item in args.condition:
        name, separator, value = item.partition('=')
        if not separator:
            raise SystemExit(f'Condition {item!r} must read name=value.')
        record['conditions'][name] = value
    record['evidence'] += args.evidence
    problems = []
    check_value(record, load_schema(), 'record', problems)
    if problems:
        raise SystemExit('\n'.join(problems))
    print(append_record(root, record, args.replace))


def load_all(root):
    result = {}
    for path in evidence_files(root):
        records, _ = read_records(path)
        result[path.stem] = [record for _, record in records]
    return result


def measurement_text(record):
    return ', '.join(f'{item["name"]} {item["value"]:g}{" " + item["unit"] if item["unit"] else ""}'
                     for item in record.get('measurements', []))


def table(records):
    lines = ['| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |',
             '| --- | --- | --- | --- | --- | --- | --- |']
    for record in sorted(records, key=lambda item: (item.get('date') or '', item['id'])):
        image = (record.get('image_sha256') or '')[:12]
        lines.append(f'| `{record["id"]}` | {record.get("date") or ""} | {record.get("port") or ""} | '
                     f'`{image}` | {record["outcome"]} | {record["verdict"]} | {measurement_text(record)} |')
    return '\n'.join(lines)


def render_block(subsystem, records):
    if not records:
        return f'{BEGIN}\nNo board result is recorded in `docs/evidence/{subsystem}.jsonl`.\n{END}'
    return (f'{BEGIN}\n{len(records)} records in `docs/evidence/{subsystem}.jsonl`; '
            f'run `tools/evidence.py sync` to regenerate this table.\n\n{table(records)}\n{END}')


def sync_file(path, records):
    """Return the status text with a current table, or None when the file has no table marker."""
    text = path.read_text()
    if BEGIN not in text or END not in text:
        return None
    head, rest = text.split(BEGIN, 1)
    return head + render_block(path.stem, records) + rest.split(END, 1)[1]


def cmd_sync(args):
    root, stale = Path(args.root), []
    everything = load_all(root)
    for path in sorted((root / 'docs/status').glob('*.md')):
        updated = sync_file(path, everything.get(path.stem, []))
        if updated is not None and updated != path.read_text():
            stale.append(str(path.relative_to(root)))
            if not args.check:
                path.write_text(updated)
    if args.check and stale:
        raise SystemExit('Evidence table out of date in: ' + ', '.join(stale))
    print('stale: ' + ', '.join(stale) if stale else 'tables current')


def claim_problems(root):
    """Every queue entry a status file cites must have a record."""
    root, problems = Path(root), []
    everything = load_all(root)
    known = {record['id'] for records in everything.values() for record in records}
    known |= {record.get('entry_id') for records in everything.values() for record in records}
    for path in sorted((root / 'docs/status').glob('*.md')):
        text = path.read_text()
        if BEGIN not in text:
            continue
        name = str(path.relative_to(root))
        updated = sync_file(path, everything.get(path.stem, []))
        if updated != text:
            problems.append(f'{name}: evidence table does not match docs/evidence/{path.stem}.jsonl')
        for entry in sorted(set(ENTRY.findall(text.split(BEGIN, 1)[0] + text.split(END, 1)[1]))):
            if entry not in known:
                problems.append(f'{name}: cites queue entry {entry} that has no evidence record')
    return problems


def cmd_validate(args):
    queue = Path(args.queue) if Path(args.queue).exists() else None
    problems = validate_records(args.root, queue) + claim_problems(args.root)
    for item in problems:
        print(item)
    if problems:
        raise SystemExit(1)
    count = sum(len(items) for items in load_all(args.root).values())
    print(f'{count} records valid')


def cmd_table(args):
    print(table(load_all(args.root).get(args.subsystem, [])))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', default=str(ROOT))
    parser.add_argument('--queue', default=str(DEFAULT_QUEUE))
    commands = parser.add_subparsers(dest='command', required=True)
    add = commands.add_parser('add', help='record a queue result')
    add.add_argument('entry_id')
    add.add_argument('--subsystem', required=True)
    add.add_argument('--measure', action='append', default=[], metavar='NAME=VALUE[UNIT]')
    add.add_argument('--condition', action='append', default=[], metavar='NAME=VALUE')
    add.add_argument('--evidence', action='append', default=[], metavar='PATH')
    add.add_argument('--verdict', choices=('passed', 'failed', 'inconclusive'))
    add.add_argument('--hypothesis')
    add.add_argument('--note')
    add.add_argument('--prediction')
    add.add_argument('--conclusion')
    add.add_argument('--design')
    add.add_argument('--date')
    add.add_argument('--replace', action='store_true')
    add.set_defaults(run=cmd_add)
    validate = commands.add_parser('validate')
    validate.set_defaults(run=cmd_validate)
    sync = commands.add_parser('sync')
    sync.add_argument('--check', action='store_true')
    sync.set_defaults(run=cmd_sync)
    show = commands.add_parser('table')
    show.add_argument('subsystem')
    show.set_defaults(run=cmd_table)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
