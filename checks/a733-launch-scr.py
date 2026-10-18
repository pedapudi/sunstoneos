#!/usr/bin/env python3
"""One changed launcher case: no SCR probe; unchanged remaining admission gates."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for name in ('bundle','baseline','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();old=a.baseline.read_text();new=(ROOT/'platform/a733-smp-observe/launch.S').read_text()
removed='    mrc p15, 0, r0, c1, c1, 0\n    tst r0, #1\n    bne a733_smp_launch_park\n'
assert old.count(removed)==1 and new==old.replace(removed,''),'Unreviewed change to another entry gate'
source=(ROOT/'checks/a733-smp-supervised-launch.py').read_text()
start=source.index('    if args.frequency_only:');end=source.index('    result =',start)
derived=source[:start]+"    cases=[check('SCR-free admission publishes contract and reaches RMR',12)]\n"+source[end:]
needle='            if word & 0xffff0fff in system:'
assert derived.count(needle)==1
derived=derived.replace(needle,"            assert word & 0xffff0fff != 0xee110f11, 'Forbidden SCR read from unproven ROM SVC context'\n"+needle)
script=a.output.parent/'scr-free-instruction-case.py';script.write_text(derived)
result=subprocess.run([sys.executable,str(script),'--bundle',str(a.bundle),'--output',str(a.output)],capture_output=True,text=True)
(a.output.parent/'instruction-output.log').write_text(result.stdout+result.stderr)
assert result.returncode==0,result.stderr
record=json.loads(a.output.read_text());record.update({'baseline_source_sha256':hashlib.sha256(old.encode()).hexdigest(),
 'entry_source_sha256':hashlib.sha256(new.encode()).hexdigest(),'case_runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
 'scope':'Exactly three SCR read/test/park instructions removed; every other source gate unchanged. One actual publication/RMR instruction case, with SCR access forbidden. Prior gate/frequency/refusal cases reused.'})
a.output.write_text(json.dumps(record,indent=2)+'\n');print('PASS one SCR-free launcher case; other entry gates source-identical.')
