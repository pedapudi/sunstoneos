#!/usr/bin/env python3
"""Capture the pinned convolution's SDK preparation through intercepted submission.

Reuses the synthetic HAL and bounded execution from nbg-loader-reference.py.
No driver or device executes; this records bytes and allocation behavior for
subsequent native assembly comparison.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn.arm64_const import UC_ARM64_REG_PC


MODEL_SHA = 'db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sdk', type=Path)
    ap.add_argument('model', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    path = Path(__file__).with_name('nbg-loader-reference.py')
    spec = importlib.util.spec_from_file_location('loader', path)
    loader = importlib.util.module_from_spec(spec); spec.loader.exec_module(loader)
    sdk, model = args.sdk.read_bytes(), args.model.read_bytes()
    assert hashlib.sha256(sdk).hexdigest() == loader.SDK_SHA
    catalogue=json.loads((Path(__file__).resolve().parents[1]/'tools/npu-models.json').read_text())['models']
    selected=next((v for v in catalogue if v['model']['sha256']==hashlib.sha256(model).hexdigest() and v['model']['bytes']==len(model)),None)
    assert selected is not None, 'Model is absent from reviewed build-time catalogue'
    selected_sha=selected['model']['sha256']
    assert struct.unpack_from('<I',model,4)[0]==0x20000
    directory_count=struct.unpack_from('<I',model,56)[0]
    sections={struct.unpack_from('<I',model,60+i*44)[0]:struct.unpack_from('<QQ',model,64+i*44) for i in range(directory_count)}
    assert all(sections[t][1]==112 for t in (1,2))
    args.output.mkdir(parents=True, exist_ok=False)
    ref = loader.Reference(sdk, 0xe0000000)
    error, network, reached = None, 0, False
    try:
        ref.call('vip_init', 0)
        address = ref.allocate(len(model))['cpu']; ref.uc.mem_write(address, model)
        result = ref.allocate(8)['cpu']
        ref.call('vip_create_network', address, len(model), 2, result)
        network = ref.read64(result)
        ref.snapshot(args.output, 'created', network)
        ref.call('vip_prepare_network', network)
        ref.snapshot(args.output, 'prepared', network)
        for tensor, operation in ((sections[1][0], 'vip_set_input'), (sections[2][0], 'vip_set_output')):
            params = bytearray(48)
            params[:32] = model[tensor:tensor + 32]
            assert struct.unpack_from('<I', model, tensor + 36)[0] == 2
            struct.pack_into('<I', params, 32, 2)
            params[36:44] = model[tensor + 40:tensor + 48]
            pointer = ref.allocate(48)['cpu']; ref.uc.mem_write(pointer, bytes(params))
            ref.call('vip_create_buffer', pointer, 48, result)
            ref.call(operation, network, 0, ref.read64(result))
            ref.bindings.append(dict(operation=operation, index=0,
                                     memory_handle=ref.allocations[-1]['handle']))
        ref.snapshot(args.output, 'bound', network)
        ref.call('vip_trigger_network', network)
        raise RuntimeError('Trigger returned without reaching intercepted submission')
    except loader.SubmissionReached:
        reached = True
        ref.snapshot(args.output, 'submission', network)
    except Exception as exc:
        error = str(exc)
    report = dict(result='submission_intercepted' if reached else 'incomplete', error=error,
                  network=network, pc=hex(ref.uc.reg_read(UC_ARM64_REG_PC) - loader.BASE),
                  allocations=ref.allocations, calls=ref.calls, logs=ref.logs,
                  tasks=ref.tasks, bindings=ref.bindings, physical_access=False,
                  sdk_sha256=loader.SDK_SHA, model_sha256=selected_sha,
                  comparison=None, video_base=0xe0000000)
    (args.output/'trace.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(dict(result=report['result'], error=error,
                          video_allocations=[v for v in ref.allocations if v['video']]), indent=2))
    if not reached:
        raise SystemExit(error or 'Submission was not reached')


if __name__ == '__main__':
    main()
