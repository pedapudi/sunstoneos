#!/usr/bin/env python3
"""Check return registrations against session grants and execute the production registry.

Owner delays model publication and scheduler interleaving, not device hardware.
Synthetic grant cases cover services whose physical session facts are unavailable.
The source check verifies registration and direct registered callback bodies; it
is not a C call-graph proof. Device checks cover their owner tasks and adapters.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('apps/os', 'apps/board/main.c', 'tools/session.py', 'tools/sunstone-manifest.py',
                'include/sunstone_app.h', 'include/sunstone_session.h')
FORBIDDEN = r'\b(?:platform_exit|board_fail(?:_at)?|fail_close|a733_smp_exit_privileged)\s*\('


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def uncomment(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)


def function_span(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', text)
    if not match:
        raise AssertionError('missing function ' + name)
    start = text.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return start, end


def function(text, name):
    a, b = function_span(uncomment(text), name)
    return uncomment(text)[a:b]


def expected_order(owner):
    if owner in ('NPU', 'GPU'):
        return 'QUEUES'
    return {'CPU_CLOCK': 'CLOCK', 'THERMAL_OWNER': 'THERMAL',
            'PMIC_OWNER': 'PMIC', 'TERMINAL_OWNER': 'TERMINALS'}.get(owner, 'DEVICES')


def inventory(sources):
    found = {}
    for path, raw in sources.items():
        text = uncomment(raw)
        for match in re.finditer(r'\bstruct\s+os_quiesce\s+(\w+)\s*=\s*\{([^;]*?)\};', text, re.S):
            fields = dict(re.findall(r'\.(owner|order|step)\s*=\s*(\w+)', match.group(2)))
            if len(fields) != 3:
                continue
            owner = fields['owner'].removeprefix('OS_QUIESCE_')
            found.setdefault(owner, []).append({'path': path, 'symbol': match.group(1),
                                                'callback': fields['step'],
                                                'order': fields['order'].removeprefix('OS_QUIESCE_')})
    return found


def source_problems(sources, required):
    errors, descriptors = [], inventory(sources)
    for owner in required:
        if owner not in descriptors:
            errors.append(owner + ': no quiesce descriptor')
            continue
        for d in descriptors[owner]:
            text = uncomment(sources[d['path']])
            if not re.search(r'\bos_quiesce_register\s*\(\s*&\s*' + re.escape(d['symbol']) + r'\s*\)', text):
                errors.append(owner + ': registration missing in ' + d['path'])
            if d['order'] != expected_order(owner):
                errors.append(owner + ': wrong order class ' + d['order'])
            body = function(text, d['callback'])
            if re.search(FORBIDDEN, body):
                errors.append(owner + ': stop in quiesce callback')
            if owner == 'NPU' and re.search(r'\bos_npu_(?:thermal|close|queue_sdk_fail)\s*\(', body):
                errors.append('NPU: supervisor callback closes the application queue')
    cpu = sources['apps/os/cpu-return.c']
    claim = function(cpu, 'failure_claim')
    if re.search(r'\b(?:xTaskNotify\w*|vTaskNotify\w*|ulTaskNotify\w*|os_npu_thermal|xTaskResume\w*|vTaskResume\w*)\s*\(', claim):
        errors.append('failure_claim calls a task-level API')
    supervisor = function(cpu, 'os_cpu_return_supervise')
    if re.search(r'\bos_npu_(?:thermal|close)\s*\(', supervisor):
        errors.append('supervisor closes the application NPU queue')
    timeout = re.search(r'os_cpu_return_expired\([^;]*OS_FAILURE_RETURN_US\)', supervisor)
    step = re.search(r'if\s*\(\s*os_quiesce_step\(board_fail_note\)\s*!=\s*OS_QUIESCE_DONE\s*\)\s*return\s*;', supervisor)
    if not timeout or not step or timeout.start() > step.start():
        errors.append('supervisor must enforce the shared bound before the registry completion gate')
    if not re.search(r'os_cpu_return_expired\([^;]*OS_FAILURE_RETURN_US\)\)\s*\{\s*'
                     r'os_quiesce_note_faults\(board_fail_note\);\s*fail\(0x742\);', supervisor):
        errors.append('supervisor must report pending fault records before timeout')
    clock = function(sources['apps/os/cpu.c'], 'os_cpu_clock_step')
    done = re.search(r'if\s*\(atomic_load_explicit\(&return_done,[^)]*\)\)\s*return\s+1\s*;', clock)
    if not done or done.start() > clock.index('a733_cpu_clock_burst'):
        errors.append('clock owner runs after lowering completed')
    return errors


def source_mutants(sources, required):
    count = 0
    for owner, items in inventory(sources).items():
        if owner not in required:
            continue
        for d in items:
            path, text = d['path'], sources[d['path']]
            pattern = r'\bos_quiesce_register\s*\(\s*&\s*' + re.escape(d['symbol']) + r'\s*\)'
            removed, n = re.subn(pattern, '1', text)
            assert n, owner
            errors = source_problems({**sources, path: removed}, required)
            assert any(e.startswith(owner + ': registration missing') for e in errors), (owner, errors)
            a, _ = function_span(text, d['callback'])
            stopped = text[:a+1] + '\n platform_exit(6);\n' + text[a+1:]
            errors = source_problems({**sources, path: stopped}, required)
            assert owner + ': stop in quiesce callback' in errors, (owner, errors)
            count += 2
    cpu = sources['apps/os/cpu-return.c']
    for name in ('failure_claim', 'os_cpu_return_supervise'):
        a, _ = function_span(cpu, name)
        mutated = cpu[:a+1] + '\n os_npu_thermal(0,1,0,0);\n' + cpu[a+1:]
        assert source_problems({**sources, 'apps/os/cpu-return.c': mutated}, required)
        count += 1
    clock = sources['apps/os/cpu.c']
    mutated, n = re.subn(r'if\s*\(atomic_load_explicit\(&return_done,[^)]*\)\)\s*return\s+1\s*;', '', clock)
    assert n == 1
    assert 'clock owner runs after lowering completed' in source_problems({**sources, 'apps/os/cpu.c': mutated}, required)
    return count + 1


def build(root, out, actors, name, registry=None, step=None, publication_probe=False):
    directory = out / name
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'actors.inc').write_text(''.join('ACTOR(%s, %s)\n' % (owner, expected_order(owner)) for owner in actors))
    (directory / 'quiesce.h').write_text((root / 'apps/os/quiesce.h').read_text())
    registry = registry if registry is not None else (root / 'apps/os/quiesce.c').read_text()
    if publication_probe:
        store = 'atomic_store_explicit(&p->requested,1,memory_order_release);'
        assert registry.count(store) == 2, 'review request-publication interleaving points'
        registry = registry.replace('#include "quiesce.h"', '#include "quiesce.h"\nextern void os_quiesce_publication_probe(struct os_quiesce *);')
        registry = registry.replace(store, '{' + store + 'os_quiesce_publication_probe(p);}')
    (directory / 'quiesce.c').write_text(registry)
    (directory / 'cpu-return-step.h').write_text(step if step is not None else (root / 'apps/os/cpu-return-step.h').read_text())
    limit = re.search(r'#define OS_FAILURE_RETURN_US UINT64_C\((\d+)\)', (root / 'apps/os/cpu-return.h').read_text())
    assert limit and int(limit.group(1)) == 3000000, 'return bound changed: review the failure protocol budget'
    command = ['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-DRETURN_LIMIT_US=' + limit.group(1), '-I' + str(root / 'include'), '-I' + str(directory), str(ROOT / 'checks/os-failure-step.c'),
               str(directory / 'quiesce.c'), '-o', str(directory / 'model')]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (directory / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    assert compiled.returncode == 0, compiled.stderr
    return directory / 'model'


def run(binary, mode, target=0, delay=0, success=True):
    result = subprocess.run([str(binary), str(mode), str(target), str(delay)], capture_output=True, text=True)
    if success:
        assert result.returncode == 0, '%s mode %d target %d: %s' % (binary, mode, target, result.stderr)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, default=ROOT, help='source under review; default this checkout')
    parser.add_argument('--session', type=Path, help='check this session.json inventory instead of all synthetic grant cases')
    parser.add_argument('--manifest', type=Path, help='manifest JSON supplying card-backed large regions')
    parser.add_argument('--service', action='append', help='limit a staged conversion to these session service records (repeat)')
    args = parser.parse_args()
    root = args.source_root.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    session = load('failure_step_session', ROOT / 'tools/session.py')
    session.ROOT = root
    manifest_tool = load('failure_step_manifest', root / 'tools/sunstone-manifest.py')
    manifest = manifest_tool.load(args.manifest) if args.manifest else None
    if args.session:
        plans = [json.loads(args.session.read_text())]
    elif args.service:
        plans = [{'services': args.service}]
    else:
        plans = [{'services': ['thermal']}]
        for grant, *_ in manifest_tool.os_service_rows('OS_GRANT_ROWS'):
            plans.append({'services': ['thermal', grant.lower()]})
        for name, bit in manifest_tool.DEVICES.items():
            plans.append({'services': ['thermal'], 'devices': bit})
        plans.append({'services': ['thermal'], 'storage': True})
    # Optional but ungranted device declarations create no owner, and a
    # future unknown service or device must not silently disappear.
    empty = session.quiesce_actors({'services': ['thermal']})
    assert session.quiesce_actors({'services': ['thermal'], 'manifest': {
        'devices_optional': sum(manifest_tool.DEVICES.values())}}) == empty
    for bad in ({'services': ['unknown_service']}, {'devices': 1 << 63}):
        try:
            session.quiesce_actors(bad)
        except session.SessionError:
            pass
        else:
            raise AssertionError('unknown session grant was ignored')
    assert 'STORAGE' in session.quiesce_actors({'symbols': {'os_sd_quiesce_register': 1}})
    assert 'STORAGE' in session.quiesce_actors({}, {'large_regions': [{'source_bytes': 512}]})
    assert 'STORAGE' not in session.quiesce_actors({}, {'large_regions': [{'source_bytes': 0}]})
    scenarios = sorted({tuple(session.quiesce_actors(plan, manifest)) for plan in plans})
    required = sorted({actor for actors in scenarios for actor in actors})
    sources = {str(p.relative_to(root)): p.read_text() for p in (root / 'apps/os').glob('*.c')}
    errors = source_problems(sources, required)
    assert not errors, '\n'.join(errors)
    mutants = source_mutants(sources, required)
    cases = 0
    for i, actors in enumerate(scenarios):
        binary = build(root, args.output, actors, 'scenario-%d' % i)
        for mode in (0, 1, 5):
            run(binary, mode);cases += 1
    # Stress all declared owners in one registry. This is not an admitted
    # hardware session: some grants share an exclusive controller.
    binary = build(root, args.output, required, 'all-owner-registry')
    for i, owner in enumerate(required):
        run(binary, 2, i);cases += 1
        if owner != 'TERMINAL_OWNER':
            run(binary, 3, i);cases += 1
    run(binary, 4);cases += 1
    orders = ('QUEUES', 'DEVICES', 'CLOCK', 'THERMAL', 'PMIC', 'TERMINALS')
    earliest = min(range(len(required)), key=lambda i: orders.index(expected_order(required[i])))
    for mode in (6, 8):
        run(binary, mode, earliest);cases += 1
    observed = build(root, args.output, required, 'class-publication', publication_probe=True)
    run(observed, 7);cases += 1
    for ms in range(40):
        run(binary, 1, delay=100000 + ms * 1000);cases += 1
    registry = (root / 'apps/os/quiesce.c').read_text()
    step = (root / 'apps/os/cpu-return-step.h').read_text()
    changes = [
        ('does-not-wait', 'if(pending)return OS_QUIESCE_PENDING;', 'if(pending&&0)return OS_QUIESCE_PENDING;'),
        ('repeats-completed-owner', 'if(r!=OS_QUIESCE_DONE){', 'if(1){'),
        ('fault-hides-later-owner', 'faults|=r==OS_QUIESCE_FAULT;', 'faults|=r==OS_QUIESCE_FAULT;if(faults)return OS_QUIESCE_FAULT;'),
        ('fault-replaces-first-code',
         'if(atomic_compare_exchange_strong_explicit(&q->fault_state,&none,1,memory_order_acq_rel,memory_order_acquire)){',
         'if(1){(void)none;'),
    ]
    if len({expected_order(owner) for owner in required}) < len(required):
        changes.append(('peer-not-requested',
                        'if(p->order==order)atomic_store_explicit(&p->requested,1,memory_order_release);',
                        '(void)p;'))
    for name, old, new in changes:
        assert registry.count(old) == 1, name
        bad = build(root, args.output, required, 'mutant-' + name, registry=registry.replace(old, new))
        mode = 3 if name.startswith('fault-') else 1
        target = next(i for i, owner in enumerate(required) if owner != 'TERMINAL_OWNER')
        assert not run(bad, mode, target, success=False), 'undetected registry mutant ' + name
        mutants += 1
    # A raw request must remain invisible until every peer request has been
    # stored. The probe schedules the observer at each publication boundary.
    armed = '(atomic_load_explicit(&armed_classes,memory_order_acquire)&(1u<<q->order))&&'
    assert registry.count(armed) == 1
    bad = build(root, args.output, required, 'mutant-class-visible-before-release',
                registry=registry.replace(armed, ''), publication_probe=True)
    assert not run(bad, 7, success=False), 'partial class publication was observable'
    mutants += 1
    # Fault reporting is independent of class advancement. An unrequested
    # later owner can fail while the first class is still waiting.
    a, b = function_span(registry, 'os_quiesce_step')
    driven = registry[a:b]
    assert driven.count('os_quiesce_note_faults(note);') == 2
    bad = build(root, args.output, required, 'mutant-pending-hides-later-fault',
                registry=registry[:a] + driven.replace('os_quiesce_note_faults(note);', '(void)note;') + registry[b:])
    assert not run(bad, 6, earliest, success=False), 'a pending class hid a later fault'
    mutants += 1
    cpu = sources['apps/os/cpu-return.c']
    flush = 'os_quiesce_note_faults(board_fail_note);'
    assert cpu.count(flush) == 1
    assert 'supervisor must report pending fault records before timeout' in source_problems(
        {**sources, 'apps/os/cpu-return.c': cpu.replace(flush, '')}, required)
    mutants += 1
    unsafe_complete = '{atomic_store_explicit(&q->result,OS_QUIESCE_DONE,memory_order_release);}'
    a, b = function_span(registry, 'os_quiesce_complete')
    bad = build(root, args.output, required, 'mutant-complete-erases-fault',
                registry=registry[:a] + unsafe_complete + registry[b:])
    target = next(i for i, owner in enumerate(required) if owner != 'TERMINAL_OWNER')
    assert not run(bad, 3, target, success=False), 'completion erased an asynchronous fault'
    mutants += 1
    timeout = 'return now_us-since_us>=limit_us;'
    assert timeout in step
    bad = build(root, args.output, required, 'mutant-timeout', step=step.replace(timeout, '(void)limit_us;return now_us>since_us;'))
    assert not run(bad, 2, success=False), 'undetected timeout mutant'
    mutants += 1
    fatal = 'if(fatal)return npu_line?OS_BOUNDED_FATAL_NPU|(npu_line&0x3ffu):OS_BOUNDED_FATAL;'
    assert fatal in step
    bad = build(root, args.output, required, 'mutant-ignores-fatal', step=step.replace(fatal, '(void)fatal;(void)npu_line;'))
    assert not run(bad, 1, success=False), 'undetected fatal-admission mutant'
    mutants += 1
    report = {'actors': required, 'registrations': {a: inventory(sources)[a] for a in required},
              'scenario_actor_sets': scenarios, 'runtime_cases': cases, 'caught_mutants': mutants,
              'scope': 'registry and direct callback source checks; no hardware or full call-graph proof'}
    (args.output / 'coverage.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS grant-derived failure return: %d actors, %d runtime cases, %d caught mutants' % (len(required), cases, mutants))


if __name__ == '__main__':
    main()
