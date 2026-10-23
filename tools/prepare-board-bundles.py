#!/usr/bin/env python3
"""Prepare frozen board-test bundles and queue submissions for the memory, GPU and NPU paths.

The tool never contacts the board, never submits and never claims. It holds a
fixed table of rows (docs/designs/board-bundles.md). For each row the caller
supplies the keyword arguments of the row's preparation module in one JSON
file, keyed by row name; the tool adds `output`. The outer process creates
~/.cache/sunstoneos/evidence/board-bundles-<date>-<n>/ exclusively and runs the
preparation step once inside a bubblewrap sandbox that has no USB device, no
/sys, no network and a read-only view of every other file. Inputs must lie
outside /tmp, /run and /sys, which the sandbox replaces with empty
directories.

For each row with inputs, the step refuses a Terminal row whose runner does
not declare COMPLETION_COUNTS. It calls the module's prepare(), which ends
with the module's own validate_bundle(), and refuses the row unless the
admitted value carries the row's admission fields. It then scans a Monitor
row's image.elf with nm, removes write permission from every bundle entry,
hashes each bundle file once into artifacts.json, derives a Terminal row's
queue timeout from the runner's own bounds and the bundle's file sizes,
snapshots the row's sources with tools/evidence-snapshot.py, and writes
submission.json after checking it with submission() from
tools/board-test-queue.py. run.json records every row's outcome. The exit
status is 0 when no row was refused.

  prepare-board-bundles.py --inputs inputs.json
"""
import argparse
import ast
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BWRAP = '/usr/bin/bwrap'
NM = '/usr/lib/llvm-21/bin/llvm-nm'
EVIDENCE = Path.home() / '.cache/sunstoneos/evidence'
BLOCK = 1 << 20
# The queue admits a peripheral test only with one of two recovery
# prerequisites. No record yet qualifies for this one, so the Monitor rows
# wait for it. Running them earlier needs a queue-defined monitor-timer
# prerequisite and the user's written acceptance of an uncovered bus stall.
MONITOR_PREREQUISITE = 'independent-watchdog-rom-return'
# One Monitor attempt: 615 s for the churn guard to admit two re-enumerations,
# 435 s of transaction and return bounds, and 150 s of margin.
MONITOR_TIMEOUT = 1200
TERMINAL_MARGIN = 60
QUEUE_TIMEOUT_LIMIT = 3600
# The churn guard admits one re-enumeration within its 600 s window plus 15 s
# spacing (usb_guard.reenumeration in run_session).
GUARD_WAIT = 615
# The supervised runners move every byte through this module's FEL requests.
SUPERVISED_TRANSPORT = 'tools/check-a733-smp.py'
# Per-job fields a Terminal runner must declare in COMPLETION_COUNTS and report
# in its result record before the tool prepares its row.
COUNT_FIELDS = ('interrupt_completions', 'fallback_completions', 'client_wakeups')
# USB stacks, the shared USB0 loop and secondary-core power-up.
FORBIDDEN_PREFIXES = ('dcd_', 'hcd_', 'tud_', 'tuh_', 'usb0_', 'a733_cpu_start_', 'a733_cpu_secondary_')
ENVIRONMENT = ('PATH', 'HOME', 'LANG', 'PYTHONDONTWRITEBYTECODE')

TERMINAL_PROCEDURE = [
    'Confirm that ranks 1 to 5 and rank 9 of docs/audit/board-test-plan.md passed earlier in this power cycle.',
    'Claim this test, then run {runner} on the frozen bundle {bundle} with this evidence directory.',
    'Save the per-job interrupt_completions, fallback_completions and client_wakeups from the runner\'s '
    'result record with the evidence; they are measurements, not pass conditions.',
    'Keep the port lock and confirm whole-board power removal before any other test; this test ends the '
    'board session.']
TERMINAL_STOP = ('Stop after the first failure. The runner keeps the A733 port lock after every outcome until '
                 'whole-board power removal is confirmed, so no other test runs in this board session.')
