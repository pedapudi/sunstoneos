#!/usr/bin/env python3
"""Check adaptive PF voltage with actual storage providers and modeled MMIO."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];CACHE=Path.home()/'.cache/sunstoneos'
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
def ref(p):return dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
source=CACHE/'sources/camera-research/A733_User_Manual_V1.00.txt';assert ref(source)['sha256']=='219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'
raw=(ROOT/'checks/sunxi-mmc.c').read_text();(a.output/'mmc-card-model.h').write_text(raw[raw.index('struct model {'):raw.index('static int barrier(void *ctx)')])
platform=(ROOT/'checks/a733-mmc-platform.c').read_text();(a.output/'mmc-platform-fixture.h').write_text(platform[:platform.index('int main(')])
sources=['drivers/storage/a733_storage_update.c','drivers/storage/storage_digest.c','drivers/storage/a733_mmc_platform.c','drivers/storage/sunxi_mmc.c','drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c','drivers/gpu/a733_gpu_resources.c','drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','checks/a733-mmc-adaptive-voltage.c']
command=['/usr/bin/clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-Iinclude','-Ichecks','-I'+str(a.output),*sources,'-o',str(a.output/'check')]
for name,cmd in [('build',command),('run',[str(a.output/'check')])]:
 result=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True);(a.output/(name+'.log')).write_text(result.stdout+result.stderr)
 assert result.returncode==0,(name,result.stdout,result.stderr)
(a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=4,source=ref(source),inputs=[ref(ROOT/x) for x in sources],command=command,scope='Changed adaptive voltage admission and loss retention only; actual shared pin/clock/rail/PMIC/MMC implementation, modeled card/MMIO/time; previous test main never executed.'),indent=2)+'\n')
print((a.output/'run.log').read_text(),end='')
