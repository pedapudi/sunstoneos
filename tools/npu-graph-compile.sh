#!/bin/bash
# Compile one graph directory from tools/npu-graph-variants.py to a network binary with the pinned compiler, quantizing
# every tensor to 16-bit dynamic fixed point. Run inside the pinned compiler image, offline:
#
#   docker run --rm --network none --memory 12g --memory-swap 12g -v WORK:/work -v $PWD/tools:/tools:ro IMAGE \
#       bash /tools/npu-graph-compile.sh /work/frags/NAME
#
# Each stage logs into the graph directory; a failing stage ends the script with its name on the last line.
set -uo pipefail
dir=$(realpath "$1"); compiler=/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py
sdk=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools
cd "$dir" || exit 2
timeout 300 python3 "$compiler" import onnx --model fragment.onnx --output-model f.json --output-data f.data > import.log 2>&1 || { echo "FAILED import"; exit 1; }
timeout 120 python3 "$compiler" generate inputmeta --model f.json --separated-database --input-meta-output inputmeta.yml > inputmeta.log 2>&1 || { echo "FAILED inputmeta"; exit 1; }
python3 - <<'PY'
from pathlib import Path
import json, yaml
names = json.loads(Path('inputs.json').read_text())
p = Path('inputmeta.yml')
meta = yaml.safe_load(p.read_text())
for database in meta['input_meta']['databases']:
    port = database['ports'][0]
    name = max((n for n in names if port['lid'].startswith(n + '_')), key=len)
    database['path'] = name + '.npy'
    database['type'] = 'NPY'
    port['category'] = 'undefined'
    port['preprocess']['reverse_channel'] = False
    port['preprocess']['preproc_node_params']['preproc_type'] = 'TENSOR'
p.write_text(yaml.safe_dump(meta, sort_keys=False))
PY
timeout 900 python3 "$compiler" quantize --model f.json --model-data f.data --device CPU --with-input-meta inputmeta.yml \
    --model-quantize f.quantize --quantizer dynamic_fixed_point --qtype int16 > quantize.log 2>&1 || { echo "FAILED quantize"; exit 1; }
export VSIMULATOR_SHADER_CORE_COUNT=1 VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B
timeout 1800 python3 "$compiler" export ovxlib --model f.json --model-data f.data --model-quantize f.quantize \
    --dtype quantized --with-input-meta inputmeta.yml --target-ide-project linux64 --pack-nbg-unify \
    --optimize "$VSIMULATOR_CONFIG" --viv-sdk "$sdk" --output-path export/f > export.log 2>&1 || { echo "FAILED export"; exit 1; }
ls export_nbg_unify/network_binary.nb > /dev/null 2>&1 || { echo "FAILED no-nbg"; exit 1; }
echo OK
