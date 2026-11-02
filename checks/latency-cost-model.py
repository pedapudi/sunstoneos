#!/usr/bin/env python3
"""Validate native cost reports preserved by the full eight-core QEMU run."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cost_host', ROOT / 'apps/latency-cost/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--on-image', type=Path, required=True)
    p.add_argument('--off-image', type=Path, required=True)
    p.add_argument('--on-log', type=Path, required=True)
    p.add_argument('--off-log', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for enabled, image, log in ((True, a.on_image, a.on_log), (False, a.off_image, a.off_log)):
        text = log.read_text()
        assert 'MODEL latency cost; emulated clocks; no board timing' in text
        assert 'PASS latency cost full notification/context-switch path on eight cores' in text
        assert 'FAIL' not in text
        words = re.findall(r'^cost_data=0x([0-9a-f]{16})$', text, re.M)
        payload = b''.join(struct.pack('<Q', int(w, 16)) for w in words)
        result = host.decode(payload)
        assert result['enabled'] == enabled
        assert set(result['timer_handler_by_tick_count']) == {'1'}
        assert all(any(n == 2 * host.ROUNDTRIPS for n in c['switches']) for c in result['cores'])
        with image.open('rb') as f:
            elf = ELFFile(f)
            symbols = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()}
            if enabled:
                for name in ('ss_latency_tick_hook', 'ss_latency_task_ready'):
                    assert symbols[name]['st_info']['bind'] == 'STB_WEAK', name
                assert 'latency_cost_inner_irq' in symbols
            else:
                assert not any(name.startswith('ss_latency_') for name in symbols)
        name = 'on' if enabled else 'off'
        (a.output / (name + '.bin')).write_bytes(payload)
        rows.append(dict(enabled=enabled, image=str(image), log=str(log),
                         image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                         log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),
                         result=result))
    (a.output / 'result.json').write_text(json.dumps(dict(passed=True,
        evidence='Host QEMU native scheduler and report coverage; OS clock/thermal APIs modeled; no board timing',
        runs=rows), indent=2) + '\n')
    print('PASS: paired native QEMU switch/IRQ paths, report decoding, weak empty workload callbacks and disabled recorder exclusion')


if __name__ == '__main__':
    main()
