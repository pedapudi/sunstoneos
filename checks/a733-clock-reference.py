#!/usr/bin/env python3
"""Check changed clock admissions for the mandatory native SD/PMIC/USB path."""
import argparse,hashlib,json,subprocess
from pathlib import Path
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
ROOT=Path(__file__).resolve().parents[1];CACHE=Path.home()/'.cache/sunstoneos'
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--providers',nargs='+',choices=['sd','pmic','usb'],default=['sd','pmic','usb']);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
def ref(p):return dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
for name,output,boundary in [('checks/a733-mmc-platform.c','mmc-platform-fixture.h','int main('),('checks/a733-usb-platform.c','usb-platform-fixture.h','int main('),('checks/a733-pmic-parent.c','pmic-parent-fixture.h','int main(')]:
 raw=(ROOT/name).read_text();(a.output/output).write_text(raw[:raw.index(boundary)])
raw=(ROOT/'checks/sunxi-mmc.c').read_text();(a.output/'mmc-card-model.h').write_text(raw[raw.index('struct model {'):raw.index('static int barrier(void *ctx)')])
common=['drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c','drivers/gpu/a733_gpu_resources.c','drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c']
plans={'sd':common+['drivers/storage/a733_storage_update.c','drivers/storage/storage_digest.c','drivers/storage/a733_mmc_platform.c','drivers/storage/sunxi_mmc.c','checks/a733-clock-reference-sd.c'],'pmic':common+['checks/a733-clock-reference-pmic.c'],'usb':['drivers/clock/a733_clock_leases.c','drivers/usb/a733_usb_platform.c','checks/a733-clock-reference-usb.c']}
results=[]
for name in a.providers:
 sources=plans[name]
 flags=['/usr/bin/clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Iinclude','-Ichecks','-I'+str(a.output)]
 command=flags+sources+['-o',str(a.output/name)]
 for phase,cmd in [('build',command),('run',[str(a.output/name)])]:
  result=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True);(a.output/(name+'-'+phase+'.log')).write_text(result.stdout+result.stderr);assert result.returncode==0,(name,phase,result.stdout,result.stderr)
 results.append(dict(provider=name,passed=True,command=command,inputs=[ref(ROOT/x) for x in sources]));(a.output/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
(a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,providers=results,header=ref(ROOT/'include/a733_clock_reference.h'),pmic_layout=ref(ROOT/'include/a733_pmic.h'),scope='Only changed DCXO/REF/PERI0 and mandatory-consumer admission/retention cases; actual providers with modeled registers/time/card; no prior main or suites executed.'),indent=2)+'\n')
for name in a.providers:print((a.output/(name+'-run.log')).read_text(),end='')
