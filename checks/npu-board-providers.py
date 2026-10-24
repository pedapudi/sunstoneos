#!/usr/bin/env python3
"""The board image's NPU construction: borrowed providers, the domain-4 row and the runtime's pool.

Runs the linked image's actual board_application_start under Unicorn, after
the state the board's kernel_main leaves: the admitted session copy, the
board's shared providers and the power-domain owner, and the extended
write-back pool initialized over 0x48000000-0x78000000. The session's own
validity, FreeRTOS constructors, cache maintenance and address translation
are modeled; the record's validator runs in the preparer's checks. Cases:
the startup borrows the board's counter, registry, locks, its one GPU/PCK
manager, ledger and owner; the domain-4 row is on the owner's list once; the
runtime's parent is one 8,388,608-byte, 2 MiB-aligned allocation inside the
pool, which keeps a largest free extent of the rest; no runtime, service or
client function acquires device memory after admission; the worker, the
bootstrap and the client are pinned to core 1 and USB0 to core 0; INTID 97 is
bound once with its route to core 1 checked.
"""
import argparse,hashlib,json,re,struct,subprocess,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'checks'))
import harness
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
LAYOUTS=('npu_board_application','npu_board_session','a733_board_session','board_provider','board_application',
    'a733_native_usb','a733_npu_startup','a733_npu_startup_providers','a733_power_domains','a733_power_domain_row',
    'device_memory_pool','device_memory_allocation','device_memory_extent','a733_counter_clock','a733_clock_leases',
    'a733_platform_locks','npu_runtime','npu_service','a733_pmic','a733_pmic_config')
