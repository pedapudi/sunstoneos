#!/usr/bin/env python3
"""Check the selected firmware ABI, GPU transition providers and shared engine."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'checks' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def abi(out, sources, model):
    """Compile the selected native headers, preserving their license and layouts."""
    inputs = {}
    manifest = json.loads((sources.parent / 'manifest.json').read_text())
    for name in ('pvr_rogue_fwif.h', 'pvr_rogue_defs.h', 'pvr_rogue_cr_defs.h',
                 'pvr_rogue_fwif_common.h', 'pvr_rogue_fwif_shared.h',
                 'pvr_rogue_fwif_shared_check.h', 'pvr_rogue_fwif_check.h'):
        raw = (sources / name).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        assert [r['sha256'] for r in manifest if r['path'] == 'imagination/' + name] == [digest]
        inputs[str(sources / name)] = digest
        (out / name).write_text(re.sub(r'^#include <linux/[^>]+>\n', '', raw.decode(), flags=re.M))
    shim = module('pvr-compute-context').SHIM.split('#include "pvr_rogue_fwif_stream.h"')[0]
    probe = out / 'abi.c'
    probe.write_text(shim + r'''
#include <stdio.h>
#include "pvr_rogue_fwif_check.h"
int main(void) {
#define O(t,m) offsetof(struct t,m)
    printf("%u %u %zu %zu %zu %zu %u %u %u %u %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu\n",
      ROGUE_FWIF_KCCB_CMD_POW & 0xffff, ROGUE_FWIF_KCCB_CMD_CORECLKSPEEDCHANGE & 0xffff,
      sizeof(struct rogue_fwif_kccb_cmd), O(rogue_fwif_kccb_cmd,cmd_data),
      O(rogue_fwif_power_request,power_req_data),O(rogue_fwif_coreclkspeedchange_data,new_clock_speed),
      ROGUE_FWIF_POW_FORCED_IDLE_REQ,ROGUE_FWIF_POWER_FORCE_IDLE,ROGUE_FWIF_POWER_CANCEL_FORCED_IDLE,
      ROGUE_FWIF_POW_FORCED_IDLE,O(rogue_fwif_runtime_cfg,core_clock_speed),
      sizeof(struct rogue_fwif_time_corr),O(rogue_fwif_time_corr,os_timestamp),
      O(rogue_fwif_time_corr,os_mono_timestamp),O(rogue_fwif_time_corr,cr_timestamp),
      O(rogue_fwif_time_corr,cr_delta_to_os_delta_kns),O(rogue_fwif_time_corr,core_clock_speed),
      O(rogue_fwif_time_corr,reserved),O(rogue_fwif_gpu_util_fwcb,time_corr_seq_count),
      O(rogue_fwif_sysdata,pow_state));
    return 0;
}
''')
    exe = model.build(out, 'abi', [probe])
    values = list(map(int, model.run(out, 'abi', [exe]).split()))
    assert values == [107, 204, 88, 8, 4, 0, 2, 1, 2, 2, 12, 40, 0, 8, 16, 24, 32, 36, 10240, 8], values
    (out / 'abi.json').write_text(json.dumps({'values': values, 'inputs': inputs}, indent=2) + '\n')
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sources', type=Path, required=True)
    ap.add_argument('--cases', nargs='+', choices=('abi', 'clock', 'platform', 'engine', 'os'),
                    default=['abi', 'clock', 'platform', 'engine', 'os'])
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    model = module('perf-mode')
    results = {}
    if 'abi' in args.cases or 'clock' in args.cases:
        values = abi(out, args.sources, model)
        results['abi'] = values
    selected = {
        'clock': (['checks/gpu-performance-clock.c', 'drivers/gpu/pvr_gpu_clock.c',
                   'drivers/gpu/pvr_gpu_control.c', 'drivers/gpu/pvr_ccb.c'], []),
        'platform': (['checks/gpu-performance-platform.c',
                      *[p for p in model.STARTUP[1:] if not p.startswith('drivers/npu/')],
                      'drivers/gpu/a733_gpu_platform.c', 'drivers/gpu/a733_gpu_performance.c'],
                     ['-DSUNSTONE_A733_OS_PERF=1']),
        'engine': (['checks/perf-mode-model.c', *model.ENGINE], [])}
    for name in args.cases:
        if name not in selected:
            continue
        sources, flags = selected[name]
        if name == 'clock':
            flags = ['-DNATIVE_POWER_COMMAND=%d' % values[0], '-DNATIVE_CORECLK_COMMAND=%d' % values[1]]
        exe = model.build(out, name, sources, flags)
        results[name] = model.run(out, name, [exe]).strip()
        print(results[name])
    if 'os' in args.cases:
        results['os'] = {str(npu): model.os_model(out, npu) for npu in (False, True)}
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
