#!/usr/bin/env python3
"""Run native orderly stop and retained-failure composition."""
from pathlib import Path
p = Path(__file__).with_name('a733-gpu-backend-emulate.py')
source = p.read_text()
selector = "symbols['backend_clock_check' if a.clock_only else 'backend_event_check' if a.event_only else 'backend_check']"
assert source.count(selector) == 1
source = source.replace(selector, "symbols['gpu_application_isolation_check']")
source = source[:source.index("print('PASS native GPU backend:")] + \
    "print('PASS native GPU application: orderly asynchronous power-off and firmware-state checks; failures retain memory and power')\n"
exec(compile(source, str(p), 'exec'))
