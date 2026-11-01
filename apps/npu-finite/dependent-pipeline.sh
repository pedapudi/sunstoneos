#!/bin/bash
# Run in the pinned compiler container with read-only source and producer inputs.
set -euo pipefail
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 PRODUCER_DIRECTORY NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
chain_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
chain_output=$2
chain_compiler=/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py
chain_sdk=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools
python3 "$chain_source/dependent-fixture.py" "$1" "$chain_output"
cd -- "$chain_output"
timeout 120 python3 "$chain_compiler" import onnx --model conv3x3.onnx --output-model conv3x3.json --output-data conv3x3.data > import.log 2>&1
timeout 120 python3 "$chain_compiler" generate inputmeta --model conv3x3.json --separated-database --input-meta-output inputmeta.yml > inputmeta.log 2>&1
python3 - <<'PY'
from pathlib import Path
import yaml
p=Path('inputmeta.yml'); meta=yaml.safe_load(p.read_text())
d=meta['input_meta']['databases'][0];d['path']='input.npy';d['type']='NPY'
port=d['ports'][0];port['category']='undefined';port['preprocess']['reverse_channel']=False
port['preprocess']['preproc_node_params']['preproc_type']='TENSOR'
p.write_text(yaml.safe_dump(meta,sort_keys=False))
PY
timeout 120 python3 "$chain_compiler" quantize --model conv3x3.json --model-data conv3x3.data --device CPU --with-input-meta inputmeta.yml --model-quantize conv3x3_uint8.quantize --quantizer asymmetric_affine --qtype uint8 > quantize.log 2>&1
python3 - <<'PY'
from pathlib import Path
import json,yaml
p=Path('conv3x3_uint8.quantize');q=yaml.safe_load(p.read_text())
source=json.loads(Path('producer-quantization.json').read_text())
inputs=[v for k,v in q['quantize_parameters'].items() if k.startswith('@input_') and k.endswith(':out0')]
assert len(inputs)==1
inputs[0].update(source)
p.write_text(yaml.safe_dump(q,sort_keys=False))
PY
timeout 120 python3 "$chain_compiler" inference --model conv3x3.json --model-data conv3x3.data --model-quantize conv3x3_uint8.quantize --device CPU --with-input-meta inputmeta.yml --dtype quantized --iterations 1 --postprocess dump_results --output-dir quantized-reference > quantized-reference.log 2>&1
export VSIMULATOR_SHADER_CORE_COUNT=1
export VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B
timeout 300 python3 "$chain_compiler" export ovxlib --model conv3x3.json --model-data conv3x3.data --model-quantize conv3x3_uint8.quantize --dtype quantized --with-input-meta inputmeta.yml --target-ide-project linux64 --pack-nbg-unify --optimize "$VSIMULATOR_CONFIG" --viv-sdk "$chain_sdk" --output-path export/dependent_conv > export.log 2>&1
cd export_nbg_unify
timeout 120 make -f makefile.linux USE_IDE_LIB=1 VIVANTE_SDK_DIR="$chain_sdk/vsimulator" > ../simulator-build.log 2>&1
export LD_LIBRARY_PATH="$chain_sdk/vsimulator/lib:$chain_sdk/common/lib"
timeout 120 ./dependentconv network_binary.nb ../quantized-reference/iter_0_input_*.qnt.tensor > ../simulator.log 2>&1
