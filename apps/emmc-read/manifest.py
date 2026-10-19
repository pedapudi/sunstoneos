#!/usr/bin/env python3
"""Freeze actual compiled eMMC sources, objects, headers and linker inputs."""
import argparse,importlib.util
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('emmc_shared_manifest',ROOT/'apps/storage-read/manifest.py')
shared=importlib.util.module_from_spec(spec);spec.loader.exec_module(shared)
def freeze(build,out):
    return shared.freeze(build,out,'sunstone-a733-emmc-read','emmc-read-single-core-supervised-v2',
      'emmc-read','a733-sd-single-core-usb2-host',
      [ROOT/'cmake/a733-storage-read-single-core.cmake',ROOT/'cmake/a733-emmc-read.cmake',
       ROOT/'apps/emmc-read/CMakeLists.txt',ROOT/'apps/storage-read-single-core/link.ld'])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();freeze(a.build,a.output)
