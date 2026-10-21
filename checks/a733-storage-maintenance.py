#!/usr/bin/env python3
"""Build the actual storage profile and check only its new launch/session paths."""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15,UC_ARM_REG_R0,UC_ARM_REG_PC,UC_ARM_REG_CPSR,UC_ARM_REG_SP,UC_ARM_REG_LR
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X19,UC_ARM64_REG_X20,UC_ARM64_REG_X21,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def build(e,freertos,tinyusb):
    source=e/'source';source.mkdir()
    paths=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines()
    for name in paths:
        p=ROOT/name
        if p.is_file():
            q=source/name;q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
    external=e/'dependencies';external.mkdir()
    f=external/'freertos';f.mkdir()
    for name in ('tasks.c','queue.c','list.c'):shutil.copy2(freertos/name,f/name)
    shutil.copytree(freertos/'include',f/'include');shutil.copytree(freertos/'portable/GCC/ARM_CR82',f/'portable/GCC/ARM_CR82')
    t=external/'tinyusb';shutil.copytree(tinyusb/'src',t/'src')
    compiler=Path(shutil.which('clang-21')).resolve();linker=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
    import unicorn,elftools,capstone
    tools=[compiler,linker.resolve(),Path('/usr/lib/llvm-21/bin/llvm-objcopy').resolve(),Path(sys.executable).resolve(),
           Path(unicorn.__file__),Path(elftools.__file__),Path(capstone.__file__)]
    tools += [p for lib in (unicorn,capstone) for p in Path(lib.__file__).parent.rglob("*.so*") if p.is_file()]
    (e/'tools.json').write_text(json.dumps({str(p):sha(p) for p in tools},indent=2)+'\n')
    (e/'source-inputs.json').write_text(json.dumps({str(p.relative_to(e)):sha(p) for parent in (source,external) for p in parent.rglob('*') if p.is_file()},indent=2)+'\n')
    toolchain=e/'toolchain.cmake';toolchain.write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
    commands=[]
    def run(cmd,label):
        commands.append(list(map(str,cmd)));r=subprocess.run(cmd,capture_output=True,text=True)
        (e/(label+'.log')).write_text(r.stdout+r.stderr);(e/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');r.check_returncode()
    b=e/'build'
    run(['cmake','-G','Unix Makefiles','-S',str(source/'platform/a733-storage-maintenance'),'-B',str(b),
         f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DFREERTOS_DIR={f}',f'-DTINYUSB_DIR={t}','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'],'configure')
    run(['cmake','--build',str(b),'--target','sunstone-a733-storage-maintenance','-j4'],'build')
    image=b/'sunstone-a733-storage-maintenance.elf'
    inputs={}
    for dep in [*b.rglob('*.o.d'),*b.rglob('*.obj.d')]:
        words=dep.read_text().replace('\\\n',' ').split(':',1)[1].split()
        for word in words:
            p=Path(word)
            if p.is_file():inputs[str(p.resolve())]=sha(p)
    # Assembly sources may have no emitted depfile; compile_commands identifies them.
    for unit in json.loads((b/'compile_commands.json').read_text()):
        p=Path(unit['file']);inputs[str(p.resolve())]=sha(p)
    (e/'compiled-dependencies.json').write_text(json.dumps(inputs,indent=2)+'\n')
    manifest=e/'image-manifest.json';manifest.write_text(json.dumps(dict(target='sunstone-a733-storage-maintenance',
        execution_profile='storage-maintenance-1',image_kind='runtime',final_elf=sha(image),
        build_id=(b/'storage-maintenance/build-id.txt').read_text().strip(),compiled_dependencies=inputs),indent=2)+'\n')
    return image,manifest


def fixture_store(host,e):
    api=host.store_api
    grant=dict(mode=0,kind=1,host=0,nonce=bytes(range(1,33)).hex(),cid=[1,2,3,4],sectors=1024,first=128,blocks=2,
               deadline_us=30000000,backup='00'*32,candidate='00'*32,allocation='45'*32,recovery='67'*32)
    store=api.ProtectedDirectory(e/'storage',create=True)
    raw=api.grant_encode(grant);fd=store.create('grant.bin');api.write_all(fd,raw);store.seal('grant.bin',fd,256,api.digest(raw))
    store.record('capture.json',dict(grant=grant,grant_sha256=hashlib.sha256(raw).hexdigest(),fixture=True))
    config=dict(minimum_reload_us=3900000,maximum_recovery_us=5000000,guard_us=1000,reset_by_us=40000000,
                rtc=[0x11,0x22,0x33],topology=17,topology_artifact='89'*32,usb_supply_uv=[3300000,3300000,1200000,900000],
                owners=[101,102,103],fabric=[0,0,0],fabric_hz=[24000000]*3,mmc_voltage_mask=0xff8000,usb_cold_reference=1,
                mmc_expected=[[0,0,0]]*12,mmc_desired=[[2,1,2]]*7+[[0,0,0]]*5,pmic_expected=[[0,0,0]]*2)
    (e/'fixture-configuration.json').write_text(json.dumps(config,indent=2)+'\n')
    return store,grant


def launch_cases(bundle,e):
    prepared=json.loads((bundle/'prepared.json').read_text());binary=(bundle/'launch.bin').read_bytes()
    with (bundle/'launch.elf').open('rb') as f:
        elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    contract,session,report=0x42004000,0x41f00000,0x54100
    output=[];good=None
    def run(label,reason,mutation=None,late=False,rtc_mismatch=False):
        nonlocal good
        u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
        for address,size in [(0x50000,0x30000),(0x41000000,0x1200000),(0x02050000,4096),(0x03000000,4096),
                             (0x07050000,4096),(0x07090000,4096),(0x08001000,4096)]:u.mem_map(address,size)
        u.mem_write(0x52000,binary);u.mem_write(0x54000,(bundle/'request.bin').read_bytes());u.mem_write(contract,(bundle/'contract.bin').read_bytes())
        u.mem_write(session,(bundle/'session.bin').read_bytes())
        for s in prepared['segments']:u.mem_write(s['address'],(bundle/s['name']).read_bytes())
        if mutation:u.mem_write(mutation[0],struct.pack('<Q',mutation[1]))
        mmio={0x070501c0:0,0x070501c4:0x12340000,0x070501d4:0,0x03000024:0,
              0x02050010:0x22,0x02050014:0xa2,0x02050018:7,0x07090108:0,0x08001004:0,0x08001008:1,
              0x07090000:0x11,0x07090060:0x22,0x07090160:0x33}
        if rtc_mismatch:mmio[0x07090160]=0x34
        system={0xee110f10:0,0xee110f11:0,0xee100fb0:0x81000000,0xee1e0f10:24000000,0xee1c0f50:0}
        writes=[];ticks=[];stop=[];steps=0
        u.reg_write(UC_ARM_REG_CPSR,0xa80f0053)
        saved={UC_ARM_REG_R0+i:0x12340000+i*0x101 for i in range(13)}
        saved.update({UC_ARM_REG_SP:0x70000,UC_ARM_REG_LR:0x60000,UC_ARM_REG_CPSR:u.reg_read(UC_ARM_REG_CPSR)})
        for reg,value in saved.items():u.reg_write(reg,value)
        def read(uc,access,address,size,value,unused):
            if 0x02000000<=address<0x41000000:
                assert address in mmio and size==4,(label,hex(address));uc.mem_write(address,struct.pack('<I',mmio[address]))
        def write(uc,access,address,size,value,unused):
            if address<0x02000000:
                assert report<=address<report+128 or 0x55000<=address<0x56000 or 0x70000-60<=address<0x70000,(label,hex(address));return
            if address>=0x41000000:
                assert contract<=address<contract+8 or contract+80<=address<contract+112 or session<=address<session+8 or session+640<=address<session+656,(label,hex(address))
                if address==contract:assert struct.unpack('<Q',uc.mem_read(session,8))[0]==0x4153373353545331
                return
            assert address in (0x070501c0,0x070501c4,0x07090108,0x02050010,0x02050014,0x02050018,0x0205000c,0x08001004,0x08001008)
            writes.append((address,value))
            if address==0x0205000c:assert value==0x14af;return
            if address in (0x02050010,0x02050014,0x02050018):assert value>>16==0x16aa;value&=65535
            mmio[address]=value
        def insn(uc,address,size,unused):
            nonlocal steps
            steps+=1
            if address in (0x60000,symbols['a733_smp_launch_park']):stop.append('return' if address==0x60000 else 'park');uc.emu_stop();return
            word=struct.unpack('<I',uc.mem_read(address,4))[0]
            if word&0xffff0fff in system:
                uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),system[word&0xffff0fff]);uc.reg_write(UC_ARM_REG_PC,address+4)
            elif word&0xfff00fff==0xec500f0e:
                value=100000000+steps if not ticks else ticks[-1]+100
                if late and any(a==0x0205000c for a,_ in writes):value+=24000
                ticks.append(value);uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),value&0xffffffff)
                uc.reg_write(UC_ARM_REG_R0+((word>>16)&15),value>>32);uc.reg_write(UC_ARM_REG_PC,address+4)
            elif word==0xee0c0f50:stop.append('rmr');uc.emu_stop()
        u.hook_add(UC_HOOK_MEM_READ,read);u.hook_add(UC_HOOK_MEM_WRITE,write);u.hook_add(UC_HOOK_CODE,insn)
        u.emu_start(0x52000,0x54000,count=40000)
        observed=struct.unpack('<Q',u.mem_read(report,8))[0];assert observed==reason,(label,observed,stop,ticks)
        assert sum(a==0x0205000c for a,_ in writes)<=1
        if reason==3:
            assert stop==['return'] and not writes
            assert all(u.reg_read(r)==v for r,v in saved.items()),label
        elif reason==12:
            assert stop==['rmr'];raw=bytes(u.mem_read(session,704));c=bytes(u.mem_read(contract,224))
            before,after=struct.unpack_from('<2Q',raw,640);assert before<after and after-before<24000
            assert struct.unpack_from('<4Q',c,80)==(before,before+24000000,30000000*24,40000000*24)
            good=(raw,c,after+100);(e/'observed-session.bin').write_bytes(raw);(e/'observed-contract.bin').write_bytes(c)
        else:assert stop==['park'] and not struct.unpack('<Q',u.mem_read(contract,8))[0]
        output.append(dict(case=label,reason=reason,stop=stop,writes=writes,counter=ticks))
    run('actual session publication and observed initial reload',12)
    for label,offset,value in [('session identity',32,0),('reset overflow',136,2**64-1),('guard reserve',128,3900000),
                               ('inherited reload',640,1),('grant recovery mismatch',360,0),('grant deadline overflow',256,2**64-1)]:
        run(label,3,(session+offset,value))
    run('RTC source mismatch before mutation',3,rtc_mismatch=True)
    run('late initial reload retains unpublished contract',9,late=True)
    (e/'launcher-results.json').write_text(json.dumps(output,indent=2)+'\n');return good


