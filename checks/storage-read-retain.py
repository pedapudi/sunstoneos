#!/usr/bin/env python3
"""Build and execute only the additive MMC retain API against existing real managers."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def run(out):
 out.mkdir(parents=True,exist_ok=False);original=(ROOT/'checks/sunxi-mmc.c').read_text();model=original[original.index('struct model {'):original.index('static int barrier(void *ctx)')];(out/'mmc-card-model.h').write_text(model)
 composition=(ROOT/'checks/a733-mmc-platform.c').read_text();(out/'storage-platform-model.h').write_text(composition[:composition.index('int main(')])
 sources=['drivers/storage/a733_storage_update.c','drivers/storage/storage_digest.c','drivers/storage/a733_mmc_platform.c','drivers/storage/sunxi_mmc.c','drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c','drivers/gpu/a733_gpu_resources.c','drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','checks/storage-read-retain.c']
 command=['/usr/lib/llvm-21/bin/clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-Iinclude','-Ichecks','-I'+str(out),*sources,'-o',str(out/'retain')]
 records=[]
 for c in (command,[str(out/'retain')]):
  p=subprocess.run(c,cwd=ROOT,capture_output=True,text=True);records.append(dict(command=c,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr));(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n');assert p.returncode==0,records[-1]
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,old_suites_replayed=False,source_hashes={s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in sources},model_source_sha256=hashlib.sha256(original.encode()).hexdigest(),model_sha256=hashlib.sha256(model.encode()).hexdigest(),scope='New metadata retain owner/busy/update rejection/deadline/no-MMIO cases; existing controller tests not run.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);run(p.parse_args().output)
