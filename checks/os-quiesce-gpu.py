#!/usr/bin/env python3
"""Check GPU registration and failure-receipt admission without hardware.

The service worker and completed-output drain execute in gpu-queue cases
69 through 75. This check covers the OS binding and its host receipt policy.
"""
import argparse
import importlib.util
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/gpu.c', 'apps/os/gpu-return.py', 'apps/os/quiesce.h',
                'drivers/gpu/pvr_gpu_service.c', 'drivers/gpu/pvr_compute_queue.c']


def function(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text)
    assert match, name
    begin = text.index('{', match.start())
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


def source_problem(text):
    if 'os_quiesce_register(&gpu_quiesce)' not in function(text, 'os_gpu_queue_start'):
        return 'GPU service has no quiesce registration'
    step = function(text, 'gpu_quiesce_step')
    if re.search(r'\b(platform_exit|board_fail|fail_close|a733_smp_exit_privileged)\s*\(', step):
        return 'GPU quiesce step stops the board'
    if 'os_quiesce_fault(' not in step or 'pvr_gpu_service_quiesce(' not in step:
        return 'GPU step bypasses its owner protocol'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    text = (ROOT / 'apps/os/gpu.c').read_text()
    assert source_problem(text) is None, source_problem(text)
    registration = 'if(!os_quiesce_register(&gpu_quiesce))return 0;'
    assert text.count(registration) == 1
    assert source_problem(text.replace(registration, '')) == 'GPU service has no quiesce registration'
    step = function(text, 'gpu_quiesce_step')
    mutated = text.replace(step, step.replace('{', '{ platform_exit(6);', 1))
    assert source_problem(mutated) == 'GPU quiesce step stops the board'

    spec = importlib.util.spec_from_file_location('gpu_return', ROOT / 'apps/os/gpu-return.py')
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    layout = {'allow_untouched': True, 'allow_failure': True}
    words = [decoder.MAGIC, 4, 64, 123, 456, 7, 63, 960000 << 32 | 2]
    def decode(fields=words, capability=layout, failure=True):
        return decoder.decode_captured(struct.pack('<8Q', *fields), capability, 123, 456, failure_return=failure)
    result = decode()
    assert result['gpu_off'] and result['worker_parked'] and result['abandoned_outputs'] == 2
    assert not result['completed_gpu_work']
    refused = 0
    for fields, capability, failure in [
        (words, layout, False), (words, {'allow_untouched': True}, True),
        (words[:3] + [124] + words[4:], layout, True),
        (words[:6] + [62, words[7]], layout, True),
        (words[:7] + [960000 << 32 | 8], layout, True),
    ]:
        try:
            decode(fields, capability, failure)
        except ValueError:
            refused += 1
        else:
            raise AssertionError('invalid failure receipt accepted')
    assert refused == 5
    (args.output / 'results.json').write_text(json.dumps({
        'physical_access': False, 'registration_mutants_rejected': 2,
        'valid_failure_receipts': 1, 'invalid_failure_receipts_rejected': refused}, indent=2) + '\n')
    print('PASS GPU quiesce registration: two mutants rejected; failure receipt accepted only with failure and image capability')


if __name__ == '__main__':
    main()
