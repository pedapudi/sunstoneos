#!/usr/bin/env python3
"""Exercise the storage return adapters with modeled SD completion and close outcomes.

The compiled fragments are the production registration, supervisor callback,
owner step and native close body. Hardware results are supplied by the fixture;
these cases establish software ordering, not card or filesystem qualification.
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/sd_owner.c', 'apps/os/large_sd.c', 'apps/os/sd_hw.h',
                'apps/os/large_load.c', 'apps/os/quiesce.c', 'apps/os/quiesce.h']


def function(source, name):
    match = re.search(r'^\S[^\n]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "quiesce.h"
#include "fat32_rw.h"
#undef SUNSTONE_CRUMB_HERE
#define SUNSTONE_CRUMB_HERE() ((const void *)0x1234)
static int requested, failing;
int os_cpu_return_requested(void){return requested;}
int os_cpu_return_failing(void){return failing;}
static unsigned notified;
static void xTaskNotifyGive(void *p){assert(p);++notified;}
static void platform_exit(unsigned status){(void)status;abort();}
static int os_large_sd_declared(void){return 1;}
static unsigned close_calls, close_ok=1;
static uint64_t sd_deadline(uint64_t v){return v;}
static int sd_time_left(uint64_t v){(void)v;return 1;}
static void sd_pause(void){}
static void fake_close(void){++close_calls;}
static int fake_closed(void){return close_ok;}
'''

OWNER_SETUP = r'''
static void *owner=(void *)1;
static atomic_uint storage_state, posting;
static atomic_int closed;
static unsigned writes_enabled;
struct request { atomic_int done; };
static _Atomic(struct request *) loader_slot, writer_slot;
static void finish(struct request *r,int ok){atomic_store(&r->done,ok?1:-1);}
static uint32_t diag_word(void){return 0x123;}
struct os_sd_hw {void (*close)(void);int (*closed)(void);};
static const struct os_sd_hw hardware={fake_close,fake_closed},*hw=&hardware;
'''

OWNER_TEST = r'''
int main(int argc,char **argv)
{
    assert(argc==2);int test=atoi(argv[1]);
    assert(os_sd_quiesce_register());assert(storage_quiesce.registered);
    if(test)atomic_store(&storage_state,SD_OPEN);
    if(test==2||test==3||test==8){writes_enabled=1;assert(retirement_enable());}
    if(test==2)assert(retirement_write(FWR_PHASE_RAW,0));
    if(test==3)atomic_store(&storage_state,SD_CLOSED);
    if(test==4)close_ok=0;
    if(test==5)atomic_store(&storage_state,SD_FAILED);
    if(test==6)atomic_store(&posting,1);
    if(test==7)atomic_store(&storage_state,SD_STARTING);
    requested=failing=1;
    enum os_quiesce_result r=os_quiesce_step(NULL);
    if(test==0||test==3){assert(r==OS_QUIESCE_DONE);assert(close_calls==0);return 0;}
    if(test==2||test==5){assert(r==OS_QUIESCE_FAULT);assert(close_calls==0);return 0;}
    assert(r==OS_QUIESCE_PENDING);assert(close_calls==0);assert(notified);
    if(test==7){
        assert(!owner_open_allowed());assert(storage_state==SD_CLOSED);
        assert(os_quiesce_step(NULL)==OS_QUIESCE_DONE);assert(!close_calls);return 0;
    }
    struct request reader={0},writer={0};loader_slot=&reader;writer_slot=&writer;
    assert(owner_quiesce_step());
    if(test==6){
        assert(close_calls==0);assert(storage_quiesce.result==OS_QUIESCE_PENDING);
        assert(reader.done==0&&writer.done==0);atomic_store(&posting,0);
        assert(owner_quiesce_step());
    }
    assert(reader.done==-1&&writer.done==-1);
    r=os_quiesce_step(NULL);
    if(test==2){assert(r==OS_QUIESCE_FAULT);assert(close_calls==0);assert(storage_quiesce.site==0x1902);}
    else if(test==4){assert(r==OS_QUIESCE_FAULT);assert(close_calls==1);}
    else {assert(r==OS_QUIESCE_DONE);assert(close_calls==1);}
    unsigned calls=close_calls;
    assert(owner_quiesce_step());
    assert(close_calls==calls);               /* no retry after uncertain close */
    return 0;
}
'''

READ_SETUP = r'''
static atomic_uint storage_state,close_proven;
static _Atomic uint64_t read_closed_us;
static uint64_t sunstone_deadline_now_us(void){return 0;}
static void os_application_wake_main(void){}
static int opened=1;
static uint8_t os_sd_pf_closed;                                     /* the close's PF withstand-field record (large_sd.c) */
struct platform_state {uint64_t stage,fault,fault_why,fault_line,refused;unsigned irq_mode;uint8_t pf_found;};
static struct platform_state platform={.irq_mode=1};
static int irq_live=1,irq_owner_on=1,mask_ok=1,irq_off_ok=1;
static unsigned mask_calls,irq_off_calls;
static int sd_irq_mask_settled(void){++mask_calls;return mask_ok;}
static int a733_mmc_platform_irq(struct platform_state *p,int on)
{assert(p==&platform&&!on);++irq_off_calls;if(irq_off_ok)p->irq_mode=0;return irq_off_ok;}
static struct {uint64_t lifetime_deadline_us;} config={100};
struct device_memory_lease {void *pool;};
static struct device_memory_lease descriptor_lease={(void *)1};
enum a733_mmc_platform_result {A733_MMC_ERROR,A733_MMC_PENDING,A733_MMC_BUSY,A733_MMC_CLOSED,A733_MMC_RETAINED};
static unsigned native_result=A733_MMC_CLOSED,released,retained;
static enum a733_mmc_platform_result a733_mmc_platform_close(void *p,uint64_t end)
{(void)p;(void)end;return native_result;}
static int a733_mmc_platform_retain(void *p){(void)p;++retained;return 1;}
static void *a733_memory_uncached_init(void){return (void *)1;}
static int device_memory_release(void *pool,struct device_memory_lease lease)
{assert(pool&&lease.pool);++released;return close_ok;}
static void vTaskDelay(unsigned tick){(void)tick;}
static uint64_t now_us(void){return 0;}
#define PLATX(r) (r)
'''

READ_TEST = r'''
int main(int argc,char **argv)
{
    assert(argc==2);int test=atoi(argv[1]);
    assert(os_sd_quiesce_register());assert(storage_quiesce.registered);
    if(test)atomic_store(&storage_state,SD_OPEN);
    if(test==2)native_result=A733_MMC_RETAINED;
    if(test==3)close_ok=0;
    if(test==4)mask_ok=0;
    if(test==5)irq_off_ok=0;
    requested=failing=1;
    enum os_quiesce_result r=os_quiesce_step(NULL);
    if(test==0){assert(r==OS_QUIESCE_DONE);assert(!released);return 0;}
    assert(r==OS_QUIESCE_PENDING);assert(!released);
    close_owner();r=os_quiesce_step(NULL);
    if(test==1){assert(r==OS_QUIESCE_DONE);assert(close_proven);assert(!opened);assert(released==1);}
    if(test==2){assert(r==OS_QUIESCE_FAULT);assert(!close_proven);assert(opened);assert(!released);assert(retained==1);}
    if(test==3){assert(r==OS_QUIESCE_FAULT);assert(!close_proven);assert(opened);assert(released==1);}
    if(test==4||test==5){assert(r==OS_QUIESCE_FAULT&&!close_proven&&opened&&!released&&retained==1);}
    assert(mask_calls==1&&!irq_live);
    assert(irq_off_calls==(test==4?0u:1u));
    if(test<4)assert(!irq_owner_on&&!platform.irq_mode);
    return 0;
}
'''


def adapter(source):
    a = source.index('enum { SD_UNOPENED')
    enum = source[a:source.index(';', a)+1]
    a = source.index('static struct os_quiesce storage_quiesce=')
    descriptor = source[a:source.index(';', a)+1]
    return enum + '\n' + function(source, 'storage_quiesce_step') + '\n' + descriptor + '\n' + function(source, 'os_sd_quiesce_register')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',action='append',choices=('owner','read'))
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    owner=(ROOT/'apps/os/sd_owner.c').read_text()
    native=(ROOT/'apps/os/large_sd.c').read_text()
    loader=(ROOT/'apps/os/large_load.c').read_text()
    assert function(loader,'os_large_start').index('os_sd_quiesce_register') < function(loader,'os_large_start').index('xTaskCreateStaticAffinitySet')
    assert 'os_cpu_return_requested()' in function(owner,'post')
    assert 'os_cpu_return_requested()' in function(owner,'start')
    assert 'owner_quiesce_step()' in function(owner,'owner_main')
    # Extract the shared close body after the conditional public function name.
    a=native.index('/* Restore the clock, the pins and the rail when the last region has loaded. */')
    a=native.index('#endif\n{',a)+len('#endif\n')
    depth,end=1,a+1
    while depth:
        depth+=(native[end]=='{')-(native[end]=='}');end+=1
    close='static void close_owner(void)\n'+native[a:end]
    tracker=owner[owner.index('/* Owner-only fields;'):owner.index('static enum os_quiesce_result storage_quiesce_step')]
    variants={'owner':PREAMBLE+OWNER_SETUP+tracker+adapter(owner)+'\n'+function(owner,'owner_quiesce_step')+'\n'+function(owner,'owner_open_allowed')+OWNER_TEST,
              'read':PREAMBLE+READ_SETUP+adapter(native)+'\n'+close+READ_TEST}
    def check(text,name,cases):
        d=args.output/name;d.mkdir(exist_ok=True)
        src=d/'check.c';src.write_text(text);binary=d/'check'
        command=['clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Wno-unused-function',
                 '-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+str(ROOT/'apps/os'),str(src),str(ROOT/'apps/os/quiesce.c'),'-o',str(binary)]
        built=subprocess.run(command,capture_output=True,text=True)
        (d/'build.log').write_text(built.stdout+built.stderr)
        assert built.returncode==0,built.stderr
        for i in range(cases):
            run=subprocess.run([str(binary),str(i)],capture_output=True,text=True)
            if run.returncode:return False
        return True
    count=0
    for name,text in variants.items():
        if args.case and name not in args.case:continue
        cases=9 if name=='owner' else 6
        assert check(text,name,cases),name
        absent=text.replace('os_quiesce_register(&storage_quiesce)','1')
        assert not check(absent,name+'-missing-registration',cases)
        call=function(text,'storage_quiesce_step')
        exits=text.replace(call,call.replace('{','{platform_exit(9);',1),1)
        assert not check(exits,name+'-exit',cases)
        count+=2
    print('PASS selected storage quiescence cases and registration/exit mutants; native hardware effects are modeled')


if __name__=='__main__':main()
