#!/usr/bin/env python3
"""Classify every check and run the pure-host checks that cover changed paths.

The index is generated from the checkout on every call and is never stored in
the repository. It is derived from each check's own text: the first docstring line, the
component documents that name the check, required command-line arguments, the
local cache areas it reads, and whether it can reach USB. A check is pure-host
when it is a runnable script that needs no USB access, no cached evidence or
build output, and no required argument other than a new output directory.

  check-index.py --json                  print the index as JSON
  check-index.py --verify                fail when a check cannot be classified or the index is tracked
  check-index.py --changed PATH...       list checks whose text or fixtures name a changed path
  check-index.py --changed PATH... --run --output DIR
                                         also run the selected pure-host checks, one new
                                         output directory per check under DIR

A few checks match the device-access scan by text but open no device; HOST_ONLY lists
them. PROSE_CHECKS marks checks whose failure may be documentation prose alone, and
SERIAL_CHECKS marks pure-host checks that the gate runs one at a time.

Coverage is textual: a check covers a path when the check, or a checks/ file
it names (followed transitively), names that path or one of its parent
directories, or when the path is reached through #include from a named source.
Changed paths are relative to the repository root; a directory selects every
check that covers a path below it.
Paths assembled from separate string segments are not found, so an empty
selection is not proof that no check covers a change.
CHECK_INPUTS supplies supplementary literal repository paths. The hook fixture
uses its complete declared inventory plus the hook, harness and fixture itself,
matching the pre-push hook's conditional fixture policy.
"""
import argparse
import ast
import functools
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
# The index was once a tracked file at this path; --verify refuses its return.
RETIRED_INDEX = 'checks/index.json'
# Checks that the device-access scan matches by text but that open no device. Each
# runs against fixtures; the value is the option that names its new output directory.
HOST_ONLY = {
    'board-runner.py': '--out',           # fake FEL and board under the output directory
    'usb-disconnect.py': '--output',      # sysfs metadata fixtures; no device is opened
    'usb-access-discover.py': '--output', # fixture sysfs tree; no device is opened
}
# Checks whose failure can reflect documentation prose alone. The gate reports
# them as WARN unless --strict-docs is given. True covers the whole check; a tuple
# names the test cases that read repository documents, so that a failure in any
# other case still fails the gate.
PROSE_CHECKS = {
    'docs-links.py': True,
    'progress.py': ('test_inventory_and_evidence', 'test_cache_and_changed_format'),
}
# Pure-host checks that run QEMU or time a result. The gate runs them one at a time,
# after the parallel group, so that other checks do not distort their deadlines.
SERIAL_CHECKS = frozenset({'cpu-pmu-smp.py', 'smp-port-cost.py', 'pcie-qemu.py', 'npu-runtime-bounds.py'})
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
NOT_PURE = {
    'radio-network.py': 'every case needs the optional --tcp, --kernel, --headers and --evidence paths',
    'h264-input-host.py': 'the default run needs --image, --inputs, --native and --launcher',
    'sae-openssl.py': 'the default run needs --openssl, --upstream and --evidence; --verify-source needs a pinned source tree',
}
# Checks whose hardware or vendor-service model has behaviors that cite neither a board measurement nor a
# reference source (docs/validation.md, "Host validation rules"). They keep their files and stay runnable by
# hand, but the gate lists them as pending until the uncited behaviors are replaced by measured ones.
PENDING = 'pending re-qualification against board measurements: '
REQUALIFICATION = {
    'a733-pcie-trace.py': PENDING + 'PCIe controller, PHY, link-training and NVMe latencies and responses are uncited',
    'pcie-qemu.py': PENDING + 'the PCIe, PHY, endpoint and NVMe models it runs are uncited',
    'a733-xhci-detach.py': PENDING + 'xHCI port power at start, port reset latency and TR Dequeue Pointer updates while running are uncited',
    'a733-xhci-dma.py': PENDING + 'the xHCI model shared with a733-xhci-detach has three uncited behaviors',
    'io-native-ordering.py': PENDING + 'TWI status sequence, reset values and delays and posted-store completion are uncited',
    'memtest-machine.py': PENDING + 'SCTLR_EL1 reset, translation walk, DC ZVA and counter step are uncited',
    'power-write-order.py': PENDING + 'the PMIC bus event latency and status after START, USB reset minimums and initial register values are uncited',
    'provider-clock-reference.py': PENDING + 'bit-level healthy values of the USB2, radio, CE and TWI clock and pin fixtures are uncited',
}
# The hook fixtures run only for their actual process inputs. References inside
# harness helpers do not add fixture inputs; this matches pre-push.fixture_selection.
COMPLETE_INPUTS = {'pre-push-hook.py': ('tools/git-hooks/pre-push', 'checks/harness.py')}


