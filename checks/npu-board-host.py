#!/usr/bin/env python3
"""Check direct NPU board preparation and the native session validators.

Unicorn executes the linked session and contract validators, private session
copy, constructors and SRAM launcher. The checks retain clock, thermal,
ownership, upload-integrity and bounded-return refusals. No device is opened.
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
    p.add_argument('--bundle-only',action='store_true')
    a=p.parse_args();a.output=module('board_host_evidence',ROOT/'tools/host_evidence.py').unique_directory(a.output)
    prep=module('npu_board_host_prepare',ROOT/'apps/npu-board/prepare.py')
    fixture=a.output/'MODEL-NOT-PHYSICAL-fixture';fixture.mkdir()
    image=a.image.read_bytes();ih=hashlib.sha256(image).hexdigest()
    catalogue=prep._image_parts(image)[3]
    identity=hashlib.sha256(b'Native input fixture; no physical qualification').digest()
    board=bytearray(BOARD_FIXTURE.read_bytes()[:592])
    for offset in (80,216,248,280):board[offset:offset+32]=identity
    struct.pack_into('<3I',board,488,0,0x80000000,0x80000000);struct.pack_into('<3I',board,500,24000000,24000000,24000000)
    owners=struct.unpack_from('<4Q',board,456);board_pck=struct.unpack_from('<I',board,540)[0]
    owner,command,supervisor=0x4e505542,0x434d4441,0x53555056;pck=(board_pck+4)%16
    assert owner not in owners
    raw=bytearray(board)+bytearray(prep.SESSION_BYTES-592)
    struct.pack_into('<6Q',raw,592,prep.SESSION_MAGIC,prep.SESSION_VERSION,prep.SESSION_BYTES,owner,command,supervisor)
    struct.pack_into('<2I',raw,640,pck,0);raw[648:680]=catalogue
    for key,offset in prep.DIGESTS.items():raw[offset:offset+32]=hashlib.sha256(key.encode()).digest()
    struct.pack_into('<2Q',raw,808,0x544853,91)
    for i,key in enumerate(('initial','electrical','clocks','supervision')):
        raw[824+32*i:856+32*i]=hashlib.sha256(key.encode()).digest()
    struct.pack_into('<2I4B',raw,952,1,12000000,1,31,31,0)
    for i in range(5):struct.pack_into('<3H',raw,964+6*i,1400,1600,1000)
    raw=bytes(raw);th=hashlib.sha256(raw).hexdigest()
    prep.session_check(raw);prep.embedded_reference(image,raw)
    (fixture/'session-template.bin').write_bytes(raw)
    cases={}
    assert prep.validate_admission({},ih,th,raw)=={}
    cases['native-session-without-process-records']=True
    bundle=a.output/'bundle'
    prepared=prep.prepare(a.image,None,None,fixture/'session-template.bin',bundle,
        a.clang,a.lld,0,[a.input],[a.reference])
    prep.validate_bundle(bundle)
    cases['bundle-prepared-and-revalidated']=True
    for path in bundle.iterdir():
        if path.is_file() and path.name not in prepared['files'] and path.name!='prepared.json':
            original=path.read_bytes()
            try:
                path.write_bytes(b'Changed archived build notes or source')
                assert prep.validate_bundle(bundle)['files']==prepared['files']
            finally:path.write_bytes(original)
    cases['archived-build-files-independent']=True
    upload=bundle/'segment-0.bin';original=upload.read_bytes()
    try:
        upload.write_bytes(original+b'x')
        harness.refused(lambda:prep.validate_bundle(bundle),'bundle input changed',ValueError)
    finally:upload.write_bytes(original)
    cases['changed-upload-refused']=True
    if a.bundle_only:
        (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
            cases=cases,bundle=str(bundle),native_cases_reused=True))+'\n')
        print('PASS NPU board runtime files; archived inputs independent and changed upload refused')
        return
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
        ('fabric-NSI-disabled',patched(492,struct.pack('<I',0)),0),
        ('old-session-version',patched(600,struct.pack('<Q',1)),0),
        ('thermal-owner-equals-npu',patched(808,struct.pack('<Q',owner)),0),
        ('thermal-owner-equals-board',patched(808,struct.pack('<Q',owners[3])),0),
        ('thermal-authority-absent',patched(816,bytes(8)),0),
        ('thermal-evidence-absent',patched(824,bytes(32)),0),
        ('thermal-clock-wrong',patched(956,struct.pack('<I',24000000)),0),
        ('thermal-alarm-mask-wrong',patched(961,bytes(1)),0),
        ('thermal-threshold-wrong',patched(964,bytes(2)),0),
        ('thermal-reserved-nonzero',patched(994,bytes([1])),0)]
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
    launcher=sram_launcher.check_bundle(bundle,a.output/'launcher',(prep.SESSION_MAGIC,prep.SESSION_VERSION,prep.SESSION_BYTES),(prep.CONTRACT_MAGIC,288),20,
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
