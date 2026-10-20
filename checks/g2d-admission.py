#!/usr/bin/env python3
"""Exercise G2D session bounds and authenticated private publication in AArch64."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
for name in ('image','session','contract','output'):
    parser.add_argument('--'+name,type=Path,required=True)
args=parser.parse_args()
args.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('g2d_admission_image',ROOT/'checks/storage-read-native.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
image=module.Image(args.image,layout_names=('a733_smp_contract','a733_g2d_session','a733_board_session'))
raw=args.session.read_bytes();contract=bytearray(args.contract.read_bytes())
assert len(raw)==904 and len(contract)==288
assert struct.unpack_from('<3Q',raw,592)==(0x4153373347324131,1,904)
assert contract[224:256]==hashlib.sha256(raw).digest() and contract[256:288]==raw[80:112]
struct.pack_into('<Q',contract,0,0x4153373347324331)
for name,value in dict(armed_at=100,startup_deadline=24000100,diagnostic_deadline=480000100,expires_at=480000100).items():
    struct.pack_into('<Q',contract,image.layouts['a733_smp_contract'][name],value)
contract=bytes(contract)
(args.output/'session.bin').write_bytes(raw);(args.output/'contract.bin').write_bytes(contract)
UPLOAD=0x41f00000
class Machine:
    def __init__(self,publish=1):
        self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.reg_write(UC_ARM64_REG_PSTATE,13)
        self.u.mem_map(0x41000000,0x1200000);self.u.mem_map(0x50000000,0x200000)
        for address,data in image.parts:
            if data:self.u.mem_write(address,data)
        self.u.mem_write(UPLOAD,raw);self.u.mem_write(0x42004000,contract)
        self.record=image.address('g2d_admission');self.u.mem_write(self.record,contract)
        self.publications=[]
        def ret(value):
            self.u.reg_write(UC_ARM64_REG_X0,value)
            self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
        def counter(*unused):ret(200)
        def publication(*unused):
            self.publications.append([self.u.reg_read(r) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2)])
            ret(publish)
        def stop(*unused):self.u.emu_stop()
        for name,callback in [('a733_smp_counter',counter),('a733_cpu_coherence_publish',publication)]:
            address=image.address(name);self.u.hook_add(UC_HOOK_CODE,callback,begin=address,end=address)
        self.u.hook_add(UC_HOOK_CODE,stop,begin=0x501ff000,end=0x501ff000)
    def call(self,name,*values):
        for index,value in enumerate(values):self.u.reg_write(UC_ARM64_REG_X0+index,value)
        self.u.reg_write(UC_ARM64_REG_SP,0x501fe000);self.u.reg_write(UC_ARM64_REG_LR,0x501ff000)
        self.u.emu_start(image.address(name),0x501ff004,count=1000000)
        assert self.u.reg_read(UC_ARM64_REG_PC)==0x501ff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
        return self.u.reg_read(UC_ARM64_REG_X0)
results=[]
machine=Machine()
assert machine.call('g2d_admitted_session')==0 and machine.call('g2d_admitted_session_digest')==0
assert machine.call('a733_smp_contract_valid',machine.record,200)==1
assert machine.call('g2d_board_prepare',machine.record)==1
assert machine.call('g2d_session_prepare',machine.record)==1
private=machine.call('g2d_admitted_session');digest=machine.call('g2d_admitted_session_digest')
assert private and digest and len(machine.publications)==2
machine.u.mem_write(UPLOAD,bytes(len(raw)))
assert bytes(machine.u.mem_read(private,len(raw)))==raw
assert bytes(machine.u.mem_read(digest,32))==hashlib.sha256(raw).digest()
assert machine.call('g2d_session_prepare',machine.record)==0
results.append('authenticated-board-and-G2D-private-publication-survives-upload-overwrite')
layout=image.layouts['a733_g2d_session']
def session(**fields):
    result=bytearray(raw)
    for name,value in fields.items():struct.pack_into('<I',result,layout[name],value)
    return bytes(result)
machine=Machine()
for name,fields,want in [
    ('maximum-strided-fill',dict(width=1280,height=720,stride=5120,op=1,dx=0,dy=0,w=1280,h=720,sx=0,sy=0),1),
    ('source-copy-corner',dict(width=3,height=2,stride=16,op=2,dx=2,dy=1,w=1,h=1,sx=2,sy=1,color=0),1),
    ('zero-width',dict(width=0),0),
    ('stride-over-capacity',dict(stride=5124),0),
    ('unaligned-stride',dict(stride=5119),0),
    ('destination-addition-overflow',dict(dx=0xffffffff,w=2),0),
    ('empty-rectangle',dict(w=0),0),
    ('source-addition-overflow',dict(op=2,sx=0xffffffff,color=0),0),
    ('fill-source-coordinate',dict(op=1,sx=1),0),
    ('copy-unused-color',dict(op=2,color=1),0),
    ('wrong-clock-parent',dict(module=3),0),
]:
    machine.u.mem_write(UPLOAD,session(**fields))
    assert machine.call('a733_g2d_session_valid',UPLOAD)==want,name
    results.append(name)
machine=Machine();machine.u.mem_write(UPLOAD+layout['fixture'],bytes(32))
assert machine.call('a733_smp_contract_valid',machine.record,200)==0
results.append('unsealed-input-identity-change')
machine=Machine()
changed=bytearray(contract)
for field in ('diagnostic_deadline','expires_at'):
    struct.pack_into('<Q',changed,image.layouts['a733_smp_contract'][field],1440000100)
machine.u.mem_write(machine.record,bytes(changed));machine.u.mem_write(0x42004000,bytes(changed))
assert machine.call('a733_smp_contract_valid',machine.record,200)==0
results.append('inherited-sixty-second-lifetime-refused')
machine=Machine(publish=-1)
assert machine.call('g2d_session_prepare',machine.record)==0
assert machine.call('g2d_admitted_session')==0 and machine.call('g2d_admitted_session_digest')==0
results.append('failed-publication-never-exposes-session')
report=dict(passed=True,physical_access=False,
    image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
    session_sha256=hashlib.sha256(raw).hexdigest(),cases=results,
    scope='Actual linked session/contract validation, hashing, private copies and getters. Physical authority, counter and cache publication are modeled. No provider, DMA, USB, secondary CPU or physical execution.')
(args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(results),'G2D admission/publication cases')
