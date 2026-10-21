#!/usr/bin/env python3
"""Build the optional interrupt-driven finite NPU application."""
from pathlib import Path
import runpy
import sys

sys.argv[1:1]=['--interrupt']
runpy.run_path(str(Path(__file__).resolve().parents[1]/'npu-finite/build.py'),run_name='__main__')
