#!/usr/bin/env python3
"""Run the isolated provider-result check with the existing native MMIO model."""
from pathlib import Path

# Reuse its instruction/translation model unchanged. Only select the focused
# entry; the older lifecycle suite is not executed.
p = Path(__file__).with_name('a733-gpu-backend-emulate.py')
source = p.read_text()
selector = "symbols['backend_clock_check' if a.clock_only else 'backend_event_check' if a.event_only else 'backend_check']"
assert source.count(selector) == 1
source = source.replace(selector, "symbols['backend_isolation_check']")
source = source[:source.index("print('PASS native GPU backend:")] + \
    "print('PASS native isolation boundary: -1 and 2 retain memory after partial teardown; only 1 authorizes reclaim')\n"
exec(compile(source, str(p), 'exec'))
