#!/bin/bash
# Run a compiled single-operation graph in the SDK simulator. Inputs: float16 inputs as float text, 16-bit
# fixed-point inputs as integer codes (round(x * 2^fl)). Offline, inside the pinned image. Environment NPU_SIM_INPUT=text
# (default half) gives the simulator the float values instead of their half-precision roundings.
set -uo pipefail
dir=$(realpath "$1"); sdk=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools
cd "$dir/export_nbg_unify" || exit 2
python3 - <<'PY'
import json, os
from pathlib import Path
import numpy as np
meta = json.loads(Path('nbg_meta.json').read_text())
for index, (name, spec) in enumerate(meta['Inputs'].items()):
    stem = max((p.stem for p in Path('..').glob('*.npy') if name.startswith(p.stem + '_')), key=len)
    x = np.load('../%s.npy' % stem).astype(np.float64)
    q = spec.get('quantize')
    if q and q.get('qtype') == 'i16':
        codes = np.clip(np.rint(x * 2.0 ** q['fl']), -32768, 32767).astype(np.int64)
        Path('sim-input%d.qnt.tensor' % index).write_text('\n'.join(str(int(v)) for v in codes.ravel()) + '\n')
    else:
        # The device takes the half-precision values; give the simulator exactly those values, not its own rounding of them.
        # NPU_SIM_INPUT=text gives it the float values and lets it round them, as the shader graphs of the catalogue were made.
        if os.environ.get('NPU_SIM_INPUT', 'half') != 'text':
            x = x.astype(np.float16).astype(np.float64)
        Path('sim-input%d.tensor' % index).write_text('\n'.join(repr(float(v)) for v in x.ravel()) + '\n')
PY
export VSIMULATOR_SHADER_CORE_COUNT=1
export VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B
if [ ! -x ./f ]; then make -f makefile.linux USE_IDE_LIB=1 VIVANTE_SDK_DIR="$sdk/vsimulator" > ../simulator-build.log 2>&1 || { echo "FAILED build"; exit 1; }; fi
export LD_LIBRARY_PATH="$sdk/vsimulator/lib:$sdk/common/lib"
inputs=$(ls sim-input*.tensor | sort -V)
timeout 900 ./f network_binary.nb $inputs > ../simulator.log 2>&1 || { echo "FAILED simulate"; exit 1; }
echo SIMULATED
