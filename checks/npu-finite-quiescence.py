#!/usr/bin/env python3
"""Execute the finite native quiescence and PMIC checkpoint with modeled scheduling."""
import argparse,hashlib,importlib.util,json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
for n in ('elf','evidence'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('layout',root/'checks/storage-read-native.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout);image=layout.Image(a.elf)
with a.elf.open('rb') as stream:
    dwarf=ELFFile(stream).get_dwarf_info();objects=[]
    for cu in dwarf.iter_CUs():
        if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/npu-convolution/native.c'):continue
        for die in cu.iter_DIEs():
            if die.tag!='DW_TAG_variable' or die.attributes.get('DW_AT_name',None) is None or die.attributes['DW_AT_name'].value!=b'app' or 'DW_AT_location' not in die.attributes:continue
            ops=DWARFExprParser(dwarf.structs).parse_expr(die.attributes['DW_AT_location'].value);assert len(ops)==1
            app=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
            fields={x.attributes['DW_AT_name'].value.decode():int(x.attributes['DW_AT_data_member_location'].value) for x in die.get_DIE_from_attribute('DW_AT_type').iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_name' in x.attributes};objects.append((app,fields))
assert len(objects)==1;app,f=objects[0];pmic=app+f['pmic'];pl=image.layouts['a733_pmic'];slot=image.layouts['a733_pmic_slot'];locks=app+f['locks'];ll=image.layouts['a733_platform_locks'];conv=app+f['reserved']+image.layouts['a733_npu_reserved']['convolution'];cl=image.layouts['npu_convolution'];service=conv+cl['service'];sl=image.layouts['npu_service'];startup=app+f['startup'];deadline=startup+image.layouts['a733_npu_startup']['config']+image.layouts['a733_npu_startup_config']['deadline_us'];cases=[]
variants=['success','closed-missing','stopped-missing','wrong-client','provider-held','provider-busy','active-request','slot-live','borrow-live','mutex-held','park-timeout','irq-enabled','irq-pending','irq-active','irq-RWP','mapping-failure','checkpoint-idle','checkpoint-active-finishes','checkpoint-unreaped-slot']
for variant in variants:
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x04000000);u.mem_map(0x03400000,0x10000)
    for at,data in image.parts:
        if data:u.mem_write(at,data)
    def put(at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
    def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
    def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    client,worker,bootstrap,provider=0x43000000,0x43000100,0x43000200,0x43000300
    for name,v in [('client',client),('bootstrap_task',bootstrap),('pmic_task',provider),('used',1),('stopped',int(variant!='stopped-missing'))]:put(app+f[name],v,4 if name in ('used','stopped') else 8)
    put(conv+cl['closed'],int(variant!='closed-missing'),4);put(service+sl['worker'],worker);put(pmic+pl['active'],8,4);put(deadline,3000000)
    put(locks+ll['mutex'],0x43001000);put(locks+ll['mutex']+8,0x43001100)
    state={'time':1000000,'suspended':[],'delays':0,'polls':0,'writes':[]}
    if variant=='provider-held':put(pmic+pl['held'],1,4)
    if variant=='provider-busy':put(pmic+pl['busy'],1,4)
    if variant in ('active-request','checkpoint-active-finishes'):put(pmic+pl['active'],0,4)
    if variant in ('slot-live','checkpoint-unreaped-slot'):put(pmic+pl['slots']+slot['state'],1,4)
    if variant=='borrow-live':put(pmic+pl['slots']+slot['borrow']+image.layouts['a733_rail_borrow']['id'],1)
    for name,at,value in [('irq-enabled',0x0340010c,2),('irq-pending',0x0340020c,2),('irq-active',0x0340030c,2),('irq-RWP',0x03400000,1<<31)]:
        if variant==name:put(at,value,4)
    checkpoint=variant.startswith('checkpoint-')
    if checkpoint:put(app+f['pmic_stop'],1,4)
    hooks={image.address(n,unit='/apps/npu-convolution/native.c' if n=='parent' else None):n for n in ['parent','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','vTaskSuspend','vTaskDelay','a733_counter_clock_now','aarch64_device_translate','xQueueGetMutexHolder','a733_pmic_poll']}
    def code(cpu,at,size,cookie):
        name=hooks.get(at)
        if name is None:return
        args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
        if name=='parent':ret(1)
        elif name=='xTaskGetSchedulerState':ret(2)
        elif name=='xTaskGetCurrentTaskHandle':ret(provider if checkpoint else client+8 if variant=='wrong-client' else client)
        elif name=='a733_counter_clock_now':put(args[1],state['time']);ret(1)
        elif name=='aarch64_device_translate':
            put(args[2],args[0]);put(args[2]+8,0,4);put(args[2]+12,2,4);ret(int(variant!='mapping-failure'))
        elif name=='xQueueGetMutexHolder':ret(1 if variant=='mutex-held' else 0)
        elif name=='vTaskSuspend':
            state['suspended'].append(args[0])
            if checkpoint:u.emu_stop()
            else:ret()
        elif name=='vTaskDelay':
            state['delays']+=1;state['time']+=1000000
            if checkpoint:
                if variant=='checkpoint-unreaped-slot' and state['delays']>=2:put(image.address('a733_npu_application_failed'),1,4)
            elif variant!='park-timeout':put(app+f['pmic_parked'],1,4)
            ret()
        elif name=='a733_pmic_poll':
            state['polls']+=1
            if variant=='checkpoint-active-finishes':put(pmic+pl['active'],8,4)
            ret(1)
    u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,lambda cpu,access,at,size,value,cookie:state['writes'].append(at),begin=0x03400000,end=0x0340ffff)
    u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);u.reg_write(UC_ARM64_REG_X0,app)
    u.emu_start(image.address('pmic_task',unit='/apps/npu-convolution/native.c') if checkpoint else image.address('a733_npu_application_quiesce'),0x43fff000,count=1000000)
    parked=get(app+f['pmic_parked'],4);quiescent=get(app+f['quiescent'],4)
    assert not state['writes'],variant
    if checkpoint:
        assert parked==(variant!='checkpoint-unreaped-slot'),(variant,state)
        assert state['suspended']==[0],(variant,state)
        assert state['polls']==({'checkpoint-idle':0,'checkpoint-active-finishes':1,'checkpoint-unreaped-slot':2}[variant]),(variant,state)
    elif variant=='success':
        assert u.reg_read(UC_ARM64_REG_X0)==1 and quiescent and parked,(variant,state)
        assert state['suspended']==[worker,bootstrap,provider],state
    else:assert u.reg_read(UC_ARM64_REG_X0)==0 and not quiescent,(variant,state)
    cases.append({'name':variant,'passed':True,'parked':parked,'quiescent':quiescent,**state})
a.evidence.mkdir(parents=True,exist_ok=True);(a.evidence/'quiescence-result.json').write_text(json.dumps({'elf_sha256':hashlib.sha256(a.elf.read_bytes()).hexdigest(),'cases':cases,'scope':'Actual native quiesce, PMIC checkpoint, slots/borrows/mutex/IRQ checks. Scheduler, parent allocation observation, time, translation and PMIC poll effects modeled. No physical return or new hardware qualification.'},indent=2)+'\n');print('Nineteen finite NPU quiescence/checkpoint cases passed')
