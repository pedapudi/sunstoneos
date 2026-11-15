#!/usr/bin/env python3
"""Build and run the CE clock bring-up against a register model that enforces the native write rules (checks/ce-clock.c)."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
# The source lines of the bring-up steps the cases fail at, so a case names the exact step.
source = (root / 'drivers/crypto/a733_ce_clock.c').read_text().splitlines()
def line(text):
    found = [i + 1 for i, s in enumerate(source) if s.strip() == text]
    assert len(found) == 1, (text, found)
    return found[0]
lines = {'CE_LINE_MBUS_UP': line('if(!bits(p,CCU+0x5e4,0x40306,neighbors|4))return failed(p);'),
         'CE_LINE_CYCLE_MBUS': line('if(!update(p,CCU+0x5e4,4,0))return failed(p);'),
         'CE_LINE_BGR_RESETS': line('if(!update(p,CCU+0xac4,0x30000,0x30000))return failed(p);')}
with tempfile.TemporaryDirectory(prefix='ce-clock-') as directory:
    binary = pathlib.Path(directory) / 'ce-clock'
    build = subprocess.run(['/usr/bin/clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(root / 'include'),
                            *('-D%s=%d' % item for item in lines.items()),
                            str(root / 'checks/ce-clock.c'), str(root / 'drivers/crypto/a733_ce_clock.c'),
                            str(root / 'drivers/clock/a733_clock_leases.c'), '-o', str(binary)],
                           capture_output=True, text=True)
    if build.returncode:
        sys.exit(build.stdout + build.stderr)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(run.stdout + run.stderr)
    if run.returncode or 'PASS' not in run.stdout:
        sys.exit(1)
# The decoder names the step and the CE words of that record (group CE of include/sunstone_crumb.h).
spec = importlib.util.spec_from_file_location('crumbs', root / 'tools/a733-crumbs.py')
crumbs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(crumbs)
defines, _ = crumbs.header()
site, value, caller = (int(w, 16) for w in run.stdout.split('RECORD ')[1].split()[:3])
record = crumbs.board_failure({'fault': 0xfa0b0400, 'esr': site, 'elr': caller, 'far': value}, defines, None)
assert record['group'] == 'ce' and record['site'] == lines['CE_LINE_MBUS_UP'], record
assert (record['mbus_entry'], record['mbus_last']) == ('0x4', '0x104'), record
assert (record['ce_clk_last'], record['ce_bgr_last'], record['write_refused']) == ('0x81000000', '0x30003', False), record
assert 'caller_source' not in record, record
words = dict.fromkeys(crumbs.WORDS, 0)
words.update(tag=0x5c0d0001, fault=0xfa0b0400, esr=site, elr=caller, far=value)
assert 'CE clock bring-up at a733_ce_clock.c line %d; CCU 0x5e4 0x4 at entry, 0x104 last read' % lines['CE_LINE_MBUS_UP'] in crumbs.decode(words, 1)['summary']
print('PASS CE first failure record: the decoder names the a733_ce_clock.c line and the CE words')