TERMINAL_COUNTS = (' The record also reports, per job, interrupt_completions, fallback_completions and '
                   'client_wakeups. These counts are measurements, not pass conditions; zero fallback '
                   'completions over the session is the evidence that removes the tick wait.')
TRANSPORT_SOURCES = ['tools/check-a733-smp-supervised.py', SUPERVISED_TRANSPORT]
OS_SOURCES = ['apps/os/prepare.py', 'tools/check-a733-os-supervised.py', 'apps/mimus-board/prepare.py',
              'apps/thermal/prepare.py', 'apps/board/prepare.py', 'tools/prepare-board-bundles.py',
              *TRANSPORT_SOURCES]

ROWS = [
    # `admission` maps fields of the value prepare() returns to the values that
    # identify this row's build. None refuses the row: no owning package has
    # named those values yet.
    {'name': 'memory-qualification', 'class': 'monitor', 'renewal_cap': True, 'admission': None,
     'component': 'DDR memory qualification, one run of the three-step map chain',
     'module': 'apps/memtest/check.py', 'runner': 'tools/a733-returning-host.py',
     'sources': ['apps/memtest/check.py', 'tools/a733-returning-host.py', 'tools/prepare-board-bundles.py'],
     'expected_result': 'An image-initiated exit with every record field present, an alias-probe pass, an '
                        'address-line pass, a page-signature match, no external abort and no expired DMA0 '
                        'deadline, over the tested range the contract names.',
     'procedure': [
         'Confirm that ranks 1 to 7 of docs/audit/board-test-plan.md passed earlier in this power cycle, with '
         'rank 3 reporting dram_mib 12288 and rank 6 reporting the boot clock.',
         'Claim this test, then run the memory qualification profile through {runner} on the frozen bundle '
         '{bundle}.',
         'Save the run record in this evidence directory; the next run is prepared from it on the host.'],
     'stop_condition': 'Stop at any record that does not chain: anything other than an image-initiated exit with '
                       'every field present, alias-probe, address-line and page-signature passes, no external '
                       'abort and no expired DMA0 deadline. Such a record retains the board: no further upload of '
                       'any kind, including rank 8 and the Terminal rows, until the user reviews the record, and '
                       'after a DMA0 expiry not until a FEL read also shows DMA0 quiet.'},
    {'name': 'npu-service-os-image', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'NPU service in the integrated OS image',
     'module': 'apps/os/prepare.py', 'runner': 'tools/check-a733-os-supervised.py',
     'sources': OS_SOURCES + ['drivers/npu/npu_service.c', 'apps/os/services.c'],
     'expected_result': 'One admitted model completes and its output matches its host reference.' + TERMINAL_COUNTS},
    {'name': 'gpu-service-os-image', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'GPU service in the integrated OS image',
     'module': 'apps/os/prepare.py', 'runner': 'tools/check-a733-os-supervised.py',
     'sources': OS_SOURCES + ['drivers/gpu/pvr_gpu_service.c', 'apps/os/services.c'],
     'expected_result': 'Every submitted GPU job completes and its output matches its host reference.'
                        + TERMINAL_COUNTS},
    {'name': 'accelerator-arbiter', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'GPU and NPU jobs under the memory and power arbiter',
     'module': 'apps/accelerators/prepare.py', 'runner': 'tools/check-a733-accelerators-supervised.py',
     'sources': ['apps/accelerators/prepare.py', 'tools/check-a733-accelerators-supervised.py',
                 'apps/npu-convolution/prepare-supervised.py', 'drivers/npu/npu_service.c',
                 'drivers/gpu/pvr_gpu_service.c', 'tools/prepare-board-bundles.py', *TRANSPORT_SOURCES],
     'expected_result': 'The GPU and NPU jobs complete under the arbiter and each output matches its host '
                        'reference.' + TERMINAL_COUNTS},
    {'name': 'recognition-dispatcher', 'class': 'terminal', 'admission': None,
     'transport': SUPERVISED_TRANSPORT,
     'component': 'Recognition dispatcher placing mimus linear layers on the CPU or GPU in the OS image',
     'module': 'apps/os/prepare.py', 'runner': 'tools/check-a733-os-supervised.py',
     'sources': OS_SOURCES + ['apps/os/dispatch.c', 'apps/os/placement.py', 'drivers/gpu/pvr_gpu_service.c'],
     'expected_result': 'Recognition of the fixed inputs matches the host transcript, with each linear layer '
                        'on the processor the placement table names.' + TERMINAL_COUNTS},
    {'name': 'gpu-npu-hang-reset', 'class': 'excluded',
     'reason': 'An intentional GPU or NPU hang needs physical recovery.'},
    {'name': 'voltage-clock-changes', 'class': 'excluded',
     'reason': 'A voltage or clock change needs physical recovery and the user\'s approval.'},
    {'name': 'display-output', 'class': 'excluded',
     'reason': 'Display output needs a connected monitor or panel.'},
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def save(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def sandbox(run_dir):
    """The bubblewrap command prefix for one preparation step."""
    run_dir = str(run_dir)
    return [BWRAP, '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc', '--tmpfs', '/sys',
            '--tmpfs', '/tmp', '--tmpfs', '/run', '--bind', run_dir, run_dir, '--unshare-all',
            '--die-with-parent', '--clearenv', '--setenv', 'PATH', '/usr/local/bin:/usr/bin:/bin',
            '--setenv', 'HOME', str(Path.home()), '--setenv', 'LANG', 'C.UTF-8',
            '--setenv', 'PYTHONDONTWRITEBYTECODE', '1', '--chdir', run_dir]


def literals(path):
    """Module-level NAME = literal assignments of a Python source, read without executing it."""
    found = {}
    for node in ast.parse(Path(path).read_text()).body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    try:
                        found.setdefault(target.id, []).append(ast.literal_eval(node.value))
                    except (ValueError, TypeError):
                        pass
    return {name: values[0] for name, values in found.items() if len(values) == 1}


def runner_terms(path):
    """The runner's PREREQUISITES, its COMPLETION_COUNTS (None when absent) and its largest capture window."""
    values = literals(path)
    prerequisites = values.get('PREREQUISITES')
    seconds = [int(x) for x in re.findall(r'capture_seconds\s*=\s*(\d+)', Path(path).read_text())]
    require(isinstance(prerequisites, set) and prerequisites and seconds,
            f'Runner {path} has no PREREQUISITES literal or capture window.')
    counts = values.get('COMPLETION_COUNTS')
    return sorted(prerequisites), (sorted(counts) if isinstance(counts, (set, tuple, list)) else None), max(seconds)


def transport_bounds(path):
    """The FEL request bound, transfer rate and SRAM backup regions of the supervised transport."""
    values = literals(path)
    request, rate, regions = (values.get(k) for k in ('FEL_REQUEST_SECONDS', 'FEL_BYTES_PER_SECOND', 'REGIONS'))
    require(type(request) is int and request > 0 and type(rate) is int and rate > 0 and isinstance(regions, list)
            and regions and all(len(r) == 3 and type(r[2]) is int and r[2] > 0 for r in regions),
            f'Transport {path} has no FEL bounds or SRAM regions.')
    return {'request': request, 'rate': rate, 'regions': [r[2] for r in regions]}


def terms(row, root):
    """Prerequisites, capture window and transport bounds of one row; refuses a Terminal row without counts."""
    if row['class'] == 'monitor':
        return [MONITOR_PREREQUISITE], None, None
    prerequisites, counts, seconds = runner_terms(root / row['runner'])
    require(counts == sorted(COUNT_FIELDS),
            f'Runner {row["runner"]} does not declare COMPLETION_COUNTS {", ".join(COUNT_FIELDS)}; its record '
            'cannot carry the per-job completion and wake-up counts.')
    return prerequisites, seconds, transport_bounds(root / row['transport'])


def terminal_timeout(seconds, sizes, bounds):
    """One supervised attempt's worst case, in run_session order, plus margin.

    Every upload is the bytes of one bundle file, so each bundle file is
    counted as an upload: a watchdog readl, then one write and readback.
    """
    request, rate, regions = bounds['request'], bounds['rate'], bounds['regions']

    def transfer(moved):
        return request + -(-moved // rate)

    parts = {'guard_wait': GUARD_WAIT,
             'version_readl_exe': 3 * request,
             'sram_backup': sum(transfer(n) for n in regions),
             'uploads': sum(request + transfer(2 * n) for n in sizes),
             'capture': seconds,
             'refusal_probe': transfer(128) + request + sum(transfer(2 * n) for n in regions),
             'margin': TERMINAL_MARGIN}
    total = sum(parts.values())
    require(total <= QUEUE_TIMEOUT_LIMIT,
            f'One attempt can take {total} s, more than the queue\'s {QUEUE_TIMEOUT_LIMIT} s limit.')
    return total, parts


def admit(row, admitted):
    """The admitted value names this row's build."""
    expected = row.get('admission')
    require(expected is not None, f'No owning package has named the admission values of {row["name"]}; '
                                  'the row is refused until it does.')
    require(isinstance(admitted, dict), 'prepare() returned no admission record.')
    wrong = sorted(k for k, v in expected.items() if admitted.get(k) != v)
    require(not wrong, 'The admitted bundle is not this row\'s build: ' +
            ', '.join(f'{k} is {admitted.get(k)!r}, not {expected[k]!r}' for k in wrong))


def monitor_scan(elf, nm, renewal_cap):
    """The defined symbols of a Monitor image allow the forced return to the ROM."""
    result = subprocess.run([nm, '--defined-only', '-P', str(elf)], capture_output=True, text=True)
    require(result.returncode == 0, f'nm failed on {elf}: {result.stderr.strip()}')
    names = {line.split()[0] for line in result.stdout.splitlines() if line.strip()}
    require(names, 'Monitor image has no symbol table.')
    require('monitor_entry' in names, 'Monitor image does not define monitor_entry.')
    forbidden = sorted(n for n in names if n.startswith(FORBIDDEN_PREFIXES))
    require(not forbidden, 'Monitor image defines USB or secondary-core startup symbols: ' + ', '.join(forbidden[:8]))
    require(not renewal_cap or 'monitor_renewal_cap' in names,
            'Memory image does not define monitor_renewal_cap, the capped deadline renewal.')


def entries(bundle):
    """Every entry below bundle, parents first; only regular files and directories."""
    require(stat.S_ISDIR(os.lstat(bundle).st_mode), f'prepare() left no bundle directory at {bundle}.')
    found = [bundle]
    for directory, dirs, files in os.walk(bundle):
        dirs.sort()
        for name in sorted(dirs) + sorted(files):
            path = Path(directory) / name
            mode = os.lstat(path).st_mode
            require(stat.S_ISREG(mode) or stat.S_ISDIR(mode), f'Bundle entry is not a file or directory: {path}')
            found.append(path)
    return found


def freeze(bundle):
    for path in reversed(entries(bundle)):
        os.chmod(path, stat.S_IMODE(os.lstat(path).st_mode) & ~0o222)


def rchar():
    with open('/proc/self/io', 'rb', buffering=0) as stream:
        text = stream.read(4096)
    return int(re.search(rb'^rchar: (\d+)$', text, re.M).group(1)), len(text)


def artifact_pass(bundle):
    """Hash each regular file once; return the artifacts, each file's size, bytes read and writable entries."""
    found = entries(bundle)
    writable = sum(bool(os.lstat(p).st_mode & 0o222) for p in found)
    files = sorted(p for p in found if not p.is_dir())
    buffer = bytearray(BLOCK)
    view = memoryview(buffer)
    artifacts = []
    sizes = [os.lstat(p).st_size for p in files]
    size = sum(sizes)
    before, sample = rchar()
    for path in files:
        digest = hashlib.sha256()
        with open(path, 'rb', buffering=0) as stream:
            while count := stream.readinto(buffer):
                digest.update(view[:count])
        artifacts.append({'path': str(path), 'sha256': digest.hexdigest()})
    after, _ = rchar()
    return artifacts, sizes, {'bundle_bytes': size, 'read_bytes': after - before - sample,
                              'writable_entries': writable}


def changed_paths(status):
    """Paths named by `git status --porcelain=v1` output."""
    paths = set()
    for line in status.splitlines():
        for part in line[3:].split(' -> '):
            paths.add(part.strip('"'))
    return paths


def snapshot(row, row_dir, artifacts_file, repository):
    command = [sys.executable, str(ROOT / 'tools/evidence-snapshot.py'), '--repository', str(repository),
               '--output', str(row_dir / 'snapshot'), '--artifact', str(artifacts_file),
               '--note', f'Sources of the {row["name"]} board-test bundle; the artifact lists the frozen bundle files.']
    for source in row['sources']:
        command += ['--source', source]
    result = subprocess.run(command, capture_output=True, text=True)
    require(result.returncode == 0, 'Snapshot failed: ' + (result.stderr.strip() or result.stdout.strip()))
    manifest = json.loads((row_dir / 'snapshot/manifest.json').read_text())
    dirty = sorted(changed_paths(manifest['worktree_status_before_copy']) & set(row['sources']))
    require(not dirty, 'Uncommitted row sources: ' + ', '.join(dirty))
    return manifest


def paths(value):
    """Every string argument of the existing prepare() functions is a file path."""
    if isinstance(value, str):
        return Path(value)
    return [paths(v) for v in value] if isinstance(value, list) else value


def prepare_row(row, arguments, row_dir, request, queue):
    """Prepare, admit, scan, freeze, hash, snapshot and submit one row."""
    record = {'class': row['class']}
    stage = 'inputs'
    try:
        require(isinstance(arguments, dict) and 'output' not in arguments,
                'Row inputs must be an object of prepare() keyword arguments without output.')
        root = Path(request['repository'])
        module_path = root / row['module']
        require(module_path.is_file(), f'Preparation module {row["module"]} does not exist.')
        prerequisites, seconds, bounds = terms(row, root)
        row_dir.mkdir()
        bundle = row_dir / 'bundle'
        stage = 'prepare'
        module = load('board_bundle_' + row['name'].replace('-', '_'), module_path)
        kwargs = {k: paths(v) for k, v in arguments.items()}
        admitted = module.prepare(**kwargs, output=bundle)
        record['admission_schema'] = admitted.get('schema') if isinstance(admitted, dict) else None
        stage = 'admission'
        admit(row, admitted)
        if row['class'] == 'monitor':
            stage = 'monitor-scan'
            monitor_scan(bundle / 'image.elf', request['nm'], row.get('renewal_cap', False))
        stage = 'freeze'
        freeze(bundle)
        stage = 'artifacts'
        artifacts, sizes, metrics = artifact_pass(bundle)
        record['metrics'] = metrics
        stage = 'timeout'
        if row['class'] == 'monitor':
            timeout = MONITOR_TIMEOUT
        else:
            timeout, record['timeout_terms'] = terminal_timeout(seconds, sizes, bounds)
        artifacts_file = row_dir / 'artifacts.json'
        save(artifacts_file, artifacts)
        stage = 'snapshot'
        manifest = snapshot(row, row_dir, artifacts_file, root)
        stage = 'submission'
        names = {'runner': row['runner'], 'bundle': bundle}
        value = {'component': row['component'], 'kind': 'peripheral',
                 'sources': [{'repository': manifest['repository'], 'revision': manifest['revision']}],
                 'artifacts': artifacts, 'prerequisites': prerequisites,
                 'expected_result': row['expected_result'],
                 'procedure': [step.format(**names) for step in row.get('procedure', TERMINAL_PROCEDURE)],
                 'timeout_seconds': timeout, 'stop_condition': row.get('stop_condition', TERMINAL_STOP),
                 'evidence_directory': str(row_dir / 'evidence')}
        queue.submission(value)
        save(row_dir / 'submission.json', value)
        record.update(status='prepared', submission=str(row_dir / 'submission.json'), revision=manifest['revision'],
                      timeout_seconds=timeout, prerequisites=prerequisites)
    except Exception as error:
        record.update(status='refused', stage=stage, message=f'{type(error).__name__}: {error}')
    return record


def step(run_dir):
    """The sandboxed preparation step: every row of request.json, in table order."""
    # bwrap always sets PWD after it changes directory; preparation and its
    # children see only the four variables the sandbox sets.
    os.environ.pop('PWD', None)
    request = json.loads((run_dir / 'request.json').read_text())
    queue = load('board_bundle_queue', ROOT / 'tools/board-test-queue.py')
    rows = {}
    for row in request['rows']:
        if row['class'] == 'excluded':
            rows[row['name']] = {'class': 'excluded', 'status': 'excluded', 'message': row['reason']}
        elif row['name'] not in request['inputs']:
            rows[row['name']] = {'class': row['class'], 'status': 'no-inputs'}
        else:
            rows[row['name']] = prepare_row(row, request['inputs'][row['name']], run_dir / row['name'], request,
                                            queue)
    save(run_dir / 'run.json', {'schema': 'board-bundles-run-v1', 'physical_access': False, 'rows': rows})
    for name, record in rows.items():
        print(f'{name}: {record["status"]}' + (f' ({record["message"]})' if 'message' in record else ''))
    return 1 if any(r['status'] == 'refused' for r in rows.values()) else 0


def run(inputs, rows=ROWS, evidence=EVIDENCE, repository=ROOT, nm=NM):
    """Create the run directory and wait once on the sandboxed step; return (exit status, run directory)."""
    names = {row['name']: row for row in rows}
    unknown = sorted(set(inputs) - set(names))
    require(not unknown, 'Inputs name unknown rows: ' + ', '.join(unknown))
    excluded = sorted(n for n in inputs if names[n]['class'] == 'excluded')
    require(not excluded, 'Inputs name excluded rows: ' + ', '.join(excluded))
    require(Path(BWRAP).is_file(), f'{BWRAP} is missing; preparation needs the sandbox.')
    evidence = Path(evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    date = datetime.now(timezone.utc).strftime('%Y%m%d')
    n = 1
    while True:
        run_dir = evidence / f'board-bundles-{date}-{n}'
        try:
            run_dir.mkdir()
            break
        except FileExistsError:
            n += 1
    save(run_dir / 'request.json', {'rows': rows, 'inputs': inputs, 'repository': str(repository), 'nm': str(nm)})
    command = sandbox(run_dir) + [sys.executable, str(Path(__file__).resolve()), '--step', str(run_dir)]
    status = subprocess.run(command).returncode
    require((run_dir / 'run.json').is_file(), f'The sandboxed step stopped before recording a result ({status}).')
    return status, run_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--inputs', type=Path, help='JSON object: row name to prepare() keyword arguments')
    parser.add_argument('--evidence', type=Path, default=EVIDENCE, help='Directory that receives the run directory')
    parser.add_argument('--nm', default=NM)
    parser.add_argument('--step', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.step:
        return step(args.step)
    if args.inputs is None:
        parser.error('--inputs is required')
    status, run_dir = run(json.loads(args.inputs.read_text()), evidence=args.evidence, nm=args.nm)
    print(run_dir)
    return status


if __name__ == '__main__':
    sys.exit(main())