WB,WB_END=0x48000000,0x78000000
PARENT=8388608
def variable(path,unit,name):
    with path.open('rb') as f:
        dwarf=ELFFile(f).get_dwarf_info()
        for cu in dwarf.iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(unit):continue
            for die in cu.iter_DIEs():
                if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value.decode()==name \
                   and 'DW_AT_location' in die.attributes:
                    ops=DWARFExprParser(dwarf.structs).parse_expr(die.attributes['DW_AT_location'].value)
                    return ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
    raise AssertionError((unit,name))
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    e=harness.Image(a.image,layout_names=LAYOUTS);L=e.layouts
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000);u.mem_map(WB,0x31000000)
    u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
    for at,data in e.parts:
        if data and at<0x48000000:u.mem_write(at,data)
    def put(at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
    def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
    app=e.address('npu_board');board=e.address('board');providers=e.address('providers')
    shared=providers+L['board_provider']['shared'];domains=providers+L['board_provider']['domains']
    nu=L['a733_native_usb']
    put(shared+nu['counter']+L['a733_counter_clock']['initialized'],1,4)
    put(shared+nu['clocks']+L['a733_clock_leases']['initialized'],1,4)
    put(shared+nu['locks']+L['a733_platform_locks']['initialized'],1,4)
    put(domains+L['a733_power_domains']['self'],domains);put(domains+L['a733_power_domains']['pmic'],shared+nu['pmic'])
    put(shared+nu['pmic']+L['a733_pmic']['config']+L['a733_pmic_config']['startup_deadline'],10**9)
    # The admitted session, also the board's own copy.
    session=bytearray(808);s=L['npu_board_session']
    build=bytes(u.mem_read(e.address('a733_smp_build_id'),32))
    session[L['a733_board_session']['image']:L['a733_board_session']['image']+32]=build
    struct.pack_into('<Q',session,L['a733_board_session']['deadline_us'],20*10**6)
    struct.pack_into('<3I',session,L['a733_board_session']['fabric'],0x03000002,0x83000000,0x81000000)
    struct.pack_into('<3I',session,L['a733_board_session']['fabric_hz'],200000000,600000000,600000000)
    struct.pack_into('<Q',session,s['owner'],31);struct.pack_into('<I',session,s['pck_client'],4)
    admitted=variable(a.image,'apps/npu-board/snapshot.c','admitted')
    u.mem_write(admitted,bytes(session));put(admitted+808,1,4);put(admitted+812,1,4)
    ba=L['board_application']
    u.mem_write(board+ba['admitted_session'],bytes(session[:592]));u.mem_write(board+ba['session'],bytes(session[:592]))
    usb0=0x43f00000;put(board+ba['handles']+8,usb0)
    gicd=0x03400000;put(gicd+0x460,0xd8<<8,4);put(gicd+0x6308,0x100)
    state={'done':False,'scheduler':1,'tasks':[],'affinity':[],'acquire_callers':[]}
    def returned(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    names=('npu_board_session_valid','vTaskEnterCritical','vTaskExitCritical','aarch64_device_cache',
        'aarch64_device_cache_stride','aarch64_device_translate','xTaskCreateStaticAffinitySet','xTaskCreateStatic',
        'xQueueGenericCreateStatic','xQueueCreateMutexStatic','vTaskCoreAffinitySet','xTaskGetSchedulerState',
        'xTaskGetCurrentTaskHandle','a733_counter_clock_now','platform_panic')
    hooks={e.address(n):n for n in names if e.symbols.get(n)}
    ret=0x43fff000;sites=set(hooks)|{ret}
    for at,data in e.parts:
        if at>=0x48000000:continue
        for off in range(0,len(data)-3,4):
            w=int.from_bytes(data[off:off+4],'little')
            if w&0xffffffe0 in (0xd5384240,0xd53b00e0,0xd53be020,0xd53be000,0xd53800a0):sites.add(at+off)
    def hook(c,at,size,x):
        if at==ret:state['done']=True;c.emu_stop();return
        if at not in hooks:
            w=get(at,4);reg=UC_ARM64_REG_X0+(w&31)
            v={0xd5384240:4,0xd53b00e0:4,0xd53be020:10**9,0xd53be000:24000000,0xd53800a0:0x100}[w&0xffffffe0]
            c.reg_write(reg,v);c.reg_write(UC_ARM64_REG_PC,at+4);return
        n=hooks[at];r=[c.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if n=='npu_board_session_valid':returned(1)
        elif n in ('vTaskEnterCritical','vTaskExitCritical'):returned()
        elif n=='aarch64_device_cache':returned(1)
        elif n=='aarch64_device_cache_stride':returned(64)
        elif n=='aarch64_device_translate':
            attr,share=(0xff,3) if r[0]>=0x40000000 else (0,2)
            u.mem_write(r[2],struct.pack('<QII',r[0],attr,share));returned(1)
        elif n=='xTaskCreateStaticAffinitySet':
            name=bytes(u.mem_read(r[1],16)).split(b'\0')[0].decode();state['tasks'].append((name,r[7],r[6]))
            returned(r[6])
        elif n=='xTaskCreateStatic':state['tasks'].append((bytes(u.mem_read(r[1],16)).split(b'\0')[0].decode(),None,r[6]));returned(r[6])
        elif n=='xQueueGenericCreateStatic':returned(r[3])
        elif n=='xQueueCreateMutexStatic':returned(r[1])
        elif n=='vTaskCoreAffinitySet':state['affinity'].append((r[0],r[1]));returned()
        elif n=='xTaskGetSchedulerState':returned(state['scheduler'])
        elif n=='xTaskGetCurrentTaskHandle':returned(0)
        elif n=='a733_counter_clock_now':put(r[1],1000);returned(1)
        else:raise AssertionError(('panic',bytes(u.mem_read(r[0],64)).split(b'\0')[0]))
    for at in sites:u.hook_add(UC_HOOK_CODE,hook,begin=at,end=at)
    def call(name,*args):
        state['done']=False;u.reg_write(UC_ARM64_REG_SP,0x43ffeff0);u.reg_write(UC_ARM64_REG_LR,ret)
        for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
        u.emu_start(e.address(name),0,count=400000000);assert state['done'],name;return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
    pool=e.address('boot_pool') if e.symbols.get('boot_pool') else variable(a.image,'platform/a733/memory_reservations.c','boot_pool')
    assert call('device_memory_pool_init_typed',pool,WB,WB,WB_END-WB,1,0)==1
    # The extended pool getter returns the pool once device preparation ends.
    hooks[e.address('a733_memory_reservations_init')]='pool';sites.add(e.address('a733_memory_reservations_init'))
    u.hook_add(UC_HOOK_CODE,lambda c,at,size,x:returned(pool),begin=e.address('a733_memory_reservations_init'),end=e.address('a733_memory_reservations_init'))
    assert call('board_application_start')==1,'board_application_start refused'
    na=L['npu_board_application'];sl=L['a733_npu_startup'];pl=L['a733_npu_startup_providers']
    startup=app+na['startup'];pv=startup+sl['providers']
    expected=dict(counter=shared+nu['counter'],clocks=shared+nu['clocks'],locks=shared+nu['locks'],
        pck=shared+nu['gpu'],rails=shared+nu['rails'],domains=domains)
    assert {k:get(pv+pl[k]) for k in expected}==expected,'startup providers differ from the board providers'
    rl=L['a733_power_domain_row'];row=get(domains+L['a733_power_domains']['rows']);rows=[]
    while row and len(rows)<8:rows.append((row,get(row+rl['domain'],4)));row=get(row+rl['next'])
    assert rows==[(startup+sl['row'],4)],rows
    extent=app+na['extent'];el=L['device_memory_extent']
    cpu,phys,size=get(extent+el['cpu']),get(extent+el['physical']),get(extent+el['bytes'])
    assert size==PARENT and cpu==phys and WB<=cpu<cpu+size<=WB_END and not cpu%(2<<20),(hex(cpu),size)
    pl2=L['device_memory_pool'];al=L['device_memory_allocation'];used=[]
    for i in range(32):
        slot=pool+pl2['allocations']+i*32 # sizeof(struct device_memory_allocation)
        if get(slot+al['owner']):used.append((get(slot+al['first'],4),get(slot+al['pages'],4)))
    assert used==[((cpu-WB)//4096,PARENT//4096)],used
    free=[]
    edge=0
    for first,pages in sorted(used):
        if first>edge:free.append(first-edge)
        edge=first+pages
    free.append((WB_END-WB)//4096-edge);largest=max(free)*4096
    assert largest>=PARENT
    # No runtime, service or client function acquires device memory.
    dis=subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(a.image)],text=True)
    acquire=hex(e.address('device_memory_acquire'))[2:];callers=set();fn=None
    for line in dis.splitlines():
        m=re.match(r'^[0-9a-f]+ <(.+)>:$',line)
        if m:fn=m.group(1);continue
        if re.search(r'\sbl\s+0x0*'+acquire+r'\b',line):callers.add(fn)
    assert 'board_application_start' in callers and not any(re.match(r'(npu_runtime|npu_service|npu_board_client|a733_npu_)',c) for c in callers),callers
    tasks={t[0]:t for t in state['tasks']}
    assert {k:tasks[k][1] for k in ('npu-boot','npu-client','npu-service')}=={'npu-boot':2,'npu-client':2,'npu-service':2},state['tasks']
    assert state['affinity']==[(usb0,1)],state['affinity']
    binding=variable(a.image,'apps/npu-board/irq.c','binding')
    assert get(binding)==app+na['runtime']+L['npu_runtime']['service']
    record=dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        providers={k:hex(v) for k,v in expected.items()},domain_rows=len(rows),parent=dict(cpu=hex(cpu),bytes=size),
        largest_free_extent_bytes=largest,acquire_callers=sorted(callers),tasks=[(t[0],t[1]) for t in state['tasks']],scope=__doc__)
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    print('PASS NPU board providers, domain-4 row and runtime pool: parent',size,'bytes, largest free extent',largest)
if __name__=='__main__':main()
