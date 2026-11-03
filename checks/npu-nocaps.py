#!/usr/bin/env python3
"""The NPU loader, page-table builder, service and queue have no fixed count of regions, patches, tensors, clients or handles.

Builds checks/npu-nocaps.c on the host shim (no device): 3000 page-table regions, 5000 relocation patches, a graph of 12
inputs and 10 outputs (made from the add graph by checks/nbg_wide.py), the service's tables for 400 clients and the queue
with 400 handles of 22 tensors, and the refusals that stay (the 4 GiB range, a table the pool cannot hold, a tensor outside
its mapping, an edge between buffers that are not one allocation).
"""
import argparse, json, subprocess, sys
from pathlib import Path

import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
CHECK_INPUTS = ('checks/npu-nocaps.c', 'checks/nbg_wide.py', 'drivers/npu/npu_service.c', 'drivers/npu/nbg_assembly.c',
                'drivers/npu/a733_npu_mmu.c', 'drivers/npu/nbg_relocation.c', 'runtime/npu_app_queue.c')
sys.path.insert(0, str(ROOT / 'checks'))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--graphs', type=Path, default=CACHE / 'build/npu-graph-variants')
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
import nbg_wide
artifacts = json.loads((args.graphs / 'artifacts.json').read_text())
wide = out / 'wide.nb'
wide.write_bytes(nbg_wide.widen(Path(artifacts['graph-add']['model']).read_bytes(), 12, 10))
units = ('checks/npu-nocaps.c', 'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_controller.c', 'drivers/npu/a733_npu_mmu.c',
         'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c',
         'drivers/storage/storage_digest.c', 'runtime/npu_app_queue.c')
exe = host_rtos.build(out / 'exe', units, 'npu-nocaps', defines=['A733_A76_ADMITTED=1'],
                      flags=['-Wno-error', '-w', '-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
done = subprocess.run([str(exe), str(wide)], capture_output=True, text=True, env=environment(), timeout=600)
(out / 'run.log').write_text(done.stdout + done.stderr)
print(done.stdout, end='')
assert done.returncode == 0, done.stderr[-1500:]
