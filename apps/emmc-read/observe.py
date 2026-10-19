#!/usr/bin/env python3
"""Decode the common copied read record against the distinct eMMC session."""
import importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location('emmc_shared_decode',Path(__file__).resolve().parents[1]/'storage-read/observe.py')
shared=importlib.util.module_from_spec(spec);spec.loader.exec_module(shared)
HEADER,MAXIMUM=shared.HEADER,shared.MAXIMUM
read_request,cancel_request=shared.read_request,shared.cancel_request
def decode(raw,session,expected=None):
    return shared.decode(raw,session,expected,0x4153373345524131,912)
