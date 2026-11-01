#!/usr/bin/env python3
"""Check eight-core latency coverage and report host QEMU maxima without hardware claims."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from harness import CACHE, environment

REGIONS = ('timer_irq', 'irq_masked', 'lock0_wait', 'lock0_hold', 'lock1_wait',
           'lock1_hold', 'wake_idle', 'wake_busy', 'DMA_completion',
           'GPU_completion', 'NPU_completion')
BUDGETS = (50, 25, 25, 25, 25, 25, 100, 250, 250, 1000, 1000)


def fields(line):
    return {key: int(value, 16) for key, value in re.findall(r'(\w+)=(0x[0-9a-f]+)', line)}


def decode(log):
    assert 'MODEL latency: host QEMU timing' in log
    assert 'PASS latency model:' in log and 'FAIL' not in log
    frequency = None
    cores, traffic = {}, {}
    core = None
    for line in log.splitlines():
        values = fields(line)
        if line.startswith('frequency='):
            frequency = values['frequency']
            assert values['modeled_DMA_jobs'] > 1
        elif line.startswith('core='):
            core = values['core']
            assert core not in cores and 0 <= core < 8
            cores[core], traffic[core] = {}, values
            assert values['sweeps'] > 0
            assert values['bytes_read'] == values['sweeps'] * 1024 * 1024
            assert values['bytes_written'] == values['bytes_read']
        elif line.split(' ', 1)[0] in REGIONS:
            region = line.split(' ', 1)[0]
            assert core is not None and region not in cores[core]
            cores[core][region] = values
    assert frequency and set(cores) == set(range(8))
    maxima, overruns = {}, []
    for c, metrics in cores.items():
        assert set(metrics) == set(REGIONS)
        assert metrics['wake_idle']['count'] == 64
        assert metrics['wake_busy']['count'] == 128
        assert metrics['irq_masked']['count'] > 0
        assert metrics['GPU_completion']['count'] == metrics['NPU_completion']['count'] == 0
        for r, budget in zip(REGIONS, BUDGETS):
            m = metrics[r]
            us = m['max_ticks'] * 1000000 / frequency
            maxima[r] = max(maxima.get(r, 0), us)
            # First-fault context must accompany each latched budget overrun.
            if m['first_over_ticks']:
                assert m['first_over_start'] and m['first_over_site']
                assert m['first_over_ticks'] <= m['max_ticks']
            if us > budget:
                assert m['first_over_ticks']
                overruns.append(dict(core=c, region=r, maximum_us=us, **m))
    assert cores[0]['timer_irq']['count'] > 0
    assert cores[0]['DMA_completion']['count'] > 1
    return dict(evidence='host QEMU; DMA hardware modeled; no board timing',
                frequency=frequency, maxima_us=maxima, cores=cores,
                traffic=traffic, overruns=overruns)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--log', type=Path, help='Decode a preserved run without repeating execution')
    parser.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = args.image.resolve()
    command = [str(args.qemu), '-M', 'virt,gic-version=3,virtualization=off',
               '-cpu', 'cortex-a53', '-smp', '8', '-m', '2G', '-nographic',
               '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
               '-accel', 'tcg,thread=single', '-icount', 'shift=0,sleep=off', '-kernel', str(image)]
    if args.log:
        log = args.log.read_text()
    else:
        run = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, env=environment(), timeout=60)
        log = run.stdout
        (args.output / 'qemu.log').write_text(log)
        assert run.returncode == 0, log[-2000:]
    (args.output / 'qemu.log').write_text(log)
    result = decode(log)
    result.update(image=str(image), image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                  log_sha256=hashlib.sha256(log.encode()).hexdigest(), command=command,
                  preserved_log=str(args.log) if args.log else None)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(result='PASS coverage; budgets are reported separately',
                          evidence=result['evidence'], maxima_us=result['maxima_us']), indent=2))


if __name__ == '__main__':
    main()
