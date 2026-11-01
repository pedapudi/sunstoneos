#!/usr/bin/env python3
"""Verify an opaque captured result before a caller-owned return continuation."""
import hashlib
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('followup_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def returned_result(record, image, raw, epoch, generation):
    host.require(record.get('complete') is True and record.get('dram_restored') is True and
                 record.get('transport_failed') is False, 'Successful restored return receipt required.')
    host.require(record.get('output_sha256') == hashlib.sha256(raw).hexdigest(), 'Copied result differs from receipt.')
    info = host.image_manifest(image)
    return host.decode_result(raw, epoch, generation, info['manifest']['output_limit'])
