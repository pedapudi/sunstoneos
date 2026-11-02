#!/usr/bin/env python3
"""Machine-readable board evidence records for the subsystem status files.

One JSON Lines file per subsystem lives in docs/evidence/. See docs/evidence.md.

  evidence.py add ENTRY_ID --subsystem S --measure name=value[unit] ...
  evidence.py validate             check schema, queue agreement and status claims
  evidence.py sync [--check]       rewrite (or compare) the table in each status file
  evidence.py table S              print the table for one subsystem
  evidence.py migrate STATUS_MD    move a prose status file to the short format
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
               'hypothesis', 'reference', 'outcome', 'verdict', 'measurements', 'conditions', 'evidence', 'note')


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
            if record.get('entry_id'):
                for need in ('port', 'image_sha256', 'git_revision'):
                    if not record.get(need):
                        problems.append(f'{where}: a queue record requires {need}')
                if queue and (Path(queue) / 'entries' / record['entry_id'] / 'run/result.json').exists():
                    problems += queue_disagreements(record, Path(queue), where)
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
            'entry_id': entry_id, 'port': entry.get('port'),
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
    """Every queue entry a migrated status file cites must have a record."""
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


# ---------------------------------------------------------------------------
# Migration of a prose status file.



def paragraphs(text):
    return [part.strip() for part in re.split(r'\n\s*\n', text) if part.strip()]


def flat(paragraph):
    return ' '.join(paragraph.split())


UNITS = r'(?:us|µs|ms|ns|s|MHz|GHz|mV|V|MiB|KiB|GiB|bytes|jobs/s|jobs|%|x|×)'


def snake(text):
    return re.sub(r'[^a-z0-9]+', '_', text.lower()).strip('_')


def cells(line):
    return [item.strip() for item in line.strip().strip('|').split('|')]


def table_measurements(text, entry):
    """Named measurements from the Markdown table row that cites the entry, if any."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not line.startswith('|') or entry not in line:
            continue
        top = index
        while top > 0 and lines[top - 1].startswith('|'):
            top -= 1
        if top + 1 >= index:
            return []
        headers, values, found = cells(lines[top]), cells(line), []
        for header, value in zip(headers[1:], values[1:]):
            base, _, unit = header.partition(',')
            unit = unit.strip()
            base = re.sub(r'`', '', base).strip()
            tail = re.search(r'\s(' + UNITS + r')$', base)
            if tail and not unit:
                base, unit = base[:tail.start()].strip(), tail.group(1)
            value = value.replace(',', '') if re.fullmatch(r'[\d,. /]+', value) else value
            parts = [item.strip() for item in base.split('/')]
            numbers = [item.strip() for item in value.split('/')]
            if len(parts) > 1 and len(numbers) == len(parts) and all(re.fullmatch(r'-?\d+(?:\.\d+)?', n) for n in numbers):
                prefix = ' '.join(parts[0].split()[:-1])
                names = [snake(prefix + ' ' + part.split()[-1]) for part in parts]
                found += [{'name': n, 'value': split_value(v)[0], 'unit': unit} for n, v in zip(names, numbers)]
            elif re.fullmatch(r'-?\d+(?:\.\d+)?', value) and unit:
                found.append({'name': snake(base), 'value': split_value(value)[0], 'unit': unit})
            else:
                for word, number, unit_text in re.findall(r'(?:([A-Za-z]+)\s+)?(-?\d[\d,]*(?:\.\d+)?)\s*(' + UNITS + r')\b', value):
                    name = snake(word if word and word.lower() not in ('and', 'the', 'with') else base)
                    found.append({'name': name or snake(base), 'value': split_value(number.replace(',', ''))[0], 'unit': unit_text})
        return found
    return []


def cmd_migrate(args):
    root = Path(args.root)
    source = Path(args.status)
    source = source if source.is_absolute() else root / source
    stem = source.stem
    text = source.read_text()
    if BEGIN in text:
        raise SystemExit(f'{source} already uses the evidence format.')
    stamp = args.date or datetime.date.today().strftime('%Y%m%d')
    history = root / 'docs/history' / f'status-{stem}-{stamp}.md'
    history.parent.mkdir(exist_ok=True)
    title = re.match(r'# ([^\n]+)', text).group(1)
    # The old file moves unchanged so relative links stay valid (docs/history has the same depth).
    history.write_text(
        f'# {title}: historical status record {stamp[:4]}-{stamp[4:6]}-{stamp[6:]}\n\n'
        f'This log holds the prose evidence of [the current status](../status/{stem}.md) as it stood on the '
        f'date above. It is not updated. Board results that have queue entries are in the records '
        f'`docs/evidence/{stem}.jsonl`.\n\n'
        + re.sub(r'\]\((?=[A-Za-z0-9_-]+\.md)', '](../status/', re.sub(r'\A# [^\n]+\n', '', text)).lstrip('\n'))
    # Records for every queue entry the file cites.
    queue = Path(args.queue)
    cited = list(dict.fromkeys(ENTRY.findall(text)))
    flat_text = flat(text)
    created = []
    for entry in cited:
        if not (queue / 'entries' / entry / 'entry.json').exists():
            print(f'warning: queue entry {entry} is not in {queue}; it stays in the historical log only')
            continue
        record = queue_record(queue, entry, stem)
        sentences = [item for item in re.split(r'(?<=[.!?])\s+', flat_text) if entry in item]
        record['note'] = ' '.join(sentences)[:600]
        record['measurements'] += [item for item in table_measurements(text, entry)
                                   if item['name'] not in {m['name'] for m in record['measurements']}]
        append_record(root, record, replace=True)
        created.append(entry)
    intro = re.split(r'^#{2,} ', text, maxsplit=1, flags=re.M)[0]
    status = re.search(r'^\*\*Status: [^\n]+\*\*[ \t]*$', intro, re.M)
    scope = re.search(r'^\*\*Board-qualified scope:\*\* [^\n]+(?:\n(?!\n|#|\*\*)[^\n]+)*', intro, re.M)
    summary = paragraphs(intro.split(status.group(0), 1)[1] if status else '')
    summary = [flat(item) for item in summary[:1] if not item.startswith('**Board') and len(item.splitlines()) <= 6]
    # Open work is never inferred: copy an existing section or leave a placeholder.
    section = re.search(r'^## (?:Open work|Next (?:steps|actions)|Remaining work)[^\n]*\n(.*?)(?=^## |\Z)',
                        text, re.M | re.S | re.I)
    if section and section.group(1).strip():
        open_work = section.group(1).strip().splitlines()
    else:
        open_work = ['1. Open work has not been written; see the historical log.']
        print(f'warning: {source.name} has no open-work section; wrote a placeholder to replace by hand')
    out = [f'# {title}', '', status.group(0) if status else '**Status: implemented.**', '',
           *([summary[0], ''] if summary else []),
           scope.group(0) if scope else '**Board-qualified scope:** None.', '',
           '## Open work', '']
    out += open_work
    out += ['', '## Evidence', '',
            f'Board results with a queue entry are records in `docs/evidence/{stem}.jsonl`, added with `tools/evidence.py add`. '
            f'Results cited by path, and earlier prose evidence, are in [the historical log](../history/{history.name}).', '',
            render_block(stem, load_all(root).get(stem, [])), '']
    source.write_text('\n'.join(out))
    print(f'{source.relative_to(root)}: {len(text.splitlines())} -> {len(out)} lines, '
          f'{len(created)} records, history {history.relative_to(root)}')


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
    migrate = commands.add_parser('migrate')
    migrate.add_argument('status')
    migrate.add_argument('--date')
    migrate.set_defaults(run=cmd_migrate)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