def run_environment():
    """The environment of the integration gate: the cached LLVM toolchain leads PATH."""
    environment = harness.environment()
    cache = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))
    environment['PATH'] = str(cache / 'host/usr/lib/llvm-21/bin') + os.pathsep + environment.get('PATH', '')
    environment['SUNSTONEOS_DIR'] = str(ROOT)
    return environment


def checks():
    return sorted(p for p in (ROOT / 'checks').glob('*.py') if p.name not in ('harness.py', 'check-index.py', 'affected.py'))


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
    if path.name in HOST_ONLY:
        areas, usb = set(), False
    areas = sorted(areas)
    uses_argv = 'sys.argv[' in text
    is_library = library(tree, text)
    pure = (not is_library and not usb and not uses_argv and not set(areas) - NEUTRAL_AREAS
            and set(required) <= {HOST_ONLY.get(path.name, '--output')} and path.name not in NOT_PURE and path.name not in REQUALIFICATION)
    option = HOST_ONLY.get(path.name, '--output')
    return {'summary': doc.splitlines()[0] if doc else '', 'docs': documents(path.name),
            'required_arguments': required + (['sys.argv'] if uses_argv else []), 'cache_areas': areas,
            'usb': usb, 'library': is_library, 'pure_host': pure, 'input_reason': NOT_PURE.get(path.name) or REQUALIFICATION.get(path.name),
            'output_option': option if option in required else None,
            'prose': PROSE_CHECKS.get(path.name), 'serial': path.name in SERIAL_CHECKS}


def generate():
    """Map each check name to its entry; a check that cannot be parsed stops with its name."""
    documents = mentions()
    index = {}
    for path in checks():
        try:
            index[path.name] = entry(path, documents)
            inputs = covered(path)
            index[path.name]['inputs'] = sorted(inputs)
            index[path.name]['input_directories'] = sorted(p for p in inputs if (ROOT / p).is_dir())
        except (SyntaxError, UnicodeDecodeError, ValueError) as error:
            raise SystemExit(f'checks/{path.name}: cannot be classified: {error}')
    return index


class Revision:
    """Read a committed dependency graph without changing the checkout."""
    def __init__(self, revision):
        archive = subprocess.check_output(['git', 'archive', '--format=tar', revision], cwd=ROOT)
        self.files = {}
        self.directories = set()
        with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
            for member in stream:
                path = ROOT / member.name.rstrip('/')
                if member.isdir():
                    self.directories.add(path)
                elif member.isfile():
                    self.files[path] = stream.extractfile(member).read()

    def text(self, path):
        return self.files[path].decode(errors='replace')


@functools.lru_cache(maxsize=None)
def declarations(path, revision=None):
    """Literal supplementary input paths, relative to the repository root."""
    text = revision.text(path) if revision else path.read_text(errors='replace')
    values = ()
    for node in ast.parse(text).body:
        if not isinstance(node, ast.Assign):
            continue
        names = {target.id for target in node.targets if isinstance(target, ast.Name)}
        if 'CHECK_INPUTS' in names:
            values = ast.literal_eval(node.value)
            if not isinstance(values, (list, tuple)) or not all(isinstance(v, str) for v in values):
                raise ValueError('CHECK_INPUTS must be a literal list or tuple of repository paths')
    inputs = set()
    for value in values or ():
        candidate = Path(os.path.normpath(ROOT / value))
        if not candidate.is_relative_to(ROOT):
            raise ValueError('CHECK_INPUTS must stay inside the repository')
        inputs.add(candidate)
    return frozenset(inputs)


