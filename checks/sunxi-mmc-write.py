#!/usr/bin/env python3
"""Focused bounded-write/status/ownership model and affected embedding builds."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sources',type=pathlib.Path,default=pathlib.Path.home()/'.cache/sunstoneos/sources')
p.add_argument('--evidence',type=pathlib.Path,required=True)
p.add_argument('--read-reference',type=pathlib.Path,default=pathlib.Path.home()/'.cache/sunstoneos/evidence/mmc-read-20261016/auto-stop-argument/mmc-reference.h')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[1]
a.evidence.mkdir(parents=True,exist_ok=False)
reference_hashes={
 'mmc-write/linux-mmc-ops.c':'4e42bfe626bbad9ae01a4e32bdffe7543eadf6c23ac02c88191a614f8a62f0c4',
 'mmc-write/linux-block.c':'be0e9d3d769570f35c5c18cce56f3ca08cddad5a7de4dddc6f6504337be877d7',
 'mmc-write/micron-emmc-4.41.pdf':'4ac8173cd298a762fc5a074f1cf53aa60908d8596ceadfc92a8c4eb2fd00857e',
 'mmc-write/micron-emmc-4.41.txt':'feea93f78fe31e924179440458fab939dafa8198a119da3248d0b16139076ed7',
 'mmc-read/linux-sd.c':'2d522377ea4ea46fa01f23fdd9e5e04d7756dd51cbb2b6031aa8fe7e13796c07',
 'mmc-read/linux-mmc.c':'4bdca067049f6813b07034cd83e8d9880992a0a01a1292fe499dcd34efcaf2e2',
 'radio-sdio/include/linux/mmc/mmc.h':'9762e69cbbadb447abe1ba191ad2fc883d645c808024dfd810c12436ad4a695b',
 'u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a/drivers/mmc/mmc_write.c':'28aa725dbf663d8cebfc98d429101bf144986fe3acfca796bd4e131b9d92683f',
 'camera-research/A733_User_Manual_V1.00.pdf':'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d',
 'camera-research/A733_User_Manual_V1.00.txt':'219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2'}
for name,digest in reference_hashes.items():assert hashlib.sha256((a.sources/name).read_bytes()).hexdigest()==digest,name
manual=' '.join((a.sources/'camera-research/A733_User_Manual_V1.00.txt').read_text().split())
assert re.search(r'TRANS_DIR Transfer Direction 10 R/W 0x0 0: Read operation 1: Write operation',manual)
assert re.search(r'FIFO_FULL FIFO full 3 R 0x0 1: FIFO full 0: FIFO not full',manual)
card=' '.join((a.sources/'mmc-write/micron-emmc-4.41.txt').read_text().split())
assert re.search(r'PERM_WRITE_PROTECT 1 R/W 13',card)
assert re.search(r'TMP_WRITE_PROTECT 1 R/W/E 12',card)
assert re.search(r'WRITE_BL_LEN 4 R \[25:22\] 09h',card)
h=(a.sources/'radio-sdio/include/linux/mmc/mmc.h').read_text()
for name,value in [('MMC_WRITE_BLOCK',24),('EXT_CSD_CACHE_CTRL',33),('EXT_CSD_PART_CONFIG',179),('EXT_CSD_DATA_SECTOR_SIZE',61),('EXT_CSD_CMDQ_MODE_EN',15)]:
 assert re.search(r'#define\s+'+name+r'\s+'+str(value)+r'\b',h),name
linux=(a.sources/'mmc-write/linux-block.c').read_text()
assert 'writecmd = MMC_WRITE_BLOCK;' in linux and '!(card->csd.cmdclass & CCC_BLOCK_WRITE)' in linux
assert 'R1_WP_VIOLATION' in linux and 'cmd.arg <<= 9;' in linux
ops=(a.sources/'mmc-write/linux-mmc-ops.c').read_text()
assert '*busy = !mmc_ready_for_data(status);' in ops
(a.evidence/'source-check.json').write_text(json.dumps({'result':'pass','scope':'CMD24, FIFO direction/full, CSD protection/block length, cache/user-area/sector controls, status wait'},indent=2)+'\n')
# Extract only helpers. No original identification or capacity suite executes.
original=(root/'checks/sunxi-mmc.c').read_text()
model=original[original.index('struct model {'):original.index('static int barrier(void *ctx)')]
(a.evidence/'mmc-card-model.h').write_text(model)
assert hashlib.sha256(a.read_reference.read_bytes()).hexdigest()=='846797588f8bd2470d885027b13078762f4a966826fc71be822c0346e6bf44af'
(a.evidence/'mmc-reference.h').write_bytes(a.read_reference.read_bytes())
(a.evidence/'model-extraction.json').write_text(json.dumps({'original':hashlib.sha256(original.encode()).hexdigest(),'extracted':hashlib.sha256(model.encode()).hexdigest(),'old_suites_executed':False},indent=2)+'\n')
cc=shutil.which('clang-21') or shutil.which('clang');assert cc
common=[cc,'-std=c11','-Wall','-Wextra','-Werror','-Iinclude','-Ichecks','-I'+str(a.evidence)]
host=common+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
kernel=a.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
native=common+['--target=aarch64-none-elf','-O2','-ffreestanding','-fno-builtin','-fstack-usage','-Iruntime/include',
 '-I'+str(kernel/'include'),'-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),
 '-DINCLUDE_xSemaphoreGetMutexHolder=1','-DINCLUDE_xTaskGetSchedulerState=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1']
host_sources=['drivers/storage/sunxi_mmc.c','checks/sunxi-mmc-write.c']
compile_fixtures=['checks/sunxi-mmc.c','checks/a733-mmc-platform.c','checks/a733-mmc-sdc3.c']
native_sources=['drivers/storage/sunxi_mmc.c','drivers/storage/a733_mmc_platform.c','drivers/storage/a733_mmc_platform_native.c']
commands=[]
def run(command,name):
 command=[str(v) for v in command];commands.append(command)
 r=subprocess.run(command,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (a.evidence/name).write_text(r.stdout);(a.evidence/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 print(r.stdout,end='')
 if r.returncode:raise SystemExit(r.returncode)
for i,s in enumerate(host_sources+compile_fixtures):run(host+['-M','-MF',a.evidence/f'host-{i}.d',s],f'deps-host-{i}.log')
for i,s in enumerate(native_sources):run(native+['-M','-MF',a.evidence/f'native-{i}.d',s],f'deps-native-{i}.log')
paths={a.sources/n for n in reference_hashes}|{pathlib.Path(cc).resolve(),a.read_reference}
paths.update(root/n for n in host_sources+compile_fixtures+native_sources+['checks/sunxi-mmc-write.py'])
for dep in a.evidence.glob('*.d'):
 paths.update((root/n).resolve() for n in dep.read_text().replace('\\\n',' ').split(':',1)[1].split())
records=[];(a.evidence/'inputs').mkdir()
for path in sorted(paths):
 data=path.read_bytes();digest=hashlib.sha256(data).hexdigest();target=a.evidence/'inputs'/digest;target.write_bytes(data)
 records.append({'path':str(path),'sha256':digest,'snapshot':str(target.relative_to(a.evidence))})
(a.evidence/'inputs.json').write_text(json.dumps(records,indent=2)+'\n')
binary=a.evidence/'write-model';run(host+host_sources+['-o',binary],'build-host.log');run([binary],'host.log')
for i,s in enumerate(compile_fixtures):run(host+['-c',s,'-o',a.evidence/(pathlib.Path(s).stem+'-fixture.o')],f'fixture-{i}.log')
for i,s in enumerate(native_sources):run(native+['-c',s,'-o',a.evidence/(pathlib.Path(s).stem+'.o')],f'native-{i}.log')
assert all(hashlib.sha256(pathlib.Path(r['path']).read_bytes()).hexdigest()==r['sha256'] for r in records)
(a.evidence/'result.json').write_text(json.dumps({'result':'pass','physical':False,'old_suites_executed':False,
 'checks':['source fields','bounded writes/status/retention and one affected read under ASan/UBSan','three existing fixture compilations only','three affected AArch64 SRE object compilations'],
 'artifacts':[{'path':str(f),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in a.evidence.iterdir() if f.is_file()]},indent=2)+'\n')
