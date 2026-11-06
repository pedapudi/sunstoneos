#!/usr/bin/env python3
"""NPU MMU exceptions through the real OS NPU runtime and service on the host.

Six single-operation graphs (sig, tanh, add, sub, swish, relu) run through LOAD and SUBMIT of the production wrapper,
runtime and service (checks/os-npu-service.c, case 25). The image is built as the board image of the unmapped-address
runs is: the first five graphs have one descriptor pointer aimed at an address the NPU page tables do not map (a canary
buffer in DRAM, or page 0 for the fifth), the sixth is untouched. The register file raises the MMU exception that the
board's NPU raises (event bit 30, status and address registers) when it finds that address in a descriptor.

Scenarios: the exception with recovery (five failed jobs, one good job, no failure of the image), the same with the
interrupt controller answering "not yet" first, with a cancel handshake that never completes, and the candidate sites at
which the board might block: each register read of the capture, the cancel handshake's read, and the interrupt mask. A
blocking site must show in the failure record. Two controls let the access through: the job completes and the wrapper
must record it, with the canary unchanged for a read and changed for a write.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}', '--graphs', '{fixture:npu-graphs}']]
CACHE = Path.home() / '.cache/sunstoneos'
CHECK_INPUTS = ('tools/npu-fixtures.json', 'apps/os/npu.c', 'apps/os/npu-declaration.c', 'apps/os/npu.h', 'apps/npu-graphs/models-r.json', 'drivers/npu/a733_npu_controller.c',
                'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/nbg_assembly.c',
                'checks/os-npu-service.c', 'checks/os-npu-fault.py', 'runtime/npu_app.c')
UNITS = ('checks/os-npu-service.c', 'kernel/umem/umem.c', 'drivers/npu/npu_declaration.c', 'drivers/npu/a733_npu_runtime.c', 'drivers/npu/a733_npu_runtime_interim.c', 'drivers/npu/npu_pool.c',
         'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_memory.c',
         'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
         'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c')
DEFINES = ['SUNSTONE_A733_OS=1', 'SUNSTONE_A733_OS_NPU_OBSERVE=1',
           'SUNSTONE_BOARD_APPLICATION=1', 'SUNSTONE_BOARD_SUPERVISED=1',
           'SUNSTONE_NPU_FAULT_HOOK=1', 'SUNSTONE_NPU_MMU_CAPTURE_ON_TIMEOUT=1',
           'SUNSTONE_NPU_CASE_SLOTS=1,2,1,2,1,-1', 'SUNSTONE_NPU_CASE_ADDRESSES=0,0,0,0,0x180,0']
MARK = {'captured': 1, 'progress': 2, 'settle': 4, 'finished': 8, 'reply': 16, 'cancel': 32, 'reinit': 64,
        'ready': 128, 'collect': 256, 'woke': 512, 'read': 1024, 'failed': 2048}
REPLY_PATH = MARK['captured'] | MARK['progress'] | MARK['settle'] | MARK['finished'] | MARK['reply'] | \
    MARK['collect'] | MARK['woke'] | MARK['read']

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--graphs', type=Path, default=CACHE / 'build/npu-graph-variants')
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
artifacts = json.loads((args.graphs / 'artifacts.json').read_text())
NETWORK = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
generated = out / 'generated'; generated.mkdir()
models = ROOT / 'apps/npu-graphs/models-r.json'
(out / 'artifacts.json').write_text(json.dumps(artifacts))
subprocess.run([sys.executable, str(ROOT / 'tools/npu-test-models.py'), '--artifacts', str(out / 'artifacts.json'),
                '--models', str(models), '--model', '0', '--all', '--output', str(generated / 'npu-models.c')], check=True)
inputs = out / 'inputs'; inputs.mkdir()
for number, name in enumerate(json.loads(models.read_text())['npu_models']):
    for k, path in enumerate(artifacts[name]['inputs']):
        shutil.copy(path, inputs / ('%02d-%d.bin' % (number, k)))
includes = [ROOT / 'apps/os', ROOT / 'checks', generated, ROOT / 'apps/board', ROOT / 'platform/a733-storage-update',
            ROOT / 'platform/a733-smp-cached', ROOT / 'drivers/usb', ROOT / 'network/include', ROOT / 'platform/qemu-smp',
            NETWORK / 'include', NETWORK / 'portable/Compiler/GCC']
exe = host_rtos.build(out / 'exe', [*UNITS, generated / 'npu-models.c'],
                      'os-npu-fault', defines=DEFINES, includes=includes,
                      flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections', '-fno-pie'],
                      link=['-Wl,--gc-sections', '-no-pie'])


def run(name, **env):
    done = subprocess.run([str(exe), '25', str(inputs)], capture_output=True, text=True,
                          env={**environment(), **env}, timeout=1800)
    (out / (name + '.log')).write_text(done.stdout + done.stderr)
    assert done.returncode == 0, (name, done.returncode, done.stdout[-600:], done.stderr[-800:])
    jobs, summary = [], None
    for line in done.stdout.splitlines():
        fields = line.split()
        if fields[:1] == ['job']:
            jobs.append({'result': int(fields[3]), 'status': int(fields[5]), 'address': int(fields[7], 16),
                         'canary': int(fields[9], 16), 'marks': int(fields[11], 16), 'window': int(fields[13])})
        elif fields[:1] == ['summary']:
            summary = {fields[i]: int(fields[i + 1], 16 if fields[i + 1].startswith('0x') else 10)
                       for i in range(1, len(fields), 2)}
    assert summary, name
    return jobs, summary


def ticket(summary):
    return summary['site'] >> 16 & 255


def recovered(name, **env):
    """Five failed jobs with their records, one good job, and an image that did not fail."""
    jobs, s = run(name, **env)
    assert [j['result'] for j in jobs] == [2, 2, 2, 2, 2, 1], (name, jobs)
    assert s['finished'] == 1 and s['failures'] == 0 and s['runtime_failed'] == 0 and s['recoveries'] == 5 and \
        s['faulted_jobs'] == 5, (name, s)
    pattern = s['pattern']
    for j in jobs[:5]:
        assert j['status'] == 2 and j['canary'] == pattern and j['marks'] & REPLY_PATH == REPLY_PATH, (name, j)
        assert j['window'] == 8000, (name, j)
    canary_address = jobs[0].get('address')
    assert all(j['address'] == canary_address for j in jobs[:4]) and canary_address % 64 == 0, (name, jobs)
    assert jobs[4]['address'] == 0x180, (name, jobs[4])
    assert s['canary_now'] == pattern, (name, s)
    print('  ', name, ': five jobs failed with status 2 and the canary intact, the good job completed, five recoveries')
    return jobs, s


def blocked(name, stage, **env):
    """The worker blocks: the first job's wait ends in the failure record, which names the stage and the marks."""
    jobs, s = run(name, **env)
    assert s['failures'] == 1 and jobs[0]['result'] == -1, (name, jobs, s)
    assert ticket(s) & 15 == stage and s['value'] >> 16 == MARK['collect'] and s['value'] & 0xffff == 8000, (name, s)
    assert s['caller'] == s['pattern'], (name, s)  # the canary is unchanged
    print('  ', name, ': the record names stage', stage, 'marks', hex(s['value'] >> 16), 'window', s['value'] & 0xffff)


