#!/usr/bin/env python3
"""Bind the linked image to its fixed recognition inputs."""
import argparse
import hashlib
import json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for name in ('image','build-id','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2]
value={'target':'sunstone-a733-mimus-board','execution_profile':'board-supervised-v2',
       'application':'recorded-mimus-with-read-only-usb-storage-v2','usb2_combined':True,
       'keyboard_trigger':'boot-neutral-enter-v1',
       'keyboard_result_version':3,'keyboard_generation_limit':4294967295,
       'keyboard_rearm':'full-result-and-ack-tx-completion',
       'final_elf':hashlib.sha256(a.image.read_bytes()).hexdigest(),'build_id':a.build_id.read_text().strip(),
       'models':json.loads((root/'apps/bluetooth/recognition-models.json').read_text()),
       'recorded_input':json.loads((root/'apps/mimus-board/inputs.json').read_text()),
       'scope':'Offline compiled application; physical concurrent operation remains unqualified.'}
assert len(bytes.fromhex(value['build_id']))==32
a.output.write_text(json.dumps(value,indent=2)+'\n')
