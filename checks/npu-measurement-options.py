#!/usr/bin/env python3
"""The NPU measurement options are off in every build that does not ask for them by name.

Three CMake cache entries select NPU measurement code (cmake/sunstone-os-core.cmake): SUNSTONE_NPU_TENSOR_PAGE_SHIFTS (larger
NPU MMU pages for chosen slots), SUNSTONE_NPU_TRAFFIC_COUNTERS (the NPU's traffic and cycle counters around every job) and
SUNSTONE_NPU_OP_TIMING (jobs cut after an operation, which also waits for the NPU to be idle inside the completion handler
for up to the job's timeout). The check requires that
  - each entry's default is empty or OFF, and its compile definition is set only inside if(<entry>);
  - no other build file, tool or image recipe sets one (a -D flag, a set(), or NAME=value), apart from the host checks that
    build the runtime with a stated option on purpose (ALLOWED);
  - in the queue, the cut and the long idle wait are compiled only under #ifdef SUNSTONE_NPU_OP_TIMING.
It then applies each rule to a mutated copy (a default turned on, a definition outside its if(), a recipe that sets the option,
the long wait outside its #ifdef) and requires the rule to catch it.
"""
import argparse, re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['cmake/sunstone-os-core.cmake', 'drivers/npu/npu_runtime_queue.c', 'checks/os-npu-graphs.py']
OPTIONS = ('SUNSTONE_NPU_TENSOR_PAGE_SHIFTS', 'SUNSTONE_NPU_TRAFFIC_COUNTERS', 'SUNSTONE_NPU_OP_TIMING')
CMAKE = 'cmake/sunstone-os-core.cmake'
QUEUE = 'drivers/npu/npu_runtime_queue.c'
# Host checks that build the runtime with an option on, each for its own stated case.
ALLOWED = {'checks/os-npu-graphs.py': {'SUNSTONE_NPU_TENSOR_PAGE_SHIFTS'}}
SCANNED = ('cmake', 'tools', 'apps', 'checks', 'CMakeLists.txt')
SUFFIXES = {'.cmake', '.txt', '.py', '.sh', '.json', '.c', '.h'}


def cmake_faults(text):
    faults = []
    for name in OPTIONS:
        default = re.search(r'set\(%s\s+"([^"]*)"\s+CACHE' % name, text) or re.search(r'option\(%s\s+"[^"]*"\s+(\w+)\)' % name, text)
        if not default:
            faults.append('%s: no cache entry' % name)
        elif default.group(1) not in ('', 'OFF'):
            faults.append('%s: default %r' % (name, default.group(1)))
        depth, guarded = 0, []
        for line in text.splitlines():
            s = line.strip()
            if re.match(r'if\(%s\)' % name, s):
                guarded.append(True); continue
            if s.startswith('if('):
                guarded.append(False); continue
            if s.startswith('endif('):
                guarded.pop() if guarded else None; continue
            if 'target_compile_definitions' in s and name + '=' in s and not any(guarded):
                faults.append('%s: compile definition outside if(%s)' % (name, name))
    return faults


def recipe_faults(files):
    faults = []
    for path, text in files.items():
        if path == CMAKE:
            continue
        for name in OPTIONS:
            if name in ALLOWED.get(path, ()):
                continue
            if re.search(r'-D\s*%s\b|set\(\s*%s\b|[\'"]%s=' % (name, name, name), text):
                faults.append('%s sets %s' % (path, name))
    return faults


def queue_faults(text):
    """Lines of the cut and of the long wait must lie inside #ifdef SUNSTONE_NPU_OP_TIMING."""
    faults, stack = [], []
    for number, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if s.startswith('#if'):
            stack.append(s.startswith('#ifdef SUNSTONE_NPU_OP_TIMING')); continue
        if s.startswith('#endif'):
            stack.pop() if stack else None; continue
        if s.startswith('#else'):
            if stack: stack[-1] = False
            continue
        if ('cut_apply' in s or 'cut_restore' in s or 'bound = q->timeout_ticks' in s) and not any(stack):
            faults.append('%s:%d outside #ifdef SUNSTONE_NPU_OP_TIMING: %s' % (QUEUE, number, s))
    return faults


def gather():
    files = {}
    for top in SCANNED:
        base = ROOT / top
        paths = [base] if base.is_file() else [p for p in base.rglob('*') if p.is_file() and p.suffix in SUFFIXES]
        for p in paths:
            rel = str(p.relative_to(ROOT))
            if rel == 'checks/npu-measurement-options.py':
                continue
            files[rel] = p.read_text(errors='replace')
    return files


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    files = gather()
    cmake, queue = files[CMAKE], (ROOT / QUEUE).read_text()
    faults = cmake_faults(cmake) + recipe_faults(files) + queue_faults(queue)
    assert not faults, faults
    # Each rule catches its mutation.
    on = cmake.replace('set(SUNSTONE_NPU_OP_TIMING ""', 'set(SUNSTONE_NPU_OP_TIMING "64"', 1)
    assert on != cmake and cmake_faults(on), 'a default turned on is not caught'
    loose = cmake + '\ntarget_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_NPU_OP_TIMING=64u)\n'
    assert cmake_faults(loose), 'a definition outside its if() is not caught'
    recipe = dict(files); recipe['tools/npu-app-image.py'] = recipe.get('tools/npu-app-image.py', '') + "\n'-DSUNSTONE_NPU_OP_TIMING=64'\n"
    assert recipe_faults(recipe), 'a recipe that sets the option is not caught'
    wait = '\n#ifdef SUNSTONE_NPU_OP_TIMING\n'
    assert wait in queue
    exposed = queue.replace('        if (q->cut_at) bound = q->timeout_ticks;\n#endif', '#endif\n        if (q->cut_at) bound = q->timeout_ticks;', 1)
    assert exposed != queue and queue_faults(exposed), 'the long wait outside its #ifdef is not caught'
    print('PASS NPU measurement options: off by default, set only inside their if(), by no recipe, and the cut and its idle wait '
          'compiled only under SUNSTONE_NPU_OP_TIMING; four mutations caught')


if __name__ == '__main__':
    main()
