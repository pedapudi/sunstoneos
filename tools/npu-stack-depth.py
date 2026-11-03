#!/usr/bin/env python3
"""Static peak stack depth of the NPU loading and service paths, from compiler stack usage and the call graph.

The OS NPU service worker (npu_service.c) and the loading task run LOAD steps on fixed stacks. This tool compiles the NPU
units for the OS target (AArch64, the flags of an OS image build directory) with -fstack-usage, reads the call edges from the
relocations of the objects with llvm-objdump, adds the edges through the function pointers of the runtime hooks by hand
(HOOKS below), and prints for each entry function the deepest chain and its stack bytes. The sum counts every frame on the
chain once, with no allowance for interrupts. It is an upper bound for a chain without recursion, which the tool checks.

  npu-stack-depth.py --flags FLAGS_MAKE --output DIR [--entry NAME ...]

FLAGS_MAKE is the CMakeFiles/sunstone-a733-os.dir/flags.make of an OS image build with the NPU (DEFINES, INCLUDES, FLAGS).
"""
import argparse, re, subprocess, os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNITS = ('apps/os/npu.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c', 'drivers/npu/npu_pool.c', 'drivers/npu/npu_service.c',
         'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c', 'drivers/npu/a733_npu_mmu.c',
         'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c',
         'drivers/npu/npu_declaration.c', 'drivers/storage/storage_digest.c')
# Calls through function pointers: caller -> callee (the hooks a runtime installs, the controller's operations).
HOOKS = {'runtime_worker': ('step', 'request', 'finished', 'closed'), 'execute_runtime': ('request', 'closed'),
         'runtime_settle': ('finished',), 'irq_progress': ('read_reg', 'write_reg', 'publish', 'finish', 'delay', 'now', 'barrier'),
         'start': (), 'step': ('start', 'load_step', 'prepare_step', 'check_step')}
DEFAULT_ENTRIES = ('load_work', 'runtime_worker', 'worker')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--flags', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--entry', action='append')
    ap.add_argument('--root', type=Path, default=ROOT, help='the source tree to measure (default: this one)')
    a = ap.parse_args()
    text = a.flags.read_text()
    get = lambda k: re.search(r'^C_%s = (.*)$' % k, text, re.M).group(1)
    a.output.mkdir(parents=True)
    edges, usage = {}, {}
    for unit in UNITS:
        obj = a.output / (Path(unit).name + '.o')
        cmd = 'clang --target=aarch64-none-elf %s %s %s -fstack-usage -ffunction-sections -c %s -o %s' % (get('DEFINES'), get('INCLUDES'), get('FLAGS'), a.root / unit, obj)
        env = dict(os.environ, PATH=str(Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin') + ':' + os.environ['PATH'])
        subprocess.run(cmd, shell=True, check=True, env=env)
        for line in (a.output / (Path(unit).name + '.su')).read_text().splitlines():
            name, size = line.split('\t')[0].split(':')[-1], int(line.split('\t')[1])
            usage[name] = size
        dump = subprocess.run(['/usr/bin/llvm-objdump-21', '-dr', str(obj)], capture_output=True, text=True, env=env).stdout
        current = None
        for line in dump.splitlines():
            m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
            if m:
                current = m.group(1).split('.')[0]; edges.setdefault(current, set()); continue
            m = re.search(r'R_AARCH64_(?:CALL26|JUMP26)\s+(\S+)', line)
            if m and current: edges[current].add(re.sub(r'^\.text\.(startup\.|unlikely\.|hot\.)?', '', m.group(1).split('+')[0]))
    for caller, callees in HOOKS.items():
        edges.setdefault(caller, set()).update(callees)
    memo, stack = {}, []
    def depth(f):
        if f in memo: return memo[f]
        if f in stack: raise SystemExit('recursion through ' + f)
        stack.append(f)
        best = (0, [])
        for g in edges.get(f, ()):
            if g in usage or g in edges:
                d = depth(g)
                if d[0] > best[0]: best = d
        stack.pop()
        memo[f] = (usage.get(f, 0) + best[0], [f] + best[1])
        return memo[f]
    for entry in a.entry or DEFAULT_ENTRIES:
        if entry not in usage: print(entry, 'not found'); continue
        total, chain = depth(entry)
        print('%s: %d bytes' % (entry, total))
        print('   ' + ' > '.join('%s(%d)' % (f, usage.get(f, 0)) for f in chain))


if __name__ == '__main__':
    main()
