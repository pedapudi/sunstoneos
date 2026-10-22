#!/bin/bash
# Compile one mimus linear layer and run it in the SDK simulator. Run inside
# the pinned compiler image with the repository mounted read-only. Arguments:
# a mimus model file, a new writable output directory, a layer name without
# its ".weight" suffix, then options. --digits and --range select the digit
# form described in npu-mimus-linear.py and --unit its unit-scale probe base;
# --output-qtype, --output-step and --output-zero set the output quantization
# of the INT16 and unit forms. This script performs no device I/O.
set -euo pipefail
if [ "$#" -lt 3 ]; then
    echo "Usage: $0 MODEL.mimus NEW_OUTPUT_DIRECTORY LAYER [--digits [--range] | --unit]" \
         "[--output-qtype i16|i8|u8] [--output-step STEP] [--output-zero ZERO]" >&2
    exit 2
fi
tool=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/npu-mimus-linear.py
model=$1
output=$(realpath -m -- "$2")
layer=$3
shift 3
fixture_options=()
quantize_options=()
while [ "$#" -gt 0 ]; do
    case $1 in
        --digits|--range|--unit) fixture_options+=("$1"); shift ;;
        --output-qtype|--output-step|--output-zero) quantize_options+=("$1" "$2"); shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
compiler=/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py
sdk=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools

python3 "$tool" fixture "$model" "$output" --layer "$layer" ${fixture_options[@]+"${fixture_options[@]}"}
cd -- "$output"
timeout 120 python3 "$compiler" import onnx --model linear.onnx \
    --output-model linear.json --output-data linear.data > import.log 2>&1
timeout 120 python3 "$compiler" generate inputmeta --model linear.json \
    --separated-database --input-meta-output inputmeta.yml > inputmeta.log 2>&1
# Each input reads its own NumPy file: input.npy, or input0.npy, input1.npy
# and so on in the digit form, whose ports the compiler names inputC_*.
python3 - <<'PY'
from pathlib import Path
import yaml
p = Path('inputmeta.yml')
meta = yaml.safe_load(p.read_text())
databases = meta['input_meta']['databases']
for index, database in enumerate(databases):
    port = database['ports'][0]
    if len(databases) == 1 and Path('input.npy').exists():
        database['path'] = 'input.npy'
    else:
        assert port['lid'].startswith(f'input{index}_'), port['lid']
        database['path'] = f'input{index}.npy'
    database['type'] = 'NPY'
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
python3 "$tool" quantize "$output" linear.quantize ${quantize_options[@]+"${quantize_options[@]}"}
# One text tensor per input in model order: the mimus codes, or the digits.
python3 - <<'PY'
from pathlib import Path
import numpy as np
names = ['codes.npy'] if Path('codes.npy').exists() else \
    ['digits%d.npy' % i for i in range(len(list(Path('.').glob('digits*.npy'))))]
for index, name in enumerate(names):
    with open('mimus-input%d.qnt.tensor' % index, 'w') as f:
        f.write('\n'.join(str(int(v)) for v in np.load(name).ravel()) + '\n')
PY
export VSIMULATOR_SHADER_CORE_COUNT=1
export VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B
# The compiler's architecture model writes one "CycleCount:" estimate per
# operation into export.log; it does not change the emitted graph.
export NN_EXT_SHOW_PERF=1
timeout 600 python3 "$compiler" export ovxlib --model linear.json --model-data linear.data \
    --model-quantize linear.quantize --dtype quantized --with-input-meta inputmeta.yml \
    --target-ide-project linux64 --pack-nbg-unify --optimize "$VSIMULATOR_CONFIG" \
    --viv-sdk "$sdk" --output-path export/linear > export.log 2>&1
cd export_nbg_unify
timeout 120 make -f makefile.linux USE_IDE_LIB=1 \
    VIVANTE_SDK_DIR="$sdk/vsimulator" > ../simulator-build.log 2>&1
export LD_LIBRARY_PATH="$sdk/vsimulator/lib:$sdk/common/lib"
inputs=$(ls ../mimus-input*.qnt.tensor | sort -V)
# shellcheck disable=SC2086
timeout 600 ./linear network_binary.nb $inputs > ../simulator.log 2>&1
cd ..
python3 "$tool" compare "$output" comparison.json
