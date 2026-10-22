#!/bin/bash
# Compile one mimus linear layer and run it in the SDK simulator. Run inside
# the pinned compiler image with the repository mounted read-only. Arguments:
# a mimus model file, a new writable output directory and a layer name
# without its ".weight" suffix. This script performs no device I/O.
set -euo pipefail
# --raw-sums builds the integer-sum form described in npu-mimus-linear.py.
if [ "$#" -ne 3 ] && ! { [ "$#" -eq 4 ] && [ "$4" = --raw-sums ]; }; then
    echo "Usage: $0 MODEL.mimus NEW_OUTPUT_DIRECTORY LAYER [--raw-sums]" >&2
    exit 2
fi
tool=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/npu-mimus-linear.py
output=$(realpath -m -- "$2")
compiler=/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py
sdk=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools

python3 "$tool" fixture "$1" "$output" --layer "$3" ${4:+"$4"}
cd -- "$output"
timeout 120 python3 "$compiler" import onnx --model linear.onnx \
    --output-model linear.json --output-data linear.data > import.log 2>&1
timeout 120 python3 "$compiler" generate inputmeta --model linear.json \
    --separated-database --input-meta-output inputmeta.yml > inputmeta.log 2>&1
python3 - <<'PY'
from pathlib import Path
import yaml
p = Path('inputmeta.yml')
meta = yaml.safe_load(p.read_text())
database = meta['input_meta']['databases'][0]
database['path'] = 'input.npy'
database['type'] = 'NPY'
port = database['ports'][0]
port['category'] = 'undefined'
port['preprocess']['reverse_channel'] = False
port['preprocess']['preproc_node_params']['preproc_type'] = 'TENSOR'
p.write_text(yaml.safe_dump(meta, sort_keys=False))
PY
# The compiler's own calibration only creates the quantize file; the next
# step replaces every entry with the mimus codes and scales.
timeout 300 python3 "$compiler" quantize --model linear.json --model-data linear.data \
    --device CPU --with-input-meta inputmeta.yml --model-quantize linear.quantize \
    --quantizer perchannel_symmetric_affine --qtype int8 > quantize.log 2>&1
python3 "$tool" quantize "$output" linear.quantize
python3 - <<'PY'
import numpy as np
codes = np.load('codes.npy')
with open('mimus-codes.qnt.tensor', 'w') as f:
    f.write('\n'.join(str(int(v)) for v in codes.ravel()) + '\n')
PY
export VSIMULATOR_SHADER_CORE_COUNT=1
export VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B
timeout 600 python3 "$compiler" export ovxlib --model linear.json --model-data linear.data \
    --model-quantize linear.quantize --dtype quantized --with-input-meta inputmeta.yml \
    --target-ide-project linux64 --pack-nbg-unify --optimize "$VSIMULATOR_CONFIG" \
    --viv-sdk "$sdk" --output-path export/linear > export.log 2>&1
cd export_nbg_unify
timeout 120 make -f makefile.linux USE_IDE_LIB=1 \
    VIVANTE_SDK_DIR="$sdk/vsimulator" > ../simulator-build.log 2>&1
export LD_LIBRARY_PATH="$sdk/vsimulator/lib:$sdk/common/lib"
timeout 600 ./linear network_binary.nb ../mimus-codes.qnt.tensor > ../simulator.log 2>&1
cd ..
python3 "$tool" compare "$output" comparison.json