recovered('recovery')
recovered('recovery-gic-defers', NPU_GIC_DEFER='3')
recovered('recovery-cancel-never-completes', NPU_NO_CANCEL='1')
blocked('blocks-on-event-read', 0, NPU_STALL_READ='0x10')
blocked('blocks-on-status-read', 2, NPU_STALL_READ='0x384')
blocked('blocks-on-address-read', 4, NPU_STALL_READ='0x380')
# The cancel handshake's read never returns. The failed job's reply is held until the slot is checked and the recovery
# has run (the recovery comes first), so the job's wait ends in the failure record, whose marks show that the capture,
# the progress pass, the settle step and the finished hook ran and the recovery did not finish.
jobs, s = run('blocks-in-cancel', NPU_STALL_READ='0x3a8')
assert jobs[0]['result'] == -1 and s['failures'] == 1, (jobs, s)
assert s['value'] >> 16 == MARK['captured'] | MARK['progress'] | MARK['settle'] | MARK['finished'] | MARK['collect'], s
print('   blocks-in-cancel : the record shows captured, progress, settle and finished marks and no reinit mark')
# The interrupt controller never answers the mask: the capture cannot run, the service retains the controller and the
# job fails with a controller fault, not with an MMU record. The failure is the image's.
jobs, s = run('blocks-in-mask', NPU_GIC_HANG='1')
assert jobs[0]['result'] == -1 and s['failures'] == 1 and s['value'] == 4, (jobs, s)  # NPU_SERVICE_FAULT
print('   blocks-in-mask : the job fails with a service fault (value', s['value'], '), no MMU record, as the code says')
# Controls: an access that is let through completes the job. The record has ticket byte 0xee and the canary CRC.
jobs, s = run('bypass-read', UNMAPPED_BYPASS='1')
assert jobs[0]['result'] == -1 and ticket(s) == 0xee and s['value'] == s['pattern'], (jobs, s)
jobs, s = run('bypass-write', UNMAPPED_BYPASS='1', UNMAPPED_BYPASS_WRITE='1')
assert jobs[0]['result'] == -1 and ticket(s) == 0xee and s['value'] != s['pattern'], (jobs, s)
print('   bypass controls : the completion is recorded, and a bypassing write changes the canary CRC')
# Shader operations: the first graph has the address word of its first tensor descriptor aimed at the canary (a data
# descriptor, S7), the second its program address (source 0); then four graphs run untouched. The register file finds the
# address in the command stream and raises the same exception.
shader_models = ROOT / 'apps/npu-graphs/models-shr.json'
shader_out = out / 'shader'; shader_out.mkdir()
(shader_out / 'artifacts.json').write_text(json.dumps(artifacts))
(shader_out / 'generated').mkdir()
subprocess.run([sys.executable, str(ROOT / 'tools/npu-test-models.py'), '--artifacts', str(shader_out / 'artifacts.json'),
                '--models', str(shader_models), '--model', '0', '--all', '--output', str(shader_out / 'generated/npu-models.c')], check=True)
