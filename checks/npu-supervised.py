#!/usr/bin/env python3
"""One supervised native link and the supply, startup sequencer and refusal composition.

The ELF contains no admitted session. Architectural/device responses are modeled;
no physical admission or numerical inference is supplied. No board is used.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
from elftools.elf.elffile import ELFFile

ROOT=Path(__file__).resolve().parents[1]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def build(args,out):
    commands=[]
    def run(name,command):
        command=list(map(str,command));commands.append(command);save(out/'commands.json',commands)
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
    b=out/'build'
    run('configure',['cmake','-S',ROOT,'-B',b,
        '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/aarch64-clang.cmake'),
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCMAKE_C_FLAGS=-Werror',
        '-DFREERTOS_DIR='+str(args.freertos.resolve()),'-DTINYUSB_DIR='+str(args.tinyusb.resolve()),
        '-DSUNSTONE_LLD='+str(args.lld.absolute()),'-DA733_NPU_SUPERVISED=ON',
        '-DPython3_EXECUTABLE='+sys.executable,
        '-DA733_NPU_MODEL='+str(args.artifacts/'fixture_model.bin'),
        '-DA733_NPU_INPUT='+str(args.artifacts/'fixture_input.bin'),
        '-DA733_NPU_REFERENCE='+str(args.artifacts/'fixture_output.bin')])
    run('build',['cmake','--build',b,'--target','a733-npu-convolution-supervised','-j4'])
    image=out/'a733-npu-convolution-supervised.elf';shutil.copy2(b/'npu-supervised'/image.name,image)
    shutil.copy2(b/'npu-supervised/a733-npu-convolution-supervised.map',out/'a733-npu-convolution-supervised.map')
    inputs={Path(p) for p in (b/'npu-supervised/a733-npu-convolution-supervised/identity-inputs.txt').read_text().splitlines()}
    # Include assembly translation units explicitly; some compilers omit .S.d.
    for entry in json.loads((b/'compile_commands.json').read_text()):
        if 'a733-npu-convolution-supervised.dir' in entry['command']:inputs.add(Path(entry['file']))
    for dep in (b/'npu-supervised/CMakeFiles/a733-npu-convolution-supervised.dir').rglob('*.obj.d'):
        inputs.update(Path(p) for p in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
    inputs.update([Path(__file__),ROOT/'checks/a733-smp-cached.py',ROOT/'CMakeLists.txt',
        ROOT/'cmake/aarch64-clang.cmake',b/'compile_commands.json',b/'npu-supervised/a733-npu-convolution-supervised/identity-inputs.txt'])
    inputs.update(ROOT/p for p in ('checks/npu-supervised-host.py','apps/npu-convolution/prepare-supervised.py',
        'apps/npu-convolution/observe.py','apps/npu-convolution/artifacts.py','tools/check-a733-npu-supervised.py',
        'tools/check-a733-smp-supervised.py','tools/check-a733-smp.py','tools/check-a733-smp-observe.py',
        'tools/usb-access.py'))
    import capstone, unicorn, elftools
    inputs.update([Path(sys.executable),Path(shutil.which('clang')),args.lld,
                   Path(capstone.__file__),Path(unicorn.__file__),Path(elftools.__file__)])
    for package in (capstone,unicorn):
        inputs.update(Path(package.__file__).parent.rglob('*.so*'))
    captured={}
    for index,p in enumerate(sorted(p.resolve() for p in inputs)):
        target=out/'inputs'/str(index)/p.name;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(p,target);captured[str(p)]={'sha256':sha(target),'snapshot':str(target.relative_to(out))}
    save(out/'inputs.json',captured)
    with image.open('rb') as stream:
        elf=ELFFile(stream);symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
        assert not [s.name for s in symbols.values() if s.name and s['st_shndx']=='SHN_UNDEF']
        layout={name:symbols[name]['st_value'] for name in ('__bss_end','__device_pool_start','__device_pool_end')}
        assert layout['__bss_end']<=layout['__device_pool_start']<layout['__device_pool_end']<=0x41f00000
        assert layout['__device_pool_end']-layout['__device_pool_start']==2097152
        ranges=[(segment['p_vaddr'],segment['p_vaddr']+segment['p_memsz'])
            for segment in elf.iter_segments() if segment['p_type']=='PT_LOAD' and segment['p_memsz']]
        assert all(0x41000000<=a<b<=0x42200000 for a,b in ranges), 'Native image uses SRAM or unreserved memory'
        layout['loaded_ranges']=ranges
        for name in ('a733_cached_el3_vectors','a733_cached_el1_vectors','a733_cpu_el1_stacks',
                     '__a733_smp_boot_stack_top','__tables_start','__tables_end'):
            assert name in symbols and 0x41000000<=symbols[name]['st_value']<=0x42200000, name
    save(out/'layout.json',layout)
    save(out/'image-manifest.json',{'target':'a733-npu-convolution-supervised','protocol_version':1,'final_elf':sha(image),
        'physical_access':False,'external_session_required':True})
    return image

def stages():
    """The startup stage numbers by name, from include/a733_npu_startup.h."""
    text=(ROOT/'include/a733_npu_startup.h').read_text()
    body=re.search(r'enum \{ (A733_NPU_STARTUP_TOKENS[^}]*)\}',text).group(1)
    return {name.strip():i for i,name in enumerate(body.split(',')) if name.strip()}


def execute(image,out,selected_case=None):
    model=module('cached_model',ROOT/'checks/a733-smp-cached.py')
    from unicorn import UC_MEM_WRITE
    stops=set()
    class Machine(model.Machine):
        translations=0
        writes=[]
        selected=0xff
        bus_phase='idle'
        rail_selector=30
        lock_missing=False
        def memory(self,u,access,address,size,value,c):
            if access==UC_MEM_WRITE and address<0x41000000:
                self.writes.append((address,value))
                if address==0x020022a0:
                    self.mmio[address]=value|(0x10000000 if value&(1<<29) and not self.lock_missing else 0);return
                if address==0x02003920:
                    self.mmio[address]=value;self.mmio[0x020039a0]=value&0x10000;return
                # A PPU policy write is acknowledged at once in the status word.
                if address&~0x7000==0x07060000 and 0x07061000<=address<0x07066000:
                    self.mmio[address]=value;self.mmio[address+8]=(self.mmio.get(address+8,0)&~15)|(value&15);return
                if address==0x7083008:
                    self.mmio[address]=value
                    if value==0x6c and self.bus_phase=='start':self.bus_phase='address'
                    elif value==0x6d and self.bus_phase=='restart':self.bus_phase='read-address'
                    elif self.bus_phase=='register':self.selected=value;self.bus_phase='register-byte'
                    else:raise AssertionError(('unexpected TWI data',self.bus_phase,value))
                    return
                if address==0x708300c:
                    if value==0x68:
                        self.bus_phase='start' if self.bus_phase=='idle' else 'restart'
                        status=8 if self.bus_phase=='start' else 0x10
                    elif value==0x58:
                        self.bus_phase='idle';self.mmio[address]=0x40;self.mmio[0x7083010]=0xf8;return
                    elif value==0x48:
                        status={'address':0x18,'register-byte':0x28,'read-address':0x40,'receive':0x58}[self.bus_phase]
                        self.bus_phase={'address':'register','register-byte':'restart-wait',
                            'read-address':'receive','receive':'data'}[self.bus_phase]
                        # DCDC2 (0x13) and DCDC4 (0x15) enabled in 0x10 at selector 30, 800 mV.
                        if status==0x58:self.mmio[0x7083008]={0xff:0,0x0e:3,0x0f:0,0x10:0x0a,0x13:self.rail_selector,0x15:30}[self.selected]
                    else:raise AssertionError(('unexpected TWI control',value))
                    self.mmio[address]=0x48;self.mmio[0x7083010]=status;return
            super().memory(u,access,address,size,value,c)
        def instruction(self,u,pc,size,c):
            if pc in stops:u.emu_stop();return
            if pc==self.symbols['digest']:
                address,n,dest=(self.reg(c,r) for r in ('x0','x1','x2'))
                assert 0<n<=524288 and 0x41000000<=address and address+n<=0x42000000
                assert 0x41200000<=dest and dest+32<=0x42000000
                ctypes.memmove(self.ram_pointer+dest-0x41000000,hashlib.sha256(self.read(address,n)).digest(),32)
                u.reg_write(model.UC_ARM64_REG_PC,self.reg(c,'x30'));return
            if pc==self.symbols['platform_panic']:
                raise AssertionError(('native panic',hex(self.reg(c,'x0')),self.reg(c,'x1'),hex(self.reg(c,'x30'))))
            if pc not in self.decoded:self.decoded[pc]=next(self.decoder.disasm(bytes(u.mem_read(pc,4)),pc))
            ins=self.decoded[pc]
            if ins.mnemonic=='at':
                op,reg=ins.op_str.split(', ');assert op in ('s1e1r','s1e1w')
                page=self.reg(c,reg)&~4095
                # Device-nGnRnE outer shareable: the NPU registers and the
                # power-domain owner's PPU, CCU and R_CCU pages.
                device=(0x03600000,0x02002000,0x07010000,0x07061000,0x07064000,0x07065000)
                self.regs[c]['par_el1']=((0xff<<56)|page|(3<<7)) if 0x41000000<=page<0x42000000 else (page|(2<<7)) if page in device else 1
                self.translations+=1;u.reg_write(model.UC_ARM64_REG_PC,pc+4);return
            # Each counter read advances the modeled counter by 1 us, so the
            # sequencer's LOCK spacing and the PMIC phase spins end.
            if ins.mnemonic=='mrs' and ins.op_str.endswith('cntpct_el0'):self.clock+=24
            super().instruction(u,pc,size,c)
    m=Machine(image);sym=m.symbols;u=m.cpus[0]
    required=('a733_npu_application_failed','a733_npu_session_layout',
        'a733_smp_contract_valid','a733_smp_diagnostic_start','a733_smp_entry','bootstrap',
        'app','a733_power_service_step','a733_clock_token_status','vTaskDelay','vTaskSuspend',
        'vTaskStartScheduler','pxCurrentTCBs','xSchedulerRunning')
    missing=[name for name in required if name not in sym]
    assert not missing, 'Fixture requires actual native symbols: '+', '.join(missing)
    def write(a,data):ctypes.memmove(m.ram_pointer+a-0x41000000,bytes(data),len(data))
    def word(a,value):write(a,struct.pack('<Q',value))
    def uint(a,value):write(a,struct.pack('<I',value))
    def q(a):return struct.unpack('<Q',m.read(a,8))[0]
    def v(a):return struct.unpack('<I',m.read(a,4))[0]
    def cpu(el=1):
        m.regs[0].update(currentel=el*4,spsel=1,daif=0 if v(sym['xSchedulerRunning']) else 0x3c0,tpidr_el1=0,
            sctlr_el1=0x30d01805,par_el1=0,ctr_el0=(4<<24)|(4<<16),
            sp_el1=sym['a733_cpu_el1_stacks']+32768-4096,sp_el3=0x42010000,
            vbar_el1=sym['a733_cached_el1_vectors'])
        u.reg_write(model.UC_ARM64_REG_SP,m.regs[0][m.spname(0)])
    def run(entry,stop,limit=12000000):
        stops.update(stop)
        try:u.emu_start(entry,0,count=limit)
        finally:stops.clear()
        pc=u.reg_read(model.UC_ARM64_REG_PC)
        assert pc in stop,('instruction bound',hex(pc),hex(u.reg_read(model.UC_ARM64_REG_LR)))
        return pc
    def call(name,*args,el=1):
        cpu(el)
        for reg,value in zip(model.GPR,args):u.reg_write(reg,value)
        u.reg_write(model.UC_ARM64_REG_LR,sym['a733_smp_entry'])
        run(sym[name],{sym['a733_smp_entry']})
        return u.reg_read(model.UC_ARM64_REG_X0)
    exported=struct.unpack('<6Q',m.read(sym['a733_npu_session_layout'],48))
    with image.open('rb') as f:
        dies=[d for cu in ELFFile(f).get_dwarf_info().iter_CUs() for d in cu.iter_DIEs()]
        def layout(name,variable=False,size=None):
            die=next(d for d in dies if d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value.decode()==name
                and d.tag==('DW_TAG_variable' if variable else 'DW_TAG_structure_type') and (variable or d.attributes.get('DW_AT_byte_size'))
                and (size is None or d.attributes['DW_AT_byte_size'].value==size))
            if variable:die=die.get_DIE_from_attribute('DW_AT_type')
            return die.attributes['DW_AT_byte_size'].value,{d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value
                for d in die.iter_children() if d.tag=='DW_TAG_member'}
        # Two structures are named npu_session; the host record has the exported size.
        layouts={'npu_session':layout('npu_session',size=exported[0])}
        layouts.update({name:layout(name) for name in ('a733_npu_application_boot','a733_npu_platform_config',
            'a733_pmic_config','a733_pin_handoff','a733_cached_runtime','a733_npu_startup','a733_npu_startup_config',
            'a733_clock_token','a733_rail_token','a733_power_domain_row','a733_power_domains')})
        _,app=layout('app',True)
    def off(name,field):return layouts[name][1][field]
    size=layouts['npu_session'][0];session=bytearray(size);boot=off('npu_session','boot')
    assert (size,boot,layouts['a733_npu_application_boot'][0])==exported[:3]
    def put(offset,value,fmt='Q'):struct.pack_into('<'+fmt,session,offset,value)
    # The all-core record: the version the all-core image admits.
    struct.pack_into('<6Q',session,0,0x415337334e504131,2,size,5,7,9)
    session[48:80]=m.read(sym['a733_smp_build_id'],32)
    for n in range(4):session[80+n*32:112+n*32]=hashlib.sha256(('modeled authority '+str(n)).encode()).digest()
    put(208,11);put(216,77)
    # Evidence words, then the NPU, PMIC, USB and bridge owners.
    struct.pack_into('<9Q',session,boot,17,18,19,20,21,1,2,3,4)
    local=boot+off('a733_npu_application_boot','npu');pmic=boot+off('a733_npu_application_boot','pmic')
    assert (local-boot,pmic-boot)==exported[3:5]
    fabric=(0x03000001,0x82000001,0x84000000)
    for i,x in enumerate(fabric):put(local+off('a733_npu_platform_config','fabric')+i*4,x,'I')
    for i,x in enumerate((300000000,400000000,400000000)):put(local+off('a733_npu_platform_config','rate_hz')+i*4,x,'I')
    put(pmic+off('a733_pmic_config','owner'),2);put(pmic+off('a733_pmic_config','identity'),20)
    pos=pmic+off('a733_pmic_config','pins');session[pos:pos+6]=bytes([2,0,0,2,0,0])
    put(boot+off('a733_npu_application_boot','pins')+off('a733_pin_handoff','authorized')+11*4,3,'I')
    write(0x41f00000,session);(out/'modeled-session.bin').write_bytes(session)
    contract=struct.pack('<6Q',0x415337334e504331,3,224,5,7,(1<<64)-8)+session[48:80]
    contract+=struct.pack('<14Q',24,24000024,480000024,480000024,9,*([0]*7),11,0)+hashlib.sha256(session).digest()
    assert len(contract)==224;write(0x42004000,contract);m.clock=100
    assert call('npu_session_copy')==0
    assert call('npu_session_valid',0x41f00000)==1
    refused=[]
    negatives=[('pmic-startup-deadline',pmic+off('a733_pmic_config','startup_deadline'),'Q',1),
        ('owner-collision',boot+7*8,'Q',1),('fabric-parent',local+off('a733_npu_platform_config','fabric'),'I',0x01000001),
        ('version',8,'Q',3)]
    for label,offset,fmt,value in negatives:
        raw=bytes(session);put(offset,value,fmt);write(0x41f00000,session)
        assert call('npu_session_valid',0x41f00000)==0,label;refused.append(label)
        session[:]=raw;write(0x41f00000,session)
    word(0x42004000+128,1);assert call('a733_smp_contract_valid',0x42004000,m.clock,el=3)==0
    write(0x42004000,contract)
    write(0x41f00000+80,bytes(32));assert call('a733_smp_contract_valid',0x42004000,m.clock,el=3)==0
    write(0x41f00000,session)
    assert call('a733_smp_contract_valid',0x42004000,m.clock,el=3)==1
    write(sym['a733_observe_identity'],struct.pack('<5Q',5,7,9,24,480000024))
    for name in ('probe_arrived','probe_finished'):write(sym[name],struct.pack('<8I',*([1]*8)))
    uint(sym['probe_release'],1)
    word(sym['a733_cached_runtime']+off('a733_cached_runtime','startup_deadline'),24000024)
    # The boot prepares the local pools in platform_init before this entry.
    assert call('a733_memory_reservations_prepare')==1 and call('a733_memory_uncached_prepare')==1
    cpu();run(sym['a733_smp_diagnostic_start'],{sym['vTaskStartScheduler']})
    assert m.translations>512 and not m.writes
    address=sym['app'];startup=address+app['startup'];domains=address+app['domains'];registry=address+app['registry']
    bootstrap=q(address+app['bootstrap_task']);pmic_task=q(address+app['pmic_task'])
    assert bootstrap and pmic_task and q(startup+off('a733_npu_startup','task'))==bootstrap
    # The startup registered its domain-4 row with the power-domain owner.
    row=startup+off('a733_npu_startup','row');listed=q(domains+off('a733_power_domains','rows'));rows=[]
    while listed and len(rows)<8:rows.append(listed);listed=q(listed+off('a733_power_domain_row','next'))
    assert row in rows and v(row+off('a733_power_domain_row','domain'))==4,rows
    uint(sym['xSchedulerRunning'],1)
    # Model scheduler entry only; the existing scheduler scenarios are not run.
    for name in ('ullCriticalNestings','ullPortInterruptNestings'):write(sym[name],bytes(8*8))
    checkpoint=bytes(m.ram);clock_at=m.clock
    stage=startup+off('a733_npu_startup','stage');failed=startup+off('a733_npu_startup','failed')
    results={}
    modes=('ready','wrong-rail','domain-on','lock-timeout')
    assert selected_case is None or selected_case in modes
    for mode in modes:
        if selected_case and mode!=selected_case:continue
        ctypes.memmove(m.ram_pointer,checkpoint,len(checkpoint));m.clock=clock_at;m.writes=[];m.bus_phase='idle';m.selected=0xff
        m.rail_selector=31 if mode=='wrong-rail' else 30
        m.lock_missing=mode=='lock-timeout'
        domain4=8 if mode=='domain-on' else 0
        m.mmio.update({0x07090160:0x10000002,0x02002000:0xf8000000|(99<<16)|(99<<8),
            0x020020a0:0xfe126310,0x020020a8:0,0x020020ac:0,
            0x02003908:0x0e060e06,0x02003988:0x0e060000,
            0x020022a0:0x40104500,0x020022a4:0,0x020022a8:0,0x020022ac:0x30000000,
            0x020022b0:0xb0000,0x02003920:0,0x020039a0:0,
            # SYSCTRL shared SRAM routing: bit 1 set, the boot ROM's CPU routing.
            0x03000008:2,
            # SID speed bin of port 3-3, code 4 (vf0400): the vendor point is 852 MHz at 800 mV.
            0x0300624c:0x496,
            0x0701000c:0,0x07010010:0,0x0701019c:0x10001,0x07025000:0x22,0x07025014:0,0x07025024:0,0x07025210:0,
            0x03400000:0,0x0340011c:0,0x0340021c:0,0x0340031c:0,
            0x708300c:0x40,0x7083010:0xf8,0x7083020:0x30,0x7083018:0,0x708301c:0,
            0x7083200:0,0x7083210:0xc0,0x7083214:0,0x7083218:0,0x708321c:0,
            0x7083314:0,0x7083318:0,0x708331c:0,0x7083014:5<<3,
            # Domain-level gates: MBUS AHB and MBUS bits of domain 4, the PPU bus clock.
            0x020025c0:1<<6,0x020025e0:1<<18,0x070101ac:1})
        for domain in (1,4,5):
            base_=0x07060000+0x1000*domain;value=domain4 if domain==4 else 0
            m.mmio.update({base_:value,base_+8:value})
        for off_,value in zip((0x500,0x580,0x588,0xa80,0xa88,0xa8c,0xb00,0xb04,0xc00,0xc0c,0x1b10,0x1b14,0x1b1c),
                              (*fabric,0,0,0,0,0x80000,0x80000000,0x10000,0,0,0)):m.mmio[0x02002000+off_]=value
        word(sym['pxCurrentTCBs'],bootstrap);cpu();u.reg_write(model.UC_ARM64_REG_X0,address)
        entry=sym['bootstrap'];polls=0
        while polls<320:
            pc=run(entry,{sym['vTaskDelay'],sym['vTaskSuspend'],sym['ulTaskGenericNotifyTake']});polls+=1
            if pc==sym['vTaskSuspend']:break
            # One power-service step as the PMIC task between bootstrap polls.
            context=u.context_save();registers=dict(m.regs[0]);word(sym['pxCurrentTCBs'],pmic_task)
            ticks=call('a733_power_service_step',domains)
            assert ticks!=(1<<32)-1,('power service failed',mode,polls)
            u.context_restore(context);m.regs[0]=registers;word(sym['pxCurrentTCBs'],bootstrap)
            m.clock+=24000;entry=u.reg_read(model.UC_ARM64_REG_LR);u.reg_write(model.UC_ARM64_REG_PC,entry)
        assert polls<320,(mode,'bootstrap did not stop',v(stage))
        npu_writes=[x for x in m.writes if x[0] in (0x020022a0,0x02003920,0x02002b00,0x02002b04)]
        ppu_writes=[x for x in m.writes if 0x07061000<=x[0]<0x07066000]
        assert not [x for x in m.writes if x[0] in (0x02002c00,0x02002c0c)], 'DRAM clocks must remain untouched'
        assert all(value&(1<<19) for address_,value in m.writes if address_==0x02002b04), 'SRAM reset changed'
        assert pc==sym['vTaskSuspend']
        stage_of=stages()
        if mode in ('ready','domain-on'):
            assert v(stage)==stage_of['A733_NPU_STARTUP_READY'] and not v(failed)
            assert m.mmio[0x03000008]==0,'The shared SRAM is routed to the NPU at READY'

            assert v(address+app['ready'])==1 and v(address+app['worker_admitted'])==1
            assert not v(sym['a733_npu_application_failed'])
            assert m.mmio[0x020022a0]==0xf8104600 and m.mmio[0x02002b04]&0xf0001==0xf0001 and m.mmio[0x02002b00]&0x8700001f==1<<31
            assert m.mmio[0x07064000]&15==8 and m.mmio[0x07064008]&15==8
            if mode=='ready':
                # Domain 4 commanded ON before the first NPU clock write.
                assert ppu_writes and all(0x07064000<=x[0]<0x07065000 for x in ppu_writes),ppu_writes
                assert m.writes.index(ppu_writes[0])<m.writes.index(npu_writes[0]),(ppu_writes,npu_writes)
            else:
                # An inherited ON domain is adopted without a PCK600 write.
                assert not ppu_writes,ppu_writes
            token=startup+off('a733_npu_startup','clock')
            assert call('a733_clock_token_status',registry,token)==0
        else:
            # Wrong DCDC2 stops at the supply wait, and a missing LOCK at
            # enable, after domain 4 is powered.
            expected={'wrong-rail':'A733_NPU_STARTUP_SUPPLY','lock-timeout':'A733_NPU_STARTUP_ENABLE'}[mode]
            assert v(stage)==stage_of[expected],(mode,v(stage))
            assert v(failed) and v(sym['a733_npu_application_failed']) and not v(address+app['ready'])
            if mode=='lock-timeout':
                # Domain 4 was commanded ON before the enable table ran; the
                # output gate stayed closed and the SRAM stayed with the CPU.
                assert npu_writes and not m.mmio[0x020022a0]&(1<<27) and ppu_writes
                assert m.writes.index(ppu_writes[0])<m.writes.index(npu_writes[0]),(ppu_writes,npu_writes)
                assert m.mmio[0x03000008]==2
            else:
                # Refused before the first NPU write.
                assert not npu_writes and not ppu_writes,(npu_writes,ppu_writes)
        results[mode]={'bootstrap_polls':polls,'stage':v(stage),'npu_writes':len(npu_writes),'ppu_writes':len(ppu_writes),
            'rail_identity':q(startup+off('a733_npu_startup','rail')+off('a733_rail_token','id')),
            'retained':mode not in ('ready','domain-on')}
        save(out/('case-'+mode+'.json'),{'image_sha256':sha(image),'runner_sha256':sha(Path(__file__)),
            'model_sha256':sha(Path(model.__file__)),'result':results[mode]})
    save(out/'session-layout.json',{'exported':list(exported),
        'typed_layouts':layouts,'fixture_sha256':sha(out/'modeled-session.bin')})
    return {'passed':True,'admission_refusals':refused+['watchdog-contract','changed-session'],
        'cases':results,'native_translation_instructions':m.translations,
        'limits':'System registers, AT/cache effects, immutable boot evidence, PLL lock, PPU acknowledgement and PMIC bus responses are modeled. Native constructors, the power service with its supply verification and domain-4 row, common leases/mutexes, the startup sequencer and service activation execute. Worker commands and numerical inference do not execute. Private repeated model-buffer SHA uses hashlib over actual bytes. No physical readiness, scheduler qualification, or old workload replay.'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('freertos','tinyusb','lld','artifacts'):p.add_argument('--'+name,type=Path)
    p.add_argument('--evidence',type=Path,required=True);p.add_argument('--image',type=Path)
    p.add_argument('--build-only',action='store_true')
    p.add_argument('--case');args=p.parse_args();out=args.evidence.resolve()
    out.mkdir(parents=True,exist_ok=False)
    try:
        image=args.image.resolve() if args.image else build(args,out)
        shutil.copy2(__file__,out/'executed-check.py')
        if args.build_only:print('BUILT '+str(image));return
        save(out/'result.json',execute(image,out,args.case))
    except Exception as error:
        (out/'failure.txt').write_text(repr(error)+'\n');raise
    print('PASS supervised native supply verification, startup sequencer and retained refusals')
if __name__=='__main__':main()
