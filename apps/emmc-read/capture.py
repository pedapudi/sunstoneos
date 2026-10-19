#!/usr/bin/env python3
"""Use the admitted storage capture protocol for the single-core eMMC image."""
import importlib.util
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
shared=module('emmc_shared_capture',ROOT/'tools/check-a733-storage-read-supervised.py')
shared.preparation=module('emmc_capture_prepare',ROOT/'apps/emmc-read/prepare.py')
shared.decoder=module('emmc_capture_decoder',ROOT/'apps/emmc-read/observe.py')
shared.PRODUCT='sunstoneos native eMMC read'
shared.SERIAL='sunstoneos-a733-emmc-read'
shared.SOFTWARE_PREREQUISITE='emmc-read-supervised-software'
shared.SOFTWARE_SCHEMA='a733-emmc-read-supervised-software-v2'
shared.PREREQUISITES={'supervised-ram-recovery','emmc-read-supervised-software'}
_shared_sources=shared.source_files
def source_files():
    return _shared_sources()+[('emmc_read-capture.py',Path(__file__)),
      ('common-read-preparation.py',ROOT/'apps/storage-read/prepare.py'),
      ('common-read-decoder.py',ROOT/'apps/storage-read/observe.py')]
shared.source_files=source_files
capture=shared.capture
freeze_bundle=shared.freeze_bundle
if __name__=='__main__':shared.main()
