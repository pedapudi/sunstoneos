#!/usr/bin/env python3
"""Run the pinned SDK simulator on same-shape UINT8 input; verify consumed bytes.

Use inside the pinned compiler image with its SDK library environment. This is
host simulation only. --check-saved verifies an existing run without reexecution.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('executable','model','input','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--check-saved',action='store_true')
    a = p.parse_args()
    catalogue = json.loads((Path(__file__).resolve().parent/'npu-models.json').read_text())['models']
    selected = next((m for m in catalogue if m['model']['sha256'] == sha(a.model) and
                     m['model']['bytes'] == a.model.stat().st_size),None)
    if selected is None: raise ValueError('Model is absent from reviewed catalogue')
    raw = a.input.read_bytes()
    if len(raw) != selected['input']['bytes']: raise ValueError('Input shape differs')
    if not a.check_saved: a.output.mkdir(parents=True,exist_ok=False)
    text = a.output/'input.qnt.tensor'
    encoded = ''.join(str(v)+'\n' for v in raw).encode()
    if a.check_saved:
        if text.read_bytes() != encoded: raise ValueError('Saved quantized input differs')
    else:
        text.write_bytes(encoded)
        command = [str(a.executable.resolve()),str(a.model.resolve()),str(text.resolve())]
        result = subprocess.run(command,cwd=a.output,stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,timeout=120)
        (a.output/'simulator.log').write_bytes(result.stdout)
        (a.output/'command.json').write_text(json.dumps(command,indent=2)+'\n')
        result.check_returncode()
    # The suffix selects UINT8 codes in the SDK's input reader. Check the dump
    # independently before treating any emitted output as this input's oracle.
    if (a.output/'input_0.dat').read_bytes() != raw:
        raise ValueError('SDK consumed input differs; numerical output is not accepted')
    matches = list(a.output.glob('output0_*.dat'))
    if len(matches) != 1 or matches[0].stat().st_size != selected['reference']['bytes']:
        raise ValueError('SDK output binding or extent differs')
    report = dict(result='pass',physical_access=False,model=selected['name'],
                  input_exact=True,reexecuted=not a.check_saved,
                  files={str(v.resolve()):dict(bytes=v.stat().st_size,sha256=sha(v))
                         for v in (a.executable,a.model,a.input,text,a.output/'input_0.dat',matches[0])})
    with (a.output/'input-check.json').open('x') as f: f.write(json.dumps(report,indent=2)+'\n')
    print('PASS SDK simulator consumed exact same-shape UINT8 input and bound output')


if __name__ == '__main__': main()
