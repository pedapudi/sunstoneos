#!/usr/bin/env python3
"""Run native orderly stop and retained-failure composition."""
from pathlib import Path
import runpy
import sys

# The emulator selects the focused entry point; the lifecycle suite is not executed.
sys.argv.insert(1, '--application-only')
runpy.run_path(str(Path(__file__).with_name('a733-gpu-backend-emulate.py')), run_name='__main__')
