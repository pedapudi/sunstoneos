#!/usr/bin/env python3
"""Check version-2 launch admission and exact SRAM instructions without devices."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_SP, UC_ARM_REG_LR

FLAG, ENTRY, ALT, INPUT = 0x070501c0, 0x070501c4, 0x070501d4, 0x03000024
CONFIG, MODE, OUTPUT, FEED, RTC = 0x02050010, 0x02050014, 0x02050018, 0x0205000c, 0x07090108
RVBAR, CONTRACT, REPORT = 0x08001004, 0x42004000, 0x52d00


def instruction_cases(bundle, output):
    image = (bundle / 'launch.bin').read_bytes()
    prepared = json.loads((bundle / 'prepared.json').read_text())
    with (bundle / 'launch.elf').open('rb') as stream:
        elf = ELFFile(stream)
        park = next(s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name == 'a733_smp_launch_park')
    assert 0 < len(image) <= 0xc00

    def check(name, reason=0, *, initial=None, ignored=None, sysreg=None, mutation=None, late=None, start=100000000, thumb_return=False):
        cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM); cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
        for base, size in [(0x50000, 0x30000), (0x41000000, 0x1000000), (0x42004000, 0x1000),
                           (0x02050000, 0x1000), (0x03000000, 0x1000), (0x07050000, 0x1000),
                           (0x07090000, 0x1000), (0x08001000, 0x1000)]: cpu.mem_map(base, size)
        cpu.mem_write(0x52000, image); cpu.mem_write(0x52c00, (bundle / 'request.bin').read_bytes())
        cpu.mem_write(CONTRACT, (bundle / 'contract.bin').read_bytes())
        for segment in prepared['segments']: cpu.mem_write(segment['address'], (bundle / segment['name']).read_bytes())
        cpu.mem_write(0x52ff0, b'A' * 16)
        if mutation:
            address, value = mutation; cpu.mem_write(address, struct.pack('<Q', value))
        registers = {FLAG:0, ENTRY:0x12340000, ALT:0, INPUT:0, CONFIG:0x22, MODE:0xa2, OUTPUT:7, RTC:0, RVBAR:0, RVBAR+4:1}
        registers.update(initial or {})
        system = {0xee110f10:0, 0xee110f11:0, 0xee100fb0:0x81000000, 0xee1e0f10:24000000, 0xee1c0f50:0}
        system.update(sysreg or {})
        writes, stopped, ticks = [], [], []
        steps = 0
        saved = {UC_ARM_REG_R0+i:0x12340000+i*0x101 for i in range(13)}
        saved.update({UC_ARM_REG_SP:0x70000, UC_ARM_REG_LR:0x60000+int(thumb_return)})
        cpu.reg_write(UC_ARM_REG_CPSR, 0xa80f0053)
        saved[UC_ARM_REG_CPSR] = cpu.reg_read(UC_ARM_REG_CPSR)
        for register,value in saved.items(): cpu.reg_write(register,value)

        def read(uc, access, address, size, value, unused):
            if address < 0x02000000 or address >= 0x41000000: return
            assert address in registers and size == 4, (name, hex(address), size)
            uc.mem_write(address, struct.pack('<I', registers[address]))

        def write(uc, access, address, size, value, unused):
            if address < 0x02000000:
                assert REPORT <= address < REPORT+128 or 0x53000 <= address < 0x54000 or 0x70000-60 <= address < 0x70000, (name, hex(address))
                return
            if address >= 0x41000000:
                assert CONTRACT <= address < CONTRACT+8 or CONTRACT+80 <= address < CONTRACT+112, (name, hex(address))
                if address == CONTRACT:
                    assert registers[MODE] == 0x41 and ticks and len(ticks) >= 3
                    values = struct.unpack('<4Q', uc.mem_read(CONTRACT+80, 32))
                    assert values == (ticks[0], ticks[0]+24000000, ticks[0]+48000000, ticks[0]+96000000)
                return
            assert address in (FLAG, ENTRY, CONFIG, MODE, OUTPUT, FEED, RTC, RVBAR, RVBAR+4) and size == 4
            writes.append((address, value))
            if address == FEED:
                assert value == 0x14af; return
            if address in (CONFIG, MODE, OUTPUT):
                assert value >> 16 == 0x16aa; value &= 0xffff
            if address == MODE:
                assert value == 0x41 and registers[FLAG] == 0xfa50392f and registers[ENTRY] == 0x20
                assert registers[RTC] == 0x5aa5a55a and registers[CONFIG] == 1 and registers[OUTPUT] == 2
            if address in (RVBAR, RVBAR+4):
                assert struct.unpack('<Q', uc.mem_read(CONTRACT, 8))[0] == 0x4153373343414331
            if address != ignored: registers[address] = value

        def instruction(uc, address, size, unused):
            nonlocal steps
            steps += 1
            if address == 0x60000:
                stopped.append('return'); uc.emu_stop(); return
            if address == park:
                stopped.append('park'); uc.emu_stop(); return
            word = struct.unpack('<I', uc.mem_read(address, 4))[0]
            if word & 0xffff0fff in system:
                uc.reg_write(UC_ARM_REG_R0 + ((word >> 12) & 15), system[word & 0xffff0fff])
                uc.reg_write(UC_ARM_REG_PC, address+4)
            elif word & 0xfff00fff == 0xec500f0e:
                value = start + steps if not ticks else ticks[0] + 100*len(ticks)
                if late and len(ticks) == late[0]: value = ticks[0] + late[1]
                ticks.append(value)
                uc.reg_write(UC_ARM_REG_R0 + ((word >> 12)&15), value & 0xffffffff)
                uc.reg_write(UC_ARM_REG_R0 + ((word >> 16)&15), value >> 32)
                uc.reg_write(UC_ARM_REG_PC,address+4)
            elif word == 0xee0c0f50:
                assert uc.reg_read(UC_ARM_REG_R0) == 3 and registers[RVBAR] == 0x41000000 and registers[RVBAR+4] == 0
                assert len(ticks) == 5 and ticks[-1]-ticks[0] < 24000
                assert uc.reg_read(UC_ARM_REG_CPSR) & 0x1c0 == 0x1c0
                stopped.append('rmr'); uc.emu_stop()

        cpu.hook_add(UC_HOOK_MEM_READ, read); cpu.hook_add(UC_HOOK_MEM_WRITE, write); cpu.hook_add(UC_HOOK_CODE, instruction)
        cpu.emu_start(0x52000, 0x52c00, count=20000)
        report = struct.unpack('<3Q', cpu.mem_read(REPORT, 24))
        clean_return = reason in (1,2,3,4,5,6,13)
        expected = 'return' if clean_return else 'rmr' if reason == 12 else 'park'
        assert stopped == [expected] and report[0] == reason, (name, stopped, report, ticks)
        assert sum(address == FEED for address, _ in writes) <= 1
        assert cpu.mem_read(0x52ff0, 16) == b'A'*16
        if reason in (0, 1, 2, 3, 4, 5, 6): assert not writes, (name, writes)
        if reason == 12: assert report[1:3] == (ticks[0], ticks[2])
        if clean_return:
            for register,value in saved.items():
                if register == UC_ARM_REG_CPSR: value = (value & ~0x20) | (int(thumb_return)<<5)
                assert cpu.reg_read(register) == value, (name,register,hex(cpu.reg_read(register)),hex(value))
            full = struct.unpack('<16Q',cpu.mem_read(REPORT,128))
            request = struct.unpack('<16Q',cpu.mem_read(0x52c00,128))
            assert full[3:7] == (request[3],request[4],request[6],0x4153373352454631) and not any(full[7:])
            assert not writes and report[1:3] == (0,0)
        return {'case':name, 'reason':reason, 'stop':expected, 'mmio_writes':len(writes), 'counter_reads':ticks}

    cases = [check('v2 publication and bounded transition',12)]
    mutations = [('legacy contract',CONTRACT+8,1),('layout ID',CONTRACT+176,2),
                 ('reservation identity',CONTRACT+184,0),('WB start',CONTRACT+192,0x48001000),
                 ('WB end',CONTRACT+200,0x78001000),('NC start',CONTRACT+208,0x78001000),
                 ('NC end',CONTRACT+216,0x79001000),('request layout',0x52c60,2),
                 ('request reservation',0x52c68,0),('request unused word',0x52c70,1)]
    for name,address,value in mutations: cases.append(check(name,3,mutation=(address,value)))
    cases.append(check('v1 request cannot launch v2 image',1,mutation=(0x52c00,0x415337334c4e4331)))
    result = {'physical_access':False,'image_sha256':hashlib.sha256(image).hexdigest(),
              'check_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'cases':cases,
              'limits':'MMIO and counter model only. No oscillator, reset, interconnect liveness, retention, AArch64 entry, or scheduler qualification.'}
    output.write_text(json.dumps(result,indent=2)+'\n'); print(f'{len(cases)} launcher instruction cases passed.')


def main():
    import copy
    import importlib.util
    import os
    import shutil
    import sys
    import elftools
    import unicorn
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--image-manifest',type=Path,required=True)
    parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--clang',default='clang-21')
    parser.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    args=parser.parse_args();root=Path(__file__).resolve().parents[1]
    evidence=args.evidence;evidence.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('device_host',root/'tools/check-a733-device-launch.py')
    host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
    inputs={}
    paths=[Path(__file__),root/'tools/check-a733-device-launch.py',root/'tools/check-a733-smp.py',
           root/'checks/a733-smp-launch.py',args.image,args.image_manifest]
    paths+=list((root/'platform/a733-device-launch').iterdir())
    paths+=list((root/'platform/a733-smp-observe').glob('launch.*'))
    tools=[Path(sys.executable).resolve(),Path(shutil.which(args.clang)).resolve(),args.lld.resolve(),
           Path('/usr/lib/llvm-21/bin/llvm-objcopy').resolve()]
    emulator=[Path(unicorn.__file__),Path(elftools.__file__)]
    emulator+=list(Path(unicorn.__file__).parent.rglob('libunicorn.so*'))
    paths+=emulator
    for path in paths:
        data=path.read_bytes();identity=hashlib.sha256(data).hexdigest();dest=evidence/'inputs'/identity
        dest.parent.mkdir(exist_ok=True);dest.write_bytes(data)
        inputs[str(path.resolve())]=identity
    # Exact old entry/linker bytes retain their prior architectural evidence.
    for name in ('launch.S','launch.ld'):
        assert (root/'platform/a733-device-launch'/name).read_bytes()==(root/'platform/a733-smp-observe'/name).read_bytes()
    host.save_json(evidence/'inputs.json',{'inputs':inputs,'physical_access':False,
                   'tools':{str(path):host.digest(path) for path in tools},
                   'emulator_versions':{'unicorn':unicorn.__version__,'pyelftools':elftools.__version__}})
    bundle=evidence/'bundle'
    host.prepare(args.image,args.image_manifest,bundle,args.clang,args.lld,0x123456789)
    prepared=host.validate_bundle(bundle)
    assert sum(s['memory_bytes'] for s in prepared['reservations']) >= 784*1024*1024
    assert all(s['address']<0x42000000 for s in prepared['segments'])
    assert sum((bundle/s['name']).stat().st_size for s in prepared['segments'])<0x1000000
    refusals=[]
    def refused(name,operation):
        try:operation()
        except (ValueError,KeyError,struct.error):refusals.append(name)
        else:raise AssertionError('Unexpected admission: '+name)
    # Change only newly admitted geometry; keep the original image immutable.
    raw=args.image.read_bytes();phoff=struct.unpack_from('<Q',raw,32)[0]
    phsize,phnum=struct.unpack_from('<HH',raw,54)
    pool=next(phoff+i*phsize for i in range(phnum)
              if struct.unpack_from('<I',raw,phoff+i*phsize)[0]==1 and
              struct.unpack_from('<Q',raw,phoff+i*phsize+16)[0]==0x48000000)
    for name,offset,value in [('pool file bytes',pool+32,1),('pool shortened',pool+40,0x200000),
                              ('pool shifted',pool+16,0x48001000)]:
        data=bytearray(raw);struct.pack_into('<Q',data,offset,value)
        path=evidence/(name.replace(' ','-')+'.elf');path.write_bytes(data)
        refused(name,lambda path=path:host.image_parts(path))
    legacy=json.loads(args.image_manifest.read_text());legacy['contract_version']=1
    refused('v1 image manifest',lambda:host.validate_image_manifest(legacy,args.image))
    # Policy fixtures exist only in memory. They are never emitted as physical
    # qualification or a launchable queue record.
    binding=host.session_binding(prepared,[1,2,3,1000,1000],'model-port','/model/port')
    records={name:{'schema':'a733-device-memory-condition-v2','condition':name,'binding':binding,
                   'physical_access':True,'established':True,'observations':[{'model':name}]}
             for name in host.CONDITIONS}
    admission={'schema':'a733-device-memory-session-v2','binding':binding,
               'physical_qualification':True,'ownership_retained':True,
               'conditions':{name:{'model':name} for name in host.CONDITIONS}}
    old_read,old_artifact=host.read_record,host.artifact;observed=[]
    host.read_record=lambda reference:records[reference['model']]
    host.artifact=lambda reference:observed.append(reference['model'])
    validate=lambda value:host.validate_memory(value,prepared,[1,2,3,1000,1000],'model-port','/model/port')
    try:
        validate(admission);assert set(observed)==host.CONDITIONS
        refused('missing physical admission',lambda:validate({}))
        for key,value in [('epoch',0),('generation',0),('boot_lease',0),('layout',2),('reservation',0),
                          ('build_id','00'*32),('image_sha256','00'*32),('ranges',[[0x41000000,0x42200000]])]:
            wrong=copy.deepcopy(admission);wrong['binding'][key]=value
            refused('wrong '+key,lambda wrong=wrong:validate(wrong))
        wrong=copy.deepcopy(admission);wrong['binding']['target']['instance']=[9]
        refused('wrong live instance',lambda:validate(wrong))
        for name in sorted(host.CONDITIONS):
            wrong=copy.deepcopy(admission);del wrong['conditions'][name]
            refused('missing '+name,lambda wrong=wrong:validate(wrong))
        record=records['dual-pas-dram'];record['physical_access']=False
        refused('modeled physical condition',lambda:validate(admission));record['physical_access']=True
        record['binding']=dict(binding,epoch=0)
        refused('stale condition identity',lambda:validate(admission));record['binding']=binding
        record['observations']=[]
        refused('condition without observations',lambda:validate(admission));record['observations']=[{'model':'dual-pas-dram'}]
        # Actual shared preload and launch operate against a finite fake FEL
        # endpoint. The model never maps, reads or writes the NOLOAD pools.
        memory={};calls=[];executed=[];readiness=[];session={'launch_attempted':False,'complete':False}
        capture=evidence/'transport';capture.mkdir()
        def transaction(*arguments,pass_fds=()):
            calls.append(list(map(str,arguments)))
            if arguments[0]=='readl':return ' '.join('0' for _ in range(len(arguments)//2))
            if arguments[0]=='read':
                _,address,length,path=arguments;address=int(address,0)
                Path(path).write_bytes(memory.get(address,bytes(length)));return ''
            if arguments[0]=='write':
                _,address,path,op,again,length,destination=arguments;address=int(address,0)
                assert address<0x48000000 and op=='read' and int(again,0)==address
                assert pass_fds and str(path).startswith('/proc/self/fd/')
                data=Path(path).read_bytes();assert len(data)==length
                memory[address]=data;Path(destination).write_bytes(data);return ''
            assert arguments==('exe','0x52000');executed.append(arguments);return ''
        def ready():validate(admission);readiness.append(True)
        ready()
        host.launch_once(transaction,lambda:None,bundle,prepared,capture,session,lambda:None,ready)
        assert len(executed)==1 and len(readiness)==2 and session['scheduler_result']=='unobserved' and not session['complete']
        assert all(int(c[1],0)<0x48000000 for c in calls if c[0] in ('write','read'))
        held=[]
        host.base.stop_after_failure(session,lambda:None,lambda:held.append('quarantine'),
                                     ValueError('modeled lost transport'),hold=lambda:held.append('ownership'))
        assert held==['quarantine','ownership']
        host.save_json(evidence/'transport.json',{'calls':calls,'physical_access':False,
                        'executions':len(executed),'ownership_retained':True})
    finally:host.read_record,host.artifact=old_read,old_artifact
    instruction_cases(bundle,evidence/'instruction.json')
    # Check the immutable input set after execution, without rerunning workloads.
    assert all(host.digest(Path(path))==identity for path,identity in inputs.items())
    artifacts={str(p.relative_to(evidence)):host.digest(p) for p in evidence.rglob('*') if p.is_file()}
    host.save_json(evidence/'results.json',{'result':'PASS','physical_access':False,'inputs':inputs,
                  'artifacts':artifacts,'refusals':refusals,
                  'limits':'Policy records were private in-memory models. No physical qualification, board access, reset, DRAM, coherence or scheduler result.'})
    print('Version-2 image policy, finite transfers and instruction admission passed.')


if __name__=='__main__':main()
