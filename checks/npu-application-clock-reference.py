#!/usr/bin/env python3
"""The linked image's startup register pass against six reference-clock cases.

The image's actual sequencer (a733_npu_startup_init and _step) and its native
allowlist adapter run under Unicorn from the register-pass step, with the
tokens and the supply already admitted. Scheduler state, the current task,
the counter, the register lock and the domain-4 check, which reads domain 4
OFF, are modeled; the CCU
and PCK words are modeled memory. An admitted case requests domain 4 ON and
returns PENDING without a device write, since power precedes every NPU clock
write; a refused case returns RETAINED and writes nothing. No physical
frequency is claimed.
"""
import argparse,hashlib,json,struct,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'checks'))
import harness
LAYOUTS=('a733_npu_startup','a733_npu_startup_config','a733_npu_platform_config','a733_npu_startup_providers','a733_power_domain_row',
    'a733_counter_clock','a733_clock_leases','a733_platform_locks','a733_power_domains','a733_pmic','a733_pmic_config')
CCU=0x02002000
CASES=[('26MHz-normalized24',0x10008002,0xf8675f00,0xff126310,True),('24MHz-normalized24',0x10000002,0xf8636300,0xff126310,True),
       ('26MHz-wrong-equal-factors',0x10008002,0xf8636300,0xff126310,False),('26MHz-unstable',0x00008002,0xf8675f00,0xff126310,False),
       ('26MHz-reference-disabled',0x10008002,0x78675f00,0xff126310,False),('26MHz-PERI0-M1-refused',0x10008002,0xf8675f00,0xff126312,False)]
# AHB, NSI and MBUS on the 600 MHz PERI0 output at 200, 600 and 600 MHz.
FABRIC=(0x03000002,0x83000000,0x81000000);RATES=(200000000,600000000,600000000)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cases',nargs='+')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    e=harness.Image(a.image,layout_names=LAYOUTS);L=e.layouts
    rows=[]
    for label,osc,ref,peri0,admitted in CASES:
        if a.cases and label not in a.cases:continue
        u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000);u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
        for at,data in e.parts:
            if data:u.mem_write(at,data)
        base=0x43000000;s=base;counter=base+0x8000;clocks=base+0x9000;locks=base+0xa000;domains=base+0xb000;pmic=base+0xc000
        cfg=base+0xd000;prov=base+0xd800;ret=0x43fff000
        def put(at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
        def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
        put(counter+L['a733_counter_clock']['initialized'],1,4);put(clocks+L['a733_clock_leases']['initialized'],1,4)
        put(locks+L['a733_platform_locks']['initialized'],1,4)
        put(domains+L['a733_power_domains']['self'],domains);put(domains+L['a733_power_domains']['pmic'],pmic)
        # The owner's service wake (ops.notify, the third callback): a hooked function that returns.
        put(domains+L['a733_power_domains']['ops']+16,e.address('a733_platform_lock_leave'))
        put(pmic+L['a733_pmic']['config']+L['a733_pmic_config']['startup_deadline'],2000000)
        f=cfg+L['a733_npu_startup_config']['fabric']
        for i in range(3):
            put(f+L['a733_npu_platform_config']['fabric']+4*i,FABRIC[i],4);put(f+L['a733_npu_platform_config']['rate_hz']+4*i,RATES[i],4)
        put(cfg+L['a733_npu_startup_config']['owner'],31);put(cfg+L['a733_npu_startup_config']['deadline_us'],10**9)
        put(cfg+L['a733_npu_startup_config']['pck_client'],4,4)
        for name,value in [('counter',counter),('clocks',clocks),('locks',locks),('pck',base+0xe000),('rails',base+0xf000),('domains',domains)]:
            put(prov+L['a733_npu_startup_providers'][name],value)
        words={0x07090160:osc,CCU:ref,CCU+0xa0:peri0,CCU+0xac:0x30000000,CCU+0x1908:0x8fff0fff,CCU+0x1988:0x0fff0000,
               CCU+0x500:FABRIC[0],CCU+0x580:FABRIC[1],CCU+0x588:FABRIC[2],CCU+0xb04:1<<19,CCU+0xc00:1<<31,CCU+0xc0c:1<<16,
               CCU+0x2ac:0x30000000,CCU+0x2b0:0x000b0000}
        for at,v in words.items():put(at,v,4)
        writes=[];state={'running':False,'time':1000,'done':False}
        u.hook_add(UC_HOOK_MEM_WRITE,lambda c,acc,at,size,v,x:writes.append((at,v)),begin=0x02000000,end=0x08ffffff)
        def returned(v=1):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
        task=base+0x10000
        hooks={e.address(n):n for n in ('xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','a733_counter_clock_now',
            'a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_power_domain_check',
            'a733_clock_release','a733_rail_release','a733_gpu_resources_release')}
        sites=set(hooks)|{ret}
        for at,data in e.parts:
            for off in range(0,len(data)-3,4):
                w=int.from_bytes(data[off:off+4],'little')
                if w&0xffffffe0 in (0xd5384240,0xd53be020):sites.add(at+off)
        def hook(c,at,size,x):
            if at==ret:state['done']=True;c.emu_stop();return
            w=get(at,4)
            if at not in hooks:
                if w&0xffffffe0==0xd5384240:c.reg_write(UC_ARM64_REG_X0+(w&31),4)        # CurrentEL: EL1
                else:state['time']+=1;c.reg_write(UC_ARM64_REG_X0+(w&31),state['time']*24)  # CNTPCT
                c.reg_write(UC_ARM64_REG_PC,at+4);return
            n=hooks[at]
            if n=='xTaskGetSchedulerState':returned(2 if state['running'] else 1)
            elif n=='xTaskGetCurrentTaskHandle':returned(task)
            elif n=='a733_counter_clock_now':state['time']+=1;put(c.reg_read(UC_ARM64_REG_X1),state['time']);returned(1)
            elif n=='a733_platform_lock_leave':returned(0)
            elif n=='a733_power_domain_check':put(c.reg_read(UC_ARM64_REG_X2),0,4);returned(1)  # domain 4 OFF
            else:returned(1)
        for at in sites:u.hook_add(UC_HOOK_CODE,hook,begin=at,end=at)
        def call(name,*args):
            state['done']=False;u.reg_write(UC_ARM64_REG_SP,0x43ffeff0);u.reg_write(UC_ARM64_REG_LR,ret)
            for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
            u.emu_start(e.address(name),0,count=20000000);assert state['done'],name;return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
        assert call('a733_npu_startup_init',s,cfg,prov,0,0)==1 and call('a733_npu_startup_bind_task',s,task)==1
        put(s+L['a733_npu_startup']['stage'],2,4)   # A733_NPU_STARTUP_PASS: tokens and supply admitted
        state['running']=True;writes.clear()
        result=call('a733_npu_startup_step',s)
        stage=get(s+L['a733_npu_startup']['stage'],4);request=get(s+L['a733_npu_startup']['row']+L['a733_power_domain_row']['request'],4)
        # PENDING (2) at the row stage (3) with the ON request published; RETAINED (3) refuses.
        if admitted:assert result==2 and stage==3 and request&1 and not writes,(label,result,stage,request,writes[:3])
        else:assert result==3 and not writes and not request&1,(label,result,writes[:3])
        rows.append(dict(case=label,admitted=admitted,domain_on_requested=bool(request&1),writes=len(writes)))
    (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,
        'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':rows,'scope':__doc__},indent=2)+'\n')
    print('PASS startup register pass reference cases',len(rows))
if __name__=='__main__':main()
