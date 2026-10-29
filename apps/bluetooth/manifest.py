#!/usr/bin/env python3
"""Record the actual linked Bluetooth ELF identity for offline preparation."""
import argparse
import hashlib
import json
from pathlib import Path

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['image', 'build-id', 'output']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--security-inputs',type=Path)
    p.add_argument('--recognition-inputs',type=Path)
    p.add_argument('--a2dp-inputs',type=Path)
    p.add_argument('--msbc-inputs',type=Path)
    a = p.parse_args()
    identity = a.build_id.read_text().strip()
    assert len(bytes.fromhex(identity)) == 32
    a.output.write_text(json.dumps({'target': 'sunstone-a733-bluetooth',
        'execution_profile': 'bluetooth-supervised-v1', 'final_elf': hashlib.sha256(a.image.read_bytes()).hexdigest(),
        'build_id': identity,
        'a2dp_provider': json.loads(a.a2dp_inputs.read_text()) if a.a2dp_inputs else None,
        'wideband_provider': json.loads(a.msbc_inputs.read_text()) if a.msbc_inputs else None,
        'recognition_provider': json.loads(a.recognition_inputs.read_text()) if a.recognition_inputs else None,
        'headset_provider': json.loads(a.security_inputs.read_text()) if a.security_inputs else None, 'scope': 'Actual native ELF linkage; no physical SID, radio, UART or HCI qualification.'}, indent=2) + '\n')
