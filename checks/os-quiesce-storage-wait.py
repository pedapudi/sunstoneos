#!/usr/bin/env python3
"""Check native storage waits against an asynchronously published return deadline."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('owner_check',ROOT/'checks/os-quiesce-storage.py')
owner_check=importlib.util.module_from_spec(spec);spec.loader.exec_module(owner_check)
CHECK_INPUTS=['apps/os/large_sd.c','apps/os/cpu-return.h','drivers/storage/storage_wait.c',
              'include/storage_wait.h','checks/os-quiesce-storage-wait.c','checks/os-quiesce-storage-lock.c',
              'drivers/storage/a733_mmc_platform_native.c','platform/a733/shared_locks.c','checks/platform-lock-step.py']

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    source=(ROOT/'apps/os/large_sd.c').read_text()
    helpers='\n'.join(owner_check.function(source,n) for n in ['sd_deadline','sd_time_left','sd_admission','sd_pause','recover_blocking'])
    helpers+='\nstruct drive { enum a733_mmc_platform_result r; int (*poll)(void); };\n'
    helpers+='static volatile enum sunxi_mmc_wait last_hint;\nstatic _Atomic uint64_t recheck_count,sleep_us,sleep_count;\n'
    helpers+='\n'.join(owner_check.function(source,n) for n in ['dr_poll','recheck_for','dr_hint','dr_now','dr_sleep','dr_live','drive_platform','hw_write_piece','hw_allocation_unit'])
    start=source.index('/* Restore the clock, the pins and the rail when the last region has loaded. */')
    start=source.index('#endif\n{',start)+len('#endif\n');depth,end=1,start+1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    helpers+='\nstatic void close_owner(void)\n'+source[start:end]
    a=source.index('/* Identification polls for up to a millisecond')
    a=source.index('for (;;)',a); b=source.index('        if (start18 || !found_at_1v8())',a)
    helpers+='\nstatic const void *identify_owner(void) {\n'+source[a:b]+'return (const void *)1;}\n'
    a=source.index('        enum a733_mmc_platform_result c;',b)
    b=source.index('        memset(&platform, 0, sizeof platform);',a)
    helpers+='\nstatic const void *reentry_close(void) {\n'+source[a:b]+'return (const void *)1;}\n'
    a=source.index('    for (unsigned waited = 0;');b=source.index('    struct device_memory_pool *pool',a)
    helpers+='\nstatic const void *pmic_wait(void) {\n#ifdef OS_SD_WRITE\n'+source[a:b]+'return (const void *)1;}\n'
    opening=owner_check.function(source,'os_sd_open_owner')
    assert opening.index('sunstone_deadline_register()')<opening.index('os_sd_open_owner_device()')
    def run(name,text):
        out=args.output/name;out.mkdir(exist_ok=True);(out/'storage-native-wait-model.h').write_text(text)
        command=['clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(out),'-I'+str(ROOT/'include'),
                 '-I'+str(ROOT/'apps/os'),str(ROOT/'checks/os-quiesce-storage-wait.c'),
                 str(ROOT/'drivers/storage/storage_wait.c'),'-o',str(out/'check')]
        build=subprocess.run(command,capture_output=True,text=True);(out/'build.log').write_text(build.stdout+build.stderr)
        assert build.returncode==0,build.stderr
        log=''
        for case in range(14):
            result=subprocess.run([str(out/'check'),str(case)],capture_output=True,text=True)
            log+=result.stdout+result.stderr
            if result.returncode:break
        (out/'result.log').write_text(log)
        return result.returncode==0,log
    passed,log=run('current',helpers);assert passed,log;print(log.strip())
    mutations={
        'no-shared-deadline':helpers.replace('if(closing&&closing<deadline)deadline=closing;','(void)closing;'),
        'retry-after-return':helpers.replace('if(os_cpu_return_requested())return 0;',''),
        'poll-after-expiry':helpers.replace('if(!sd_time_left(config.lifetime_deadline_us)){d->r=A733_MMC_ERROR;return 1;}',''),
        'unknown-deadline':helpers.replace('if(os_cpu_return_requested()&&!os_cpu_return_deadline_us())return 0;',''),
    }
    for name,text in mutations.items():assert not run(name,text)[0],name
    print('PASS 14 native storage wait cases and 4 deadline/admission mutants; hardware is modeled')
    spec=importlib.util.spec_from_file_location('lock_check',ROOT/'checks/platform-lock-step.py')
    lock_check=importlib.util.module_from_spec(spec);spec.loader.exec_module(lock_check)
    locks=(ROOT/'platform/a733/shared_locks.c').read_text()
    locks=locks[locks.index('static uint64_t lock_deadline('):]
    native=(ROOT/'drivers/storage/a733_mmc_platform_native.c').read_text()
    fixture=(ROOT/'checks/os-quiesce-storage-lock.c').read_text()
    def native_run(name,lock_source,native_source):
        out=args.output/name;out.mkdir(exist_ok=True)
        (out/'storage-native-current.c').write_text(native_source)
        (out/'check.c').write_text(lock_check.SHIM+lock_source+fixture)
        command=['clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(out),'-I'+str(ROOT/'include'),
                 str(out/'check.c'),str(ROOT/'drivers/storage/a733_storage_bounds.c'),str(ROOT/'drivers/storage/storage_digest.c'),'-o',str(out/'check')]
        build=subprocess.run(command,capture_output=True,text=True);(out/'build.log').write_text(build.stdout+build.stderr)
        assert build.returncode==0,build.stderr
        result=subprocess.run([str(out/'check')],capture_output=True,text=True);(out/'result.log').write_text(result.stdout+result.stderr)
        return result
    result=native_run('native-lock',locks,native);assert not result.returncode,result.stdout+result.stderr;print(result.stdout.strip())
    variants=[(locks.replace('deadline=lock_deadline(p,deadline);',''),native),
              (locks,native.replace('!a733_platform_lock_live(n->locks)','0')),
              (locks,native.replace('a733_platform_lock_live(n->locks)&&',''))]
    for index,(l,n) in enumerate(variants):assert native_run('native-lock-mutant-'+str(index),l,n).returncode!=0
    print('PASS three native MMC held-lock/time/MMIO mutations')
if __name__=='__main__':main()
