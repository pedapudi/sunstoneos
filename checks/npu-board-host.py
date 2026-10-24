#!/usr/bin/env python3
"""Prepare the linked NPU board image with MODEL-NOT-PHYSICAL authority and check its admission.

The board prefix is the reviewed G2D native fixture's board session with its
qualification digests bound to a model note and an NPU-capable fabric record
(every fabric clock at 24 MHz). The NPU record, its four evidence records and
the admission are written here with physical_authority false; production
preparation must refuse them, and only physical_authority is substituted to
build the bundle. The image's actual record and contract validators,
private session copy and getter run under Unicorn on the prepared session,
and the prepared SRAM launcher runs in the AArch32 model of
checks/sram_launcher.py. The Python and C validators must agree on the
accepted record and on each refusal. No board, USB device or queue is used.
"""
import argparse,hashlib,importlib.util,json,struct,sys
from pathlib import Path
from unittest.mock import patch
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'checks'))
import harness,sram_launcher
BUILD=Path.home()/'.cache/sunstoneos/build/npu-board'
BOARD_FIXTURE=Path.home()/'.cache/sunstoneos/evidence/g2d-native-20261020/root-fixture-bound/session.bin'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
UPLOAD=0x41f00000
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,default=BUILD/'root/sunstone-a733-npu-board.elf')
    p.add_argument('--manifest',type=Path,default=BUILD/'root/npu-board-application/manifest.json')
    p.add_argument('--input',type=Path,default=BUILD/'session-fixtures/fixture_0_input_0.bin',help='conv8 input tensor')
    p.add_argument('--reference',type=Path,default=BUILD/'session-fixtures/fixture_0_reference_1.bin',help='conv8 reference output')
    p.add_argument('--clang',type=Path,default=Path('/usr/lib/llvm-21/bin/clang'))
    p.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    prep=module('npu_board_host_prepare',ROOT/'apps/npu-board/prepare.py')
    fixture=a.output/'MODEL-NOT-PHYSICAL-fixture';fixture.mkdir()
    image=a.image.read_bytes();ih=hashlib.sha256(image).hexdigest()
    catalogue=prep._image_parts(image)[3]
    target=dict(identity='npu-board-software-only-model',port='no-physical-port',image_sha256=ih,physical_authority=False,modeled=True)
    def record(name,value):
        data=json.dumps(value,sort_keys=True,indent=2).encode()+b'\n';path=(fixture/name).resolve();path.write_bytes(data)
        return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
    note=record('MODEL-NOT-PHYSICAL.json',dict(**target,purpose=__doc__))
    board=bytearray(BOARD_FIXTURE.read_bytes()[:592])
    for offset in (80,216,248,280):board[offset:offset+32]=bytes.fromhex(note['sha256'])
    struct.pack_into('<3I',board,488,0,0x80000000,0x80000000);struct.pack_into('<3I',board,500,24000000,24000000,24000000)
    owners=struct.unpack_from('<4Q',board,456);board_pck=struct.unpack_from('<I',board,540)[0]
    owner,command,supervisor=0x4e505542,0x434d4441,0x53555056;pck=(board_pck+4)%16
    assert owner not in owners
    def ddr(name,ranges):
        return record(name,dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,
            remote_writers_excluded=True,ranges=ranges,evidence=[note]))
    npu_ddr=ddr('npu-ddr.json',[[0x48000000,0x79000000]])
    facts={}
    for key,conditions in prep.CONDITIONS.items():
        extra={'admission':{'ddr_sha256':npu_ddr['sha256']},'power':{'supply_uv':800000},
               'supervision':{'supervisor':supervisor,'one_attempt':True}}.get(key,{})
        facts[key]=record('npu-'+key+'.json',dict(**target,schema='a733-npu-board-'+key+'-v1',reviewed=True,
            conditions={c:note for c in sorted(conditions)},**extra))
    raw=bytearray(board)+bytearray(216)
    struct.pack_into('<6Q',raw,592,prep.SESSION_MAGIC,1,808,owner,command,supervisor)
    struct.pack_into('<2I',raw,640,pck,0);raw[648:680]=catalogue
    for key,offset in prep.DIGESTS.items():raw[offset:offset+32]=bytes.fromhex(facts[key]['sha256'])
    raw=bytes(raw);th=hashlib.sha256(raw).hexdigest()
    prep.session_check(raw);prep.embedded_reference(image,raw)
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma',
        'remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')
    platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,
        physical_coherence_result='under-test',conditions={k:note for k in conditions}))
    admission=dict(**target,schema='a733-npu-board-supervised-entry-v1',reservation=struct.unpack_from('<Q',raw,200)[0],
        session_template_sha256=th,ddr=ddr('ddr.json',[[0x41000000,0x42200000]]),npu_ddr=npu_ddr,platform=platform,**facts,
        whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,
        recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,
            preserve_microsd=True,one_attempt=True,evidence=[note]))
    record('admission.json',admission);(fixture/'session-template.bin').write_bytes(raw)
    cases={}
    try:prep.validate_admission(admission,ih,th,raw)
    except ValueError as error:assert 'Modeled NPU board evidence' in str(error),error
    else:raise AssertionError('Production accepted modeled physical authority')
    cases['production-refuses-modeled-authority']=True
    bundle=a.output/'MODEL-NOT-PHYSICAL-bundle'
    with patch.object(prep,'physical_authority',lambda value:None):
        prepared=prep.prepare(a.image,a.manifest,fixture/'admission.json',fixture/'session-template.bin',bundle,
            a.clang,a.lld,0,[a.input],[a.reference])
        prep.validate_bundle(bundle)
    cases['modeled-bundle-prepared-and-revalidated']=True
    session=(bundle/'session.bin').read_bytes();contract_page=(bundle/'contract.bin').read_bytes()
    assert contract_page[224:256]==hashlib.sha256(session).digest() and contract_page[256:288]==session[80:112]
    assert struct.unpack_from('<Q',(bundle/'request.bin').read_bytes(),0)[0]==prep.REQUEST_MAGIC
    # The image's own validators on the prepared session and contract.
    e=harness.Image(a.image,layout_names=('a733_smp_contract','npu_board_session','a733_board_session'))
    contract=bytearray(contract_page[:288]);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC)
    for name,value in dict(armed_at=100,startup_deadline=24000100,diagnostic_deadline=480000100,expires_at=480000100).items():
        struct.pack_into('<Q',contract,e.layouts['a733_smp_contract'][name],value)
    contract=bytes(contract)
    class Machine:
        def __init__(self,publish=1):
            self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.reg_write(UC_ARM64_REG_PSTATE,13)
            self.u.mem_map(0x41000000,0x1200000);self.u.mem_map(0x50000000,0x200000)
            for address,data in e.parts:
                if data and address<0x42200000:self.u.mem_write(address,data)
            self.u.mem_write(UPLOAD,session);self.u.mem_write(0x42004000,contract)
            self.record=e.address('npu_admission');self.u.mem_write(self.record,contract);self.publications=[]
            def ret(value):self.u.reg_write(UC_ARM64_REG_X0,value);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
            def publication(*unused):
                self.publications.append([self.u.reg_read(r) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2)]);ret(publish)
            for name,callback in [('a733_smp_counter',lambda *x:ret(200)),('a733_cpu_coherence_publish',publication)]:
                address=e.address(name);self.u.hook_add(UC_HOOK_CODE,callback,begin=address,end=address)
            self.u.hook_add(UC_HOOK_CODE,lambda *x:self.u.emu_stop(),begin=0x501ff000,end=0x501ff000)
        def call(self,name,*values):
            for index,value in enumerate(values):self.u.reg_write(UC_ARM64_REG_X0+index,value)
            self.u.reg_write(UC_ARM64_REG_SP,0x501fe000);self.u.reg_write(UC_ARM64_REG_LR,0x501ff000)
            self.u.emu_start(e.address(name),0x501ff004,count=2000000)
            assert self.u.reg_read(UC_ARM64_REG_PC)==0x501ff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
            return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff
    m=Machine()
    assert m.call('npu_board_admitted_session')==0
    assert m.call('a733_smp_contract_valid',m.record,200)==1
    assert m.call('npu_board_prepare',m.record)==1 and m.call('npu_board_session_prepare',m.record)==1
    private=m.call('npu_board_admitted_session')
    assert private and len(m.publications)==2
    m.u.mem_write(UPLOAD,bytes(len(session)))
    assert bytes(m.u.mem_read(0x41000000+(private-0x41000000),len(session)))==session
    assert m.call('npu_board_session_prepare',m.record)==0
    cases['authenticated-private-copy-survives-upload-overwrite']=True
    def python_accepts(raw):
        try:prep.session_check(raw);prep.embedded_reference(image,raw)
        except ValueError:return 0
        return 1
    def patched(offset,value):
        changed=bytearray(session);changed[offset:offset+len(value)]=value;return bytes(changed)
    variants=[('accepted',session,1),
        ('workload-3',patched(400,struct.pack('<I',3)),0),
        ('owner-equals-pmic-owner',patched(616,struct.pack('<Q',owners[0])),0),
        ('owner-equals-usb0-owner',patched(616,struct.pack('<Q',owners[1])),0),
        ('pck-client-equals-board',patched(640,struct.pack('<I',board_pck)),0),
        ('pck-client-out-of-range',patched(640,struct.pack('<I',16)),0),
        ('wrong-catalogue-digest',patched(648,bytes([catalogue[0]^1])),0),
        ('zero-admission-digest',patched(680,bytes(32)),0),
        ('zero-supervision-digest',patched(776,bytes(32)),0),
        ('zero-command-admission',patched(624,bytes(8)),0),
        ('nonzero-reserved',patched(644,struct.pack('<I',1)),0),
        ('fabric-NSI-disabled',patched(492,struct.pack('<I',0)),0)]
    m=Machine()
    for name,raw,want in variants:
        m.u.mem_write(UPLOAD,raw)
        c=m.call('npu_board_session_valid',UPLOAD);py=python_accepts(raw)
        assert c==want and py==want,(name,c,py)
        cases[name]=dict(c=c,python=py)
    m=Machine(publish=-1)
    assert m.call('npu_board_session_prepare',m.record)==0 and m.call('npu_board_admitted_session')==0
    cases['failed-publication-never-exposes-session']=True
    board_pck_bytes=struct.pack('<I',board_pck)
    launcher=sram_launcher.check_bundle(bundle,a.output/'launcher',(prep.SESSION_MAGIC,1,808),(prep.CONTRACT_MAGIC,288),20,
        [('bounded-handoff',12,None,None),('owner-zero',14,(616,bytes(8)),None),
         ('pck-client-collision',14,(640,board_pck_bytes),None),('memory-authority-mismatch',14,(200,bytes(8)),None)],
        'Actual prepared AArch32 entry, NPU record predicates, fixed 20-second contract publication and refusal restoration. '
        'System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.',
        'PASS {count} NPU board SRAM launcher cases')
    cases['sram-launcher']=[c['case'] for c in launcher['cases']]
    result=dict(passed=True,physical_access=False,physical_authority=False,scope=__doc__,image_sha256=ih,
        session_sha256=hashlib.sha256(session).hexdigest(),launcher_sha256=prep.base.digest(bundle/'launch.bin'),
        bundle=str(bundle.resolve()),cases=cases)
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS NPU board preparation and admission:',len(cases),'case groups,',len(variants),'record variants')
if __name__=='__main__':main()
