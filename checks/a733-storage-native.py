#!/usr/bin/env python3
"""Additive native storage guards and supply profiles; no physical operations."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--watchdog-include',type=Path,required=True)
    p.add_argument('--only',choices=['all','usb-native'],default='all',help='Resume pending USB/compile work without replaying passed PMIC/MMC cases.')
    p.add_argument('--sources',type=Path,default=Path.home()/'.cache/sunstoneos/sources')
    a=p.parse_args();root=Path(__file__).resolve().parents[1];out=a.evidence.resolve()
    out.mkdir(parents=True,exist_ok=False)
    transforms=[]
    def generated(name,source,changes):
        original=(root/source).read_text();text=original
        for before,after,count in changes:
            assert text.count(before)==count,(source,before,text.count(before),count)
            text=text.replace(before,after)
        (out/name).write_text(text)
        transforms.append(dict(source=source,output=name,original_sha256=hashlib.sha256(original.encode()).hexdigest(),
            transformed_sha256=hashlib.sha256(text.encode()).hexdigest(),changes=changes))
    original=(root/'checks/sunxi-mmc.c').read_text()
    model=original[original.index('struct model {'):original.index('static int barrier(void *ctx)')]
    (out/'mmc-card-model.h').write_text(model)
    transforms.append(dict(source='checks/sunxi-mmc.c',output='mmc-card-model.h',
        original_sha256=hashlib.sha256(original.encode()).hexdigest(),transformed_sha256=hashlib.sha256(model.encode()).hexdigest(),
        extraction='struct model through the byte before static int barrier; original main is excluded'))
    generated('storage-provider-model.h','checks/a733-mmc-platform.c',[
        ('storage_leave,storage_retain,storage_read,storage_write,storage_barrier,storage_outside',
         'storage_leave,storage_retain,update_read,update_write,storage_barrier,storage_outside',1)])
    original=(root/'checks/a733-storage-update.c').read_text();marker='int main(int argc,char **argv)'
    assert original.count(marker)==1
    model=original[:original.index(marker)]
    for text in ['if(address==s->platform.base+0x200) {','if(address==s->platform.base+0x18&&(value&63)==24) {']:
        assert model.count(text)==1;model=model.replace(text,text+'\n        native_boundary(address,value);')
    (out/'storage-native-service-model.h').write_text(model)
    transforms.append(dict(source='checks/a733-storage-update.c',output='storage-native-service-model.h',
        original_sha256=hashlib.sha256(original.encode()).hexdigest(),transformed_sha256=hashlib.sha256(model.encode()).hexdigest(),
        extraction='Exclude main; insert native guard call before modeled CMD24 and FIFO write. No old lifecycle is run.'))
    generated('mmc-native-host.c','drivers/storage/a733_mmc_platform_native.c',[
        ('#include "task.h"','#include "task.h"\nextern uint64_t native_model_el,native_model_flags;',1),
        ('__asm__ volatile("mrs %0, CurrentEL\\n mrs %1, daif" : "=r"(el),"=r"(flags));','el=native_model_el;flags=native_model_flags;',1),
        ('__asm__ volatile("msr daifset, #2" ::: "memory");','atomic_signal_fence(memory_order_seq_cst);',1),
        ('__asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));','core=0;',1),
        ('__asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");','atomic_signal_fence(memory_order_seq_cst);',1),
        ('__asm__ volatile("dsb sy" ::: "memory");','atomic_signal_fence(memory_order_seq_cst);',6)])
    (out/'transformations.json').write_text(json.dumps(transforms,indent=2))
    compiler=shutil.which('clang-21') or shutil.which('clang');assert compiler
    kernel=a.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    tiny=a.sources/'tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973'
    inc=['-Iinclude','-Ichecks','-I'+str(out),'-I'+str(a.watchdog_include.resolve()),
         '-Iplatform/a733-storage-update','-I'+str(kernel/'include'),'-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),'-I'+str(tiny/'src')]
    defines=['-DINCLUDE_xSemaphoreGetMutexHolder=1','-DINCLUDE_xTaskGetSchedulerState=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1']
    flags=[compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections']+inc+defines
    providers=['drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c',
        'drivers/gpu/a733_gpu_resources.c','drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c']
    storage=['drivers/storage/a733_storage_update.c','drivers/storage/storage_digest.c','drivers/storage/storage_update_usb.c',
        'drivers/storage/a733_mmc_platform.c','drivers/storage/sunxi_mmc.c']
    cases={
        'power':providers+['checks/a733-storage-native-power.c'],
        'mmc':providers+storage+[str(out/'mmc-native-host.c'),'checks/a733-storage-native-mmc.c'],
        'usb':providers+['drivers/storage/storage_digest.c','drivers/storage/storage_update_usb.c','checks/a733-storage-native-usb.c']}
    if a.only=='usb-native':cases={'usb':cases['usb']}
    native_sources=['platform/a733-storage-update/bootstrap.c','platform/a733-storage-update/worker.c',
        'platform/a733-storage-update/usb.c','drivers/storage/a733_mmc_platform_native.c']
    native=[compiler,'--target=aarch64-none-elf','-std=c11','-O2','-ffreestanding','-fno-builtin','-Wall','-Wextra','-Werror','-fstack-usage','-Iruntime/include']+inc+defines
    commands=[]
    def run(cmd,name):
        cmd=list(map(str,cmd));commands.append(cmd)
        r=subprocess.run(cmd,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (out/name).write_text(r.stdout);(out/'commands.json').write_text(json.dumps(commands,indent=2))
        if r.returncode:print(r.stdout);raise SystemExit(r.returncode)
        return r.stdout
    dependencies=set()
    for group,sources in cases.items():
        for source in sources:dependencies.add((source,False))
    for source in native_sources:dependencies.add((source,True))
    for i,(source,target) in enumerate(sorted(dependencies)):
        run((native if target else flags)+['-M','-MF',out/f'dep-{i}.d',source],f'dep-{i}.log')
    paths={Path(compiler).resolve(),root/'checks/a733-storage-native.py',root/'checks/sunxi-mmc.c',root/'checks/a733-storage-update.c'}
    for dep in out.glob('*.d'):
        paths.update((root/s).resolve() for s in dep.read_text().replace('\\\n',' ').split(':',1)[1].split())
    records=[]
    for path in sorted(paths):
        data=path.read_bytes();sha=hashlib.sha256(data).hexdigest();snapshot=out/'inputs'/sha
        snapshot.parent.mkdir(exist_ok=True)
        if not snapshot.exists():snapshot.write_bytes(data)
        records.append(dict(path=str(path),sha256=sha,snapshot=str(snapshot.relative_to(out))))
    (out/'inputs.json').write_text(json.dumps(records,indent=2))
    for name,sources in cases.items():
        binary=out/name;run(flags+sources+['-Wl,--gc-sections','-o',binary],name+'-build.log')
        print(run([binary],name+'-run.log').strip())
    for i,source in enumerate(native_sources):run(native+['-c',source,'-o',out/f'native-{i}.o'],f'native-{i}.log')
    assert all(hashlib.sha256(Path(r['path']).read_bytes()).hexdigest()==r['sha256'] for r in records)
    (out/'result.json').write_text(json.dumps(dict(result='pass',physical=False,
        scope='Native C admission with modeled CPU/task/MMIO; actual provider metadata and new PMIC snapshots; original AArch64 compile.',
        executed_cases=list(cases),old_lifecycle_replayed=False,watchdog_runtime_executed=False,bootstrap_executed=False),indent=2))


if __name__=='__main__':main()
