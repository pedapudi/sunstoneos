#!/usr/bin/env python3
"""Execute routing-epoch races and require the real stale-validation negative."""
import argparse
import os
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build-dir', '{output}']]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = dict(os.environ, TMPDIR=str(build))
    source = ROOT / 'kernel/scheduler/freertos_control.c'
    common = ['/usr/bin/clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror',
              '-I' + str(ROOT / 'kernel/scheduler')]
    def compile_host(control, output):
        subprocess.run([*common, '-DSS_CONTROL_ROUTE_TEST', '-O1', '-g', '-pthread',
                        '-fsanitize=address,undefined', str(control),
                        str(ROOT/'kernel/scheduler/owner.c'),
                        str(ROOT/'checks/scheduler-route.c'), '-o', str(output)],
                       check=True, env=env, cwd=build)
    binary = build / 'routes'
    compile_host(source, binary)
    positive = subprocess.run([str(binary)], env=env, cwd=build, timeout=20,
                              text=True, capture_output=True)
    (build/'positive.log').write_text(positive.stdout+positive.stderr)
    assert positive.returncode == 0, positive.stdout+positive.stderr
    print(positive.stdout, end='')
    validation = 'atomic_load_explicit(&s->route_word, memory_order_acquire) == word'
    text = source.read_text()
    assert text.count(validation) == 1
    broken = build / 'control-no-validation.c'
    broken.write_text(text.replace(validation, 'true'))
    negative = build / 'routes-no-validation'
    compile_host(broken, negative)
    result = subprocess.run([str(negative)], env=env, cwd=build, timeout=20,
                            text=True, capture_output=True)
    (build/'negative.log').write_text(result.stdout+result.stderr)
    assert result.returncode != 0 and 'stale route admitted after bank reuse' in result.stderr
    obj = build / 'control-aarch64.o'
    subprocess.run([*common, '--target=aarch64-none-elf', '-march=armv8.1-a+lse',
                    '-ffreestanding', '-O2', '-c', str(source), '-o', str(obj)], check=True, env=env)
    dis = subprocess.check_output(['llvm-objdump-21', '-dr', str(obj)], text=True)
    (build/'control-aarch64.disassembly').write_text(dis)
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', dis)
    assert 'ss_control_route_test_sample' not in dis
    print('scheduler routes: removing epoch validation admits a stale route; native code has no LL/SC or test hook')

if __name__ == '__main__':
    main()