class Native:
    def __init__(self,image):
        from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
        from unicorn import arm64_const
        self.constants=arm64_const;self.decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
        self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x41000000,0x1200000);self.u.mem_map(0x43000000,4096)
        with image.open('rb') as f:
            elf=ELFFile(f);self.symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            for s in elf.iter_segments():
                if s['p_type']=='PT_LOAD' and s['p_filesz']:self.u.mem_write(s['p_vaddr'],s.data())
            self.layout={}
            for cu in elf.get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    name=die.attributes.get('DW_AT_name')
                    if die.tag=='DW_TAG_structure_type' and name and 'DW_AT_byte_size' in die.attributes:
                        label=name.value.decode()
                        if label in ('a733_storage_native_boot','a733_maintenance_transfer','a733_storage_grant','a733_maintenance_lease'):
                            members={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value
                                for d in die.iter_children() if d.tag=='DW_TAG_member' and 'DW_AT_name' in d.attributes}
                            self.layout[label]=(die.attributes['DW_AT_byte_size'].value,members)
        self.sys={'sctlr_el3':0x30c51835,'tcr_el3':0x80823019,'mair_el3':0x44ff00,'scr_el3':0x535,
                  'ttbr0_el3':self.symbols['a733_cached_el3_l1'],'mpidr_el1':0x81000300,
                  'tcr_el1':0,'sctlr_el1':0,'esr_el3':0x17<<26,'far_el3':0,'elr_el3':0x41000000}
        self.writes=[];self.stop=None
        self.u.hook_add(UC_HOOK_MEM_WRITE,lambda uc,a,p,n,v,x:self.writes.append((p,n,v)))
        self.u.hook_add(UC_HOOK_CODE,self.step)
    def reg(self,name):
        if name=='xzr':return 0
        if name=='sp':return self.u.reg_read(UC_ARM64_REG_SP)
        return self.u.reg_read(getattr(self.constants,'UC_ARM64_REG_'+name.upper()))
    def setreg(self,name,value):self.u.reg_write(getattr(self.constants,'UC_ARM64_REG_'+name.upper()),value)
    def step(self,u,pc,size,unused):
        if pc==0x43000000:self.stop='return';u.emu_stop();return
        if pc==self.symbols.get('a733_smp_stop'):self.stop='primary-stop';u.emu_stop();return
        inst=next(self.decoder.disasm(bytes(u.mem_read(pc,4)),pc))
        if inst.mnemonic=='wfe':self.stop='park';u.emu_stop();return
        if inst.mnemonic=='mrs':
            reg,sysreg=inst.op_str.split(', ')
            assert sysreg in self.sys,(hex(pc),sysreg)
            self.setreg(reg,self.sys[sysreg]);u.reg_write(UC_ARM64_REG_PC,pc+4)
        elif inst.mnemonic=='msr':
            sysreg,reg=inst.op_str.split(', ')
            if reg.startswith('#'):value=int(reg[1:],0)
            else:value=self.reg(reg)
            self.sys[sysreg]=value;u.reg_write(UC_ARM64_REG_PC,pc+4)
    def call(self,name,args=()):
        self.stop=None;self.u.reg_write(UC_ARM64_REG_SP,0x41dff000);self.u.reg_write(UC_ARM64_REG_LR,0x43000000)
        for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
        self.u.emu_start(self.symbols[name],0x43000004,count=100000)
        assert self.stop,(name,'instruction bound')
        return self.u.reg_read(UC_ARM64_REG_X0)
    def q(self,address):return struct.unpack('<Q',self.u.mem_read(address,8))[0]