@functools.lru_cache(maxsize=None)
def named(path, includes_only, revision=None):
    """Repository files and subdirectories one file names; only #include targets for non-check sources."""
    text = revision.text(path) if revision else path.read_text(errors='replace')
    tokens = set(INCLUDE.findall(text)) if includes_only else set(PATH_TOKEN.findall(text)) | set(INCLUDE.findall(text))
    found = set()
    for token in tokens:
        if '/' not in token and '.' not in token:
            continue
        for base in (ROOT, path.parent, ROOT / 'include'):
            candidate = Path(os.path.normpath(base / token))
            exists = candidate in revision.files or candidate in revision.directories if revision else candidate.exists()
            directory = candidate in revision.directories if revision else candidate.is_dir()
            if candidate.is_relative_to(ROOT) and candidate.parent != ROOT.parent and exists:
                # A top-level directory such as drivers/ or checks/ would select everything under it.
                if not (directory and candidate.parent == ROOT) and candidate != ROOT:
                    found.add(candidate)
                break
    if not includes_only and re.search(r'^import harness$', text, re.M):
        found.add(ROOT / 'checks/harness.py')
    if not includes_only and path.suffix == '.py':
        found.update(declarations(path, revision))
    return frozenset(found)


def covered(path, revision=None):
    """Paths named by a check, followed through the checks/ files it names and their #include closure."""
    seen, stack = set(), [path]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        exists = current in revision.files if revision else current.is_file()
        if exists and current.parent == ROOT / 'checks' and current.suffix == '.py':
            if current.name in COMPLETE_INPUTS:
                seen.update(declarations(current, revision))
                seen.update(ROOT / name for name in COMPLETE_INPUTS[current.name])
                continue
        if exists and current.suffix in ('.py', '.c', '.cc', '.h', '.S', '.inc'):
            stack.extend(named(current, current.parent != ROOT / 'checks', revision))
    return {p.relative_to(ROOT).as_posix() for p in seen}


def select(index, changed):
    changed = [Path(os.path.normpath(ROOT / p)).relative_to(ROOT).as_posix() for p in changed]
    chosen = []
    for name in sorted(index):
        names = set(index[name]['inputs'])
        dirs = set(index[name]['input_directories'])
        if any(c in names or any(c.startswith(d + '/') for d in dirs) or any(n.startswith(c + '/') for n in names)
               for c in changed):
            chosen.append(name)
    return chosen


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--json', action='store_true')
    action.add_argument('--verify', action='store_true')
    action.add_argument('--changed', nargs='+', metavar='PATH')
    parser.add_argument('--run', action='store_true', help='run the selected pure-host checks')
    parser.add_argument('--output', type=Path, help='new directory for --run results')
    parser.add_argument('--timeout', type=int, default=900, help='seconds per check')
    args = parser.parse_args()
    index = generate()
    if args.json:
        print(json.dumps(index, indent=1, sort_keys=True))
        return
    if args.verify:
        problems = [f'{name}: listed in NOT_PURE but absent from checks/' for name in sorted(set(NOT_PURE) - set(index))]
        if subprocess.run(['git', 'ls-files', '--error-unmatch', RETIRED_INDEX], cwd=ROOT,
                          capture_output=True).returncode == 0:
            problems.append(f'{RETIRED_INDEX}: tracked; the index is generated on demand, so remove it with git rm --cached')
        problems += [f'{name}: listed in HOST_ONLY, PROSE_CHECKS or SERIAL_CHECKS but absent from checks/'
                     for name in sorted({*HOST_ONLY, *PROSE_CHECKS, *SERIAL_CHECKS, *REQUALIFICATION} - set(index))]
        problems += [f'{name}: listed in HOST_ONLY but not classified pure-host' for name in sorted(HOST_ONLY)
                     if name in index and not index[name]['pure_host']]
        for problem in problems:
            print(problem)
        if problems:
            raise SystemExit('the check index failed verification')
        print(f'PASS: {len(index)} checks indexed, {sum(e["pure_host"] for e in index.values())} pure-host')
        return
    chosen = select(index, args.changed)
    runnable = [name for name in chosen if index[name]['pure_host']]
    for name in chosen:
        e = index[name]
        needs = 'pure-host' if e['pure_host'] else 'needs ' + ', '.join(
            ['library'] * e['library'] + ['USB'] * e['usb'] + e['required_arguments'] +
            ['cache/' + a for a in e['cache_areas'] if a not in NEUTRAL_AREAS] +
            ([e['input_reason']] if e['input_reason'] else [])) or 'needs inputs'
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
        if index[name]['output_option']:
            command += [index[name]['output_option'], str(out / Path(name).stem)]
        try:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, env=run_environment(), timeout=args.timeout)
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
