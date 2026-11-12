#!/usr/bin/env python3
"""Check DMA retirement without hardware writes or deadline extension."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['drivers/dma/a733_dma_platform.c', 'include/a733_dma_platform.h',
                'checks/dma-platform.c', 'checks/dma-adopted-return.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    exe = args.output / 'dma-adopted-return'
    cmd = [str(CLANG), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
           '-I' + str(ROOT / 'include'), str(ROOT / 'checks/dma-adopted-return.c'),
           str(ROOT / 'drivers/dma/a733_dma_platform.c'),
           str(ROOT / 'drivers/clock/a733_clock_leases.c'), '-o', str(exe)]
    for name, command in [('build', cmd), ('run', [str(exe)])]:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=environment())
        (args.output / (name + '.log')).write_text(result.stdout)
        assert result.returncode == 0, result.stdout
    source = ROOT / 'drivers/dma/a733_dma_platform.c'
    original = source.read_text()
    mutations = {
        'skip-channel-drain': ('return quiet(p)&&rd(p,DMA+A733_DMA_REG_CPU_ROUTE',
                               'return rd(p,DMA+A733_DMA_REG_CPU_ROUTE'),
        'expect-initial-route': ('(cpu&0xffff)==p->config.desired_cpu',
                                 '(cpu&0xffff)==p->config.expected_cpu'),
        'erase-cold-policy': ('struct a733_dma_platform check=*p;',
                              'p->config.cold=0;struct a733_dma_platform check=*p;'),
    }
    for name, (before, after) in mutations.items():
        assert original.count(before) == 1, name
        mutant = args.output / (name + '.c')
        mutant.write_text(original.replace(before, after))
        binary = args.output / name
        changed = [str(mutant) if v == str(source) else str(binary) if v == str(exe) else v for v in cmd]
        result = subprocess.run(changed, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=environment())
        (args.output / (name + '-build.log')).write_text(result.stdout)
        assert result.returncode == 0, result.stdout
        result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=environment())
        (args.output / (name + '-run.log')).write_text(result.stdout)
        assert result.returncode != 0 and 'Assertion' in result.stdout, (name, result.stdout)
    (args.output / 'result.json').write_text(json.dumps(dict(
        command=cmd, sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        evidence='host register model; no physical access', mutants_rejected=list(mutations),
        result='PASS'), indent=2) + '\n')
    print('PASS adopted and cold DMA return model; three mutants rejected')


if __name__ == '__main__':
    main()
