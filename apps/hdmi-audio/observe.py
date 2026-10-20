#!/usr/bin/env python3
"""Archive and decode supplied HDMI audio RAM observations; never opens a device."""
import argparse
import importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location('hdmi_audio_offline_capture',Path(__file__).with_name('capture.py'))
capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','pcm','result','ready','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    capture.capture(*(getattr(args,name).read_bytes() for name in ('image','session','pcm','result','ready')),args.output)


if __name__=='__main__':main()
