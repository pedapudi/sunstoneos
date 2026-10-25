#!/usr/bin/env python3
"""Run the finite host fixtures against the interrupt host and its admission record.

The cases are those of apps/npu-finite/host-check.py, with the interrupt
record in the admission, the interrupt receipts in each returned log, and the
interrupt refusal and receipt cases. Never discovers hardware or touches queues."""
from pathlib import Path
import runpy
import sys

sys.argv[1:1]=['--interrupt']
runpy.run_path(str(Path(__file__).resolve().parents[1]/'npu-finite/host-check.py'),run_name='__main__')
