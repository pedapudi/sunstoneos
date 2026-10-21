#!/usr/bin/env python3
"""Maintain checks/index.json and run the pure-host checks that cover changed paths.

The index is derived from each check's own text: the first docstring line, the
component documents that name the check, required command-line arguments, the
local cache areas it reads, and whether it can reach USB. A check is pure-host
when it is a runnable script that needs no USB access, no cached evidence or
build output, and no required argument other than a new output directory.

  check-index.py --write                 regenerate checks/index.json
  check-index.py --verify                fail when the index misses, invents or misdescribes a check
  check-index.py --changed PATH...       list checks whose text or fixtures name a changed path
  check-index.py --changed PATH... --run --output DIR
                                         also run the selected pure-host checks, one new
                                         output directory per check under DIR

Coverage is textual: a check covers a path when the check, or a checks/ file
it names (followed transitively), names that path or one of its parent
directories, or when the path is reached through #include from a named source.
Changed paths are relative to the repository root; a directory selects every
check that covers a path below it.
Paths assembled from separate string segments are not found, so an empty
selection is not proof that no check covers a change.
"""
import argparse
import ast
import functools
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
INDEX = ROOT / 'checks/index.json'
# Required options the runner may satisfy with a new, empty directory.
OUTPUT_OPTIONS = {'--output'}
# Top-level ~/.cache/sunstoneos area named by a path literal or by a CACHE/cache variable.
CACHE_AREA = re.compile(r"""(?:\.cache/sunstoneos['"]?\s*/\s*['"]?|\b(?i:cache)\s*/\s*['"])([a-z][a-z0-9-]*)""")
SIBLING = re.compile(r"""with_name\(\s*['"]([\w.-]+\.py)['"]|['"]checks/([\w.-]+\.py)['"]""")
# Device access: pyusb, the board lock and queue, FEL, and the tools that launch or prepare a board.
USB = re.compile(r"""^\s*(?:import|from)\s+usb\b|usb-access|usb_guard|/dev/bus/usb|sunxi-fel|board_lock|board-test-queue|"""
                 r"""tools/(?:run|usb|prepare)-|sunstoneos-a733-""", re.M)
PATH_TOKEN = re.compile(r"""[A-Za-z0-9_.+-]+(?:/[A-Za-z0-9_.+-]+)*""")
INCLUDE = re.compile(r"""^\s*#\s*include\s+"([^"]+)\"""", re.M)
# Cache areas that hold no evidence or build output: toolchains, pinned sources, the host venv.
NEUTRAL_AREAS = {'host', 'sources', 'venv'}
# Checks the text scan would call pure-host but that cannot run without arguments.
NOT_PURE = {'radio-network.py': 'every case needs the optional --tcp, --kernel, --headers and --evidence paths'}


def checks():
    return sorted(p for p in (ROOT / 'checks').glob('*.py') if p.name not in ('harness.py', 'check-index.py'))


def mentions():
    """Map each check name to the tracked Markdown documents that name it."""
    names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', 'docs', '*.md'],
                                    cwd=ROOT, text=True).split()
    texts = {name: (ROOT / name).read_text(errors='replace') for name in names
             if name.endswith('.md') and not name.startswith('docs/audit/') and (ROOT / name).is_file()}
    return lambda check: sorted(name for name, text in texts.items() if check in text)


def arguments(tree):
    """Required options and positional parameters declared through argparse.

    Options declared in a loop such as `for name in ('image', 'output'):
    add_argument('--' + name, ...)` are expanded from the loop's literal values.
    """
    loops = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.For) and isinstance(node.target, ast.Name):
            try:
                values = ast.literal_eval(node.iter)
            except ValueError:
                continue
            if all(isinstance(v, str) for v in values):
                for child in ast.walk(node):
                    loops[id(child)] = (node.target.id, values)
    required = []
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == 'add_argument'
                and node.args):
            continue
        first = node.args[0]
        keywords = {k.arg: k.value for k in node.keywords}
        flag = isinstance(keywords.get('required'), ast.Constant) and keywords['required'].value is True
        if isinstance(first, ast.Constant) and isinstance(first.value, str):
            optional_positional = isinstance(keywords.get('nargs'), ast.Constant) and keywords['nargs'].value in ('?', '*')
            if flag or (not first.value.startswith('-') and not optional_positional and 'default' not in keywords):
                required.append(first.value)
        elif flag:
            loop = loops.get(id(node))
            if (loop and isinstance(first, ast.BinOp) and isinstance(first.op, ast.Add) and isinstance(first.left, ast.Constant)
                    and isinstance(first.right, ast.Name) and first.right.id == loop[0]):
                required += [first.left.value + value for value in loop[1]]
            else:
                required.append(ast.unparse(first))
    return required


def library(tree, text):
    """A module that only defines helpers for other checks."""
    if "__name__" in text or 'sys.argv' in text:
        return False
    plain = (ast.Import, ast.ImportFrom, ast.FunctionDef, ast.ClassDef, ast.Assign, ast.AnnAssign, ast.Expr)
    return all(isinstance(node, plain) and not (isinstance(node, ast.Expr) and not isinstance(node.value, ast.Constant))
               for node in tree.body)


def scan(path, seen=()):
    """Arguments, cache areas and USB use, including checks this one executes as source."""
    text = path.read_text()
    tree = ast.parse(text)
    required, areas, usb = arguments(tree), set(CACHE_AREA.findall(text)), bool(USB.search(text))
    if 'exec(compile(' in text:
        for name in sorted({a or b for a, b in SIBLING.findall(text)} - set(seen)):
            if (path.parent / name).is_file():
                more = scan(path.parent / name, (*seen, path.name))
                required += [r for r in more[0] if r not in required]
                areas |= more[1]
                usb |= more[2]
    return required, areas, usb


