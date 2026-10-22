#!/bin/bash
# Compile and simulate the two-input, two-output fixture. Run inside the pinned
# compiler image with the repository mounted read-only. The destination must be
# new and writable. This script performs no device I/O.
set -euo pipefail
# --perchannel-int8 selects INT8 tensors with per-channel symmetric weights.
if [ "$#" -ne 1 ] && ! { [ "$#" -eq 2 ] && [ "$2" = --perchannel-int8 ]; }; then
    echo "Usage: $0 NEW_OUTPUT_DIRECTORY [--perchannel-int8]" >&2
    exit 2
fi
quantizer=asymmetric_affine
qtype=uint8
if [ "$#" -eq 2 ]; then
    quantizer=perchannel_symmetric_affine
    qtype=int8
fi
fixture_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
fixture_output=$(realpath -m -- "$1")
fixture_compiler=/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py
fixture_sdk=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools

python3 "$fixture_source/tools/npu-dual-fixture.py" "$fixture_output"
cd -- "$fixture_output"
timeout 120 python3 "$fixture_compiler" import onnx --model dual.onnx \
    --output-model dual.json --output-data dual.data > import.log 2>&1
timeout 120 python3 "$fixture_compiler" generate inputmeta --model dual.json \
    --separated-database --input-meta-output inputmeta.yml > inputmeta.log 2>&1
python3 - <<'PY'
from pathlib import Path
import yaml
p = Path('inputmeta.yml')
meta = yaml.safe_load(p.read_text())
for index, database in enumerate(meta['input_meta']['databases']):
    database['path'] = f'input{index}.npy'
    database['type'] = 'NPY'
    port = database['ports'][0]
    assert port['lid'].startswith(f'input{index}_')
    port['category'] = 'undefined'
    port['preprocess']['reverse_channel'] = False
    port['preprocess']['preproc_node_params']['preproc_type'] = 'TENSOR'
p.write_text(yaml.safe_dump(meta, sort_keys=False))
PY
timeout 120 python3 "$fixture_compiler" inference --model dual.json \
    --model-data dual.data --device CPU --with-input-meta inputmeta.yml \
    --dtype float32 --iterations 1 --postprocess dump_results \
    --output-dir float-reference > float-reference.log 2>&1
timeout 120 python3 "$fixture_compiler" quantize --model dual.json \
    --model-data dual.data --device CPU --with-input-meta inputmeta.yml \
    --model-quantize dual.quantize --quantizer "$quantizer" \
    --qtype "$qtype" > quantize.log 2>&1
timeout 120 python3 "$fixture_compiler" inference --model dual.json \
    --model-data dual.data --model-quantize dual.quantize \
    --device CPU --with-input-meta inputmeta.yml --dtype quantized --iterations 1 \
    --postprocess dump_results --output-dir quantized-reference > quantized-reference.log 2>&1
export VSIMULATOR_SHADER_CORE_COUNT=1
export VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B
timeout 300 python3 "$fixture_compiler" export ovxlib --model dual.json \
    --model-data dual.data --model-quantize dual.quantize \
    --dtype quantized --with-input-meta inputmeta.yml --target-ide-project linux64 \
    --pack-nbg-unify --optimize "$VSIMULATOR_CONFIG" --viv-sdk "$fixture_sdk" \
    --output-path export/dual > export.log 2>&1
cd export_nbg_unify
timeout 120 make -f makefile.linux USE_IDE_LIB=1 \
    VIVANTE_SDK_DIR="$fixture_sdk/vsimulator" > ../simulator-build.log 2>&1
export LD_LIBRARY_PATH="$fixture_sdk/vsimulator/lib:$fixture_sdk/common/lib"
input0=$(ls ../quantized-reference/iter_0_input0_*.qnt.tensor)
input1=$(ls ../quantized-reference/iter_0_input1_*.qnt.tensor)
timeout 120 ./dual network_binary.nb "$input0" "$input1" > ../simulator.log 2>&1
cd ..
python3 "$fixture_source/checks/npu-dual-reference.py" . reference-check.json
