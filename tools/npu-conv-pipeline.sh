#!/bin/bash
# Run inside the pinned compiler image with the repository mounted read-only.
# The destination must be new and writable. This script performs no device I/O.
set -euo pipefail
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
fixture_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
fixture_output=$(realpath -m -- "$1")
fixture_compiler=/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py
fixture_sdk=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools

python3 "$fixture_source/tools/npu-conv-fixture.py" "$fixture_output"
cd -- "$fixture_output"
timeout 120 python3 "$fixture_compiler" import onnx --model conv3x3.onnx \
    --output-model conv3x3.json --output-data conv3x3.data > import.log 2>&1
cp conv3x3.data imported.data
timeout 120 python3 "$fixture_compiler" generate inputmeta --model conv3x3.json \
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
timeout 120 python3 "$fixture_compiler" inference --model conv3x3.json \
    --model-data conv3x3.data --device CPU --with-input-meta inputmeta.yml \
    --dtype float32 --iterations 1 --postprocess dump_results \
    --output-dir float-reference > float-reference.log 2>&1
timeout 120 python3 "$fixture_compiler" quantize --model conv3x3.json \
    --model-data conv3x3.data --device CPU --with-input-meta inputmeta.yml \
    --model-quantize conv3x3_uint8.quantize --quantizer asymmetric_affine \
    --qtype uint8 > quantize.log 2>&1
timeout 120 python3 "$fixture_compiler" inference --model conv3x3.json \
    --model-data conv3x3.data --model-quantize conv3x3_uint8.quantize \
    --device CPU --with-input-meta inputmeta.yml --dtype quantized --iterations 1 \
    --postprocess dump_results --output-dir quantized-reference > quantized-reference.log 2>&1
export VSIMULATOR_SHADER_CORE_COUNT=1
export VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B
timeout 300 python3 "$fixture_compiler" export ovxlib --model conv3x3.json \
    --model-data conv3x3.data --model-quantize conv3x3_uint8.quantize \
    --dtype quantized --with-input-meta inputmeta.yml --target-ide-project linux64 \
    --pack-nbg-unify --optimize "$VSIMULATOR_CONFIG" --viv-sdk "$fixture_sdk" \
    --output-path export/conv3x3 > export.log 2>&1
cd export_nbg_unify
timeout 120 make -f makefile.linux USE_IDE_LIB=1 \
    VIVANTE_SDK_DIR="$fixture_sdk/vsimulator" > ../simulator-build.log 2>&1
export LD_LIBRARY_PATH="$fixture_sdk/vsimulator/lib:$fixture_sdk/common/lib"
timeout 120 ./conv3x3 network_binary.nb \
    ../quantized-reference/iter_0_input_2_out0_1_3_8_8.qnt.tensor > ../simulator.log 2>&1
cd ..
python3 "$fixture_source/checks/npu-conv-reference.py" . reference-check.json