(shader_out / 'inputs').mkdir()
for number, name in enumerate(json.loads(shader_models.read_text())['npu_models']):
    for k, path in enumerate(artifacts[name]['inputs']):
        shutil.copy(path, shader_out / 'inputs' / ('%02d-%d.bin' % (number, k)))
shader_exe = host_rtos.build(shader_out / 'exe', [*UNITS, shader_out / 'generated/npu-models.c'], 'os-npu-fault-shader',
                             defines=[d for d in DEFINES if not d.startswith('SUNSTONE_NPU_CASE')] +
                             ['SUNSTONE_NPU_CASE_SLOTS=100,101,-1,-1,-1,-1', 'SUNSTONE_NPU_CASE_ADDRESSES=0,0,0,0,0,0'],
                             includes=[ROOT / 'apps/os', ROOT / 'checks', shader_out / 'generated', *includes[3:]],
                             flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections', '-fno-pie'],
                             link=['-Wl,--gc-sections', '-no-pie'])
done = subprocess.run([str(shader_exe), '25', str(shader_out / 'inputs')], capture_output=True, text=True,
                      env=environment(), timeout=1800)
(out / 'shader.log').write_text(done.stdout + done.stderr)
assert done.returncode == 0, ('shader', done.stdout[-600:], done.stderr[-800:])
results = [int(l.split()[3]) for l in done.stdout.splitlines() if l.startswith('job ')]
summary = [l for l in done.stdout.splitlines() if l.startswith('summary')][0].split()
assert results == [2, 2, 1, 1, 1, 1] and 'recoveries' in summary and summary[summary.index('recoveries') + 1] == '2' and \
    summary[summary.index('failures') + 1] == '0', (results, summary)
print('   shader operations : a data descriptor and a program address aimed at the canary fail alone, the NPU recovers, four graphs run')
print('PASS NPU MMU exception recovery on the real runtime and service: 3 recovery scenarios, 5 blocking sites, 2 controls')