def entry(path, documents):
    text = path.read_text()
    tree = ast.parse(text)
    doc = ast.get_docstring(tree) or ''
    required, areas, usb = scan(path)
    areas = sorted(areas)
    uses_argv = 'sys.argv[' in text
    is_library = library(tree, text)
    pure = (not is_library and not usb and not uses_argv and not set(areas) - NEUTRAL_AREAS
            and set(required) <= OUTPUT_OPTIONS and path.name not in NOT_PURE)
    return {'summary': doc.splitlines()[0] if doc else '', 'docs': documents(path.name),
            'required_arguments': required + (['sys.argv'] if uses_argv else []), 'cache_areas': areas,
            'usb': usb, 'library': is_library, 'pure_host': pure}


def generate():
    documents = mentions()
    return {path.name: entry(path, documents) for path in checks()}


@functools.lru_cache(maxsize=None)
def named(path, includes_only):
    """Repository files and subdirectories one file names; only #include targets for non-check sources."""
    text = path.read_text(errors='replace')
    tokens = set(INCLUDE.findall(text)) if includes_only else set(PATH_TOKEN.findall(text)) | set(INCLUDE.findall(text))
    found = set()
    for token in tokens:
        if '/' not in token and '.' not in token:
            continue
        for base in (ROOT, path.parent, ROOT / 'include'):
            candidate = Path(os.path.normpath(base / token))
            if candidate.is_relative_to(ROOT) and candidate.parent != ROOT.parent and candidate.exists():
                # A top-level directory such as drivers/ or checks/ would select everything under it.
                if not (candidate.is_dir() and candidate.parent == ROOT) and candidate != ROOT:
                    found.add(candidate)
                break
    if not includes_only and re.search(r'^import harness$', text, re.M):
        found.add(ROOT / 'checks/harness.py')
    return frozenset(found)


def covered(path):
    """Paths named by a check, followed through the checks/ files it names and their #include closure."""
    seen, stack = set(), [path]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        if current.is_file() and current.suffix in ('.py', '.c', '.cc', '.h', '.S', '.inc'):
            stack.extend(named(current, current.parent != ROOT / 'checks'))
    return {p.relative_to(ROOT).as_posix() for p in seen}


def select(index, changed):
    changed = [Path(os.path.normpath(ROOT / p)).relative_to(ROOT).as_posix() for p in changed]
    chosen = []
    for name in sorted(index):
        names = covered(ROOT / 'checks' / name)
        dirs = {n for n in names if (ROOT / n).is_dir()}
        if any(c in names or any(c.startswith(d + '/') for d in dirs) or any(n.startswith(c + '/') for n in names)
               for c in changed):
            chosen.append(name)
    return chosen


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--write', action='store_true')
    action.add_argument('--verify', action='store_true')
    action.add_argument('--changed', nargs='+', metavar='PATH')
    parser.add_argument('--run', action='store_true', help='run the selected pure-host checks')
    parser.add_argument('--output', type=Path, help='new directory for --run results')
    parser.add_argument('--timeout', type=int, default=900, help='seconds per check')
    args = parser.parse_args()
    if args.write:
        INDEX.write_text(json.dumps(generate(), indent=1, sort_keys=True) + '\n')
        return
    index = json.loads(INDEX.read_text())
    if args.verify:
        current = generate()
        missing = sorted(set(current) - set(index))
        invented = sorted(set(index) - set(current))
        stale = sorted(name for name in set(index) & set(current) if index[name] != current[name])
        for label, names in (('unindexed', missing), ('absent', invented), ('stale', stale)):
            for name in names:
                print(f'{label}: checks/{name}')
        if missing or invented or stale:
            raise SystemExit('checks/index.json is out of date; regenerate it with checks/check-index.py --write')
        print(f'PASS: {len(index)} checks indexed, {sum(e["pure_host"] for e in index.values())} pure-host')
        return
    chosen = select(index, args.changed)
    runnable = [name for name in chosen if index[name]['pure_host']]
    for name in chosen:
        e = index[name]
        needs = 'pure-host' if e['pure_host'] else 'needs ' + ', '.join(
            ['library'] * e['library'] + ['USB'] * e['usb'] + e['required_arguments'] +
            ['cache/' + a for a in e['cache_areas'] if a not in NEUTRAL_AREAS]) or 'needs inputs'
        print(f'checks/{name}: {needs}')
    if not args.run:
        return
    if args.output is None:
        raise SystemExit('--run requires --output')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    results = {}
    for name in runnable:
        command = [sys.executable, str(ROOT / 'checks' / name)]
        if '--output' in index[name]['required_arguments']:
            command += ['--output', str(out / Path(name).stem)]
        try:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, env=harness.environment(), timeout=args.timeout)
            output, code = result.stdout + result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            output, code = (error.stdout or b'') + (error.stderr or b'') + b'\nDeclared check deadline expired.\n', 124
        (out / (Path(name).stem + '.log')).write_bytes(output)
        results[name] = {'command': command, 'exit': code}
        print(f'{"PASS" if code == 0 else "FAIL"} checks/{name}')
    harness.save(out / 'results.json', {'changed': args.changed, 'selected': chosen, 'ran': results})
    if any(r['exit'] for r in results.values()):
        raise SystemExit('one or more selected checks failed; logs are under ' + str(out))


if __name__ == '__main__':
    main()