def native_cases(image,observed,e):
    raw,contract,now=observed;cases=[]
    n=Native(image);n.u.mem_write(0x41f00000,raw);n.u.mem_write(0x42004000,contract)
    assert n.call('a733_smp_contract_valid',(0x42004000,now))==1
    assert n.call('a733_storage_session_decode',(0x41e00000,0x41e04000,0x41f00000,now))==1
    bm=n.layout['a733_storage_native_boot'][1];tm=n.layout['a733_maintenance_transfer'][1];gm=n.layout['a733_storage_grant'][1]
    epoch,generation,lease=struct.unpack_from('<3Q',raw,24)
    assert n.q(0x41e00000+bm['identity'])==generation
    assert n.q(0x41e04000+tm['epoch'])==epoch and n.q(0x41e04000+tm['generation'])==generation and n.q(0x41e04000+tm['lease'])==lease
    assert n.q(0x41e04000+tm['storage'])==0x41e00000+bm['grant']
    assert n.q(0x41e00000+bm['grant']+gm['deadline_us'])==30000000
    before,after=struct.unpack_from('<2Q',raw,640)
    assert n.q(0x41e04000+tm['reload_before_us'])==before//24 and n.q(0x41e04000+tm['reload_after_us'])==after//24
    assert bytes(n.u.mem_read(0x41e04000+tm['image'],32))==raw[48:80]
    cases.append('actual ARM-produced session accepted by linked EL3 admission and native decoder')
    for label,offset,value in [('expired cutoff',256,now//24),('recovery mismatch',360,0),('backward observation',648,before-1)]:
        m=Native(image);bad=bytearray(raw);struct.pack_into('<Q',bad,offset,value);m.u.mem_write(0x41f00000,bytes(bad))
        m.u.mem_write(0x41e00000,b'\xa5'*4096);m.u.mem_write(0x41e04000,b'\x5a'*4096)
        assert m.call('a733_storage_session_decode',(0x41e00000,0x41e04000,0x41f00000,now))==0
        assert bytes(m.u.mem_read(0x41e00000,4096))==b'\xa5'*4096 and bytes(m.u.mem_read(0x41e04000,4096))==b'\x5a'*4096
        cases.append(label+' leaves outputs untouched')
    for state in (0,16,1):
        m=Native(image);ptr=m.call('a733_maintenance_native_lease');off=m.layout['a733_maintenance_lease'][1]['state']
        m.u.mem_write(ptr+off,struct.pack('<I',state));records=m.symbols['a733_cached_records']
        for c in range(8):
            values=[1,c,3,0,0,0,0,0,m.sys['sctlr_el3'],m.sys['ttbr0_el3'],0,0,0,0,0,0]
            m.u.mem_write(records+c*128,struct.pack('<16Q',*values))
        m.setreg('x18',8);m.writes=[];m.call('a733_cached_el3_fault',(4,123))
        assert m.stop=='park' and struct.unpack('<I',m.u.mem_read(ptr+off,4))[0]==state|4
        assert m.symbols['a733_cpu_entry_stacks']+3*4096<=m.reg('sp')<=m.symbols['a733_cpu_entry_stacks']+4*4096
        assert m.call('a733_maintenance_alive',(ptr,))==0
        # Author's actual bind-interleaving fixture proves publication races;
        # this composition checks the linked fatal hook reaches the same atomic.
        cases.append(f'secondary fatal hook preserves revoke for lifecycle state {state}')
    for label,alter in [('missing adoption',None),('live translation mismatch','tcr_el3')]:
        m=Native(image);records=m.symbols['a733_cached_records']
        for c in range(8):m.u.mem_write(records+c*128,struct.pack('<16Q',1,c,3,0,0,0,0,0,m.sys['sctlr_el3'],m.sys['ttbr0_el3'],0,0,0,0,0,0))
        if alter:m.sys[alter]=0
        else:m.u.mem_write(records+64,bytes(8))
        m.setreg('x18',8);m.writes=[];m.call('a733_cached_el3_fault',(4,123))
        assert m.stop=='park' and all(0x42000000<=address<0x42200000 for address,_,_ in m.writes),m.writes
        cases.append(label+' parks without ordinary RAM or stack writes')
    (e/'native-results.json').write_text(json.dumps(cases,indent=2)+'\n');return cases


def host_case(host,bundle,p,store,grant,e):
    import usb.core
    events=[];memory={};block=bytes(range(256))*4
    api=host.store_api
    class Device:
        bcdDevice=0x0102
        short=False
        def write(self,endpoint,packet,timeout):
            assert endpoint==2 and timeout==1500;events.append('OUT');self.packet=bytes(packet);return len(packet)
        def read(self,endpoint,size,timeout):
            assert endpoint==0x81
            magic,sequence,command,n=struct.unpack_from('<4I',self.packet);assert (magic,command,n)==(0x53534f53,0x53555044,len(self.packet)-16)
            body=self.packet[16:];assert body[:32]==bytes.fromhex(grant['nonce'])
            seq,op,offset=struct.unpack_from('<IIQ',body,32);assert sequence==seq
            data=api.grant_encode(grant)+struct.pack('<I',8) if op==1 else block if op==2 else b''
            digest=hashlib.sha256(block).digest() if op in (2,3,5) else bytes(32)
            response=struct.pack('<IIQII',seq,0,offset,len(data),0)+digest+data
            events.append('IN');return b'x' if self.short else struct.pack('<4I',magic,seq,0xd3555044,len(response))+response
    class Access:
        def identity(self,node):return {'fixture':'current'}
        def require_session(self,node):pass
        def quarantine_instance(self,*args,**kwargs):events.append('quarantine')
    device=Device();access=Access();saved=usb.core.find
    usb.core.find=lambda **kw:device
    def exchange():
        events.append('runtime');return api.USBExchange(access,Path('/dev/bus/usb/003/999'),{'fixture':'current'},grant)
    def transaction(*args,pass_fds=()):
        if args==('readl','0x02050014'):return '00000000'
        if len(args)==8:return '0 0 0 0'
        if args[0]=='exe':assert args==('exe','0x52000');events.append('exe');return ''
        if args[0]=='read':
            _,address,size,path=args;Path(path).write_bytes(memory.get(int(address,16),bytes([0xa5])*int(size))[:int(size)]);return ''
        assert args[0]=='write' and args[3]=='read' and len(pass_fds)==1
        assert fcntl.fcntl(pass_fds[0],fcntl.F_GET_SEALS)==15
        address=int(args[1],16);data=Path(args[2]).read_bytes();memory[address]=data;Path(args[6]).write_bytes(data[:int(args[5])]);return ''
    import fcntl
    record={};evidence=e/'host';evidence.mkdir()
    def save():host.save_json(evidence/'session.json',record)
    def rom():
        assert store.json('backup-complete.json') and record['storage_complete'];events.append('ROM')
    try:
        host.connected_launch(transaction,exchange,rom,lambda:host.validate_bundle(bundle,store),bundle,p,store,grant,evidence,record,save)
        assert events.count('exe')==1 and events[-1]=='ROM' and record['complete'] and record['completion_sha256']
        device.short=True;bad=api.Client(exchange(),grant)
        try:bad.identify();assert False
        except ValueError:pass
        count=events.count('OUT')
        try:bad.identify();assert False
        except ValueError:pass
        assert events.count('OUT')==count and events[-1]=='quarantine'
    finally:usb.core.find=saved
    result=dict(events=events,physical_access=False,limit='Synthetic USB endpoint; actual sealed preload, Client, USBExchange and durable backup lifecycle.')
    (e/'host-results.json').write_text(json.dumps(result,indent=2)+'\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence',type=Path,required=True);parser.add_argument('--freertos',type=Path,required=True);parser.add_argument('--tinyusb',type=Path,required=True)
    parser.add_argument('--build-only',action='store_true');parser.add_argument('--reuse-image',type=Path);parser.add_argument('--reuse-manifest',type=Path)
    a=parser.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=False)
    shutil.copy2(__file__,e/'executed-check.py')
    if a.reuse_image:image,manifest=a.reuse_image.resolve(),a.reuse_manifest.resolve()
    else:image,manifest=build(e,a.freertos,a.tinyusb)
    if a.build_only:print(image);return
    host=module('maintenance_launch',ROOT/'tools/check-a733-storage-launch.py')
    store,grant=fixture_store(host,e);store.close()
    p=host.prepare(image,manifest,e/'fixture-configuration.json',e/'storage',e/'bundle','clang-21',Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    with host.store_api.ProtectedDirectory(e/'storage') as store:
        p,grant=host.validate_bundle(e/'bundle',store)
        observed=launch_cases(e/'bundle',e)
        native_cases(image,observed,e)
        host_case(host,e/'bundle',p,store,grant,e)
        try:host.validate_session({},p,grant,{}, {},'fixture','fixture');assert False
        except ValueError:pass
    (e/'results.json').write_text(json.dumps(dict(physical_access=False,image_sha256=sha(image),launcher_sha256=sha(e/'bundle/launch.bin'),
        native_image=True,host_instruction_composition=True,limits='No physical recovery, coherence, storage, USB or board qualification; unchanged scheduler/driver workloads reused.'),indent=2)+'\n')
    print('PASS: connected image, launcher/session/fatal-hook instructions and protected host composition')
if __name__=='__main__':main()
