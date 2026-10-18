#!/usr/bin/env python3
"""Only supervised GPU admission, immutable input packaging and launcher cases."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_prepare',ROOT/'apps/gpu-render/prepare.py')
prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)

def main():
    a=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','output','clang','lld'):a.add_argument('--'+name,type=Path,required=True)
    a.add_argument('--skip-launcher',action='store_true',help='reuse unchanged SRAM launcher cases')
    a.add_argument('--prepare-only',action='store_true',help='prepare the modeled baseline without replaying checks')
    x=a.parse_args();out=x.output;out.mkdir(exist_ok=False)
    image_hash=prep.digest(x.image);parts,address,identity=prep.image_parts(x.image)
    # Explicit software-only prerequisite model. These records are never valid
    # physical admission; target identity makes that boundary visible in every file.
    raw=bytearray(592);struct.pack_into('<6Q',raw,0,0x4153373342535332,2,592,1,2,3)
    raw[48:80]=identity
    struct.pack_into('<4Q',raw,184,4,5,6,7);raw[216:312]=bytes([3])*96
    struct.pack_into('<I',raw,400,2);struct.pack_into('<2Q',raw,456,20,21)
    struct.pack_into('<3I',raw,488,0,1<<31,1<<31);struct.pack_into('<3I',raw,500,24000000,24000000,24000000)
    struct.pack_into('<4I',raw,512,3300000,3300000,1200000,900000);struct.pack_into('<I',raw,532,2 if json.loads(x.manifest.read_text()).get('execution_profile')=='gpu-single-core-supervised-v2' else 1)
    raw[564:570]=bytes([2,1,1,2,1,1])
    fixture=out/'fixtures';fixture.mkdir()
    def artifact(name,data):
        p=(fixture/name).absolute();p.write_bytes(data);return {'path':str(p),'sha256':hashlib.sha256(data).hexdigest()}
    evidence=artifact('software-only-model.txt',b'Software-only modeled prerequisite; never physical admission.\n')
    target={'identity':'software-only-model','port':'no-physical-port','image_sha256':image_hash}
    gpu={**target,'schema':'a733-gpu-initial-entry-v1','reservation':100,'cold_master':5,'entry_conditions_established':True,
        'no_gpu_activity_since_initial_isolation':True,'conditions':{k:evidence for k in sorted(prep.GPU_CONDITIONS)}}
    gpu_ref=artifact('gpu.json',json.dumps(gpu,sort_keys=True).encode());raw[80:112]=bytes.fromhex(gpu_ref['sha256'])
    template=fixture/'session-template.bin';template.write_bytes(raw)
    def ddr(name,ranges):return artifact(name,json.dumps({**target,'schema':'a733-ddr-reservation-v1','exclusive':True,
        'published_cache_disabled':True,'remote_writers_excluded':True,'ranges':ranges,'evidence':[evidence]}).encode())
    conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram',
        'exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
    platform=artifact('platform.json',json.dumps({**target,'schema':'a733-smp-observer-platform-v1','entry_conditions_established':True,
        'physical_coherence_result':'under-test','conditions':{k:evidence for k in sorted(conditions)}}).encode())
    record={**target,'schema':'a733-gpu-supervised-entry-v2','reservation':100,'session_template_sha256':prep.digest(template),
        'gpu':gpu_ref,'ddr':ddr('ddr.json',[[0x41000000,0x42200000]]),'gpu_ddr':ddr('gpu-ddr.json',[[0x48000000,0x79000000]]),
        'platform':platform,'whole_board_power_removal':True,'no_resource_reuse_before_power_removal':True,
        'recovery':{'mode':'manual-usb-reconnect','demonstrated_fel':True,'explicit_reconnect_before_resumption':True,
            'preserve_microsd':True,'one_attempt':True,'evidence':[evidence]}}
    admission=fixture/'admission.json';admission.write_text(json.dumps(record))
    bundle=out/'bundle';p=prep.prepare(x.image,x.manifest,admission,template,bundle,x.clang,x.lld)
    if x.prepare_only:
        print('Prepared software-only GPU fixture; never physical admission');return
    results=['immutable-model-bundle']
    for field in ('gpu','gpu_ddr'):
        changed={**record,field:evidence}
        try:prep.validate_admission(changed,image_hash,prep.digest(template))
        except (ValueError,RuntimeError,json.JSONDecodeError):pass
        else:raise AssertionError('missing prerequisite accepted')
        results.append(field+'-evidence-refused')
    incomplete={**gpu,'conditions':{k:v for k,v in gpu['conditions'].items() if k!='initial-power-removal-and-gpu-drain'}}
    ref=artifact('incomplete-gpu.json',json.dumps(incomplete).encode())
    try:prep.validate_admission({**record,'gpu':ref},image_hash,prep.digest(template))
    except ValueError:pass
    else:raise AssertionError('missing initial isolation evidence accepted')
    results.append('missing-initial-isolation-refused')
    # Run only the new SRAM ABI cases in the maintained instruction model.
    if not x.skip_launcher:
        text=(ROOT/'checks/a733-smp-supervised-launch.py').read_text().replace('0x4153373353554333','0x4153373347534332').replace('480000000','1440000000')
        text=text.replace('reason in (1,2,3,4,5,6,13)','reason in (1,2,3,4,5,6,13,14)')
        first=text.index('    if args.frequency_only:');last=text.index('    result =',first)
        text=text[:first]+'''    cases=[check('GPU supervised large reservation',12),
            check('GPU uploaded watchdog claim refused',14,mutation=(0x41f00000+112,1)),
            check('GPU large reservation mismatch refused',3,mutation=(CONTRACT+192,0x49000000)),
            check('GPU initial isolation digest mismatch refused',3,mutation=(CONTRACT+256,0))]
'''+text[last:]
        model=out/'launcher-model.py';model.write_text(text)
        r=subprocess.run([sys.executable,str(model),'--bundle',str(bundle),'--output',str(out/'launcher-result.json')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (out/'launcher.log').write_text(r.stdout);assert r.returncode==0,r.stdout
        results.append('native-SRAM-supervised-ABI')
    with x.image.open('rb') as f:
        e=ELFFile(f);symbols={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
    runtime=bytearray((bundle/'contract.bin').read_bytes()[:288]);start=100000000
    struct.pack_into('<Q',runtime,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',runtime,80,start,start+24000000,start+1440000000,start+1440000000)
    session=(bundle/'session.bin').read_bytes()
    for mutation,label in [(None,'native-supervised-admission'),(112,'watchdog-session-refused'),(456,'owner-session-refused')]:
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x41000000,0x1200000);cpu.mem_map(0x43000000,0x10000)
        for part in parts:
            if part['data']:cpu.mem_write(part['address'],part['data'])
        cpu.mem_write(symbols['gpu_admission'],bytes(runtime));changed=bytearray(session)
        if mutation is not None:struct.pack_into('<Q',changed,mutation,9)
        cpu.mem_write(0x41f00000,bytes(changed));cpu.reg_write(UC_ARM64_REG_SP,0x4300fff0);cpu.reg_write(UC_ARM64_REG_LR,0x43000000)
        cpu.reg_write(UC_ARM64_REG_X0,symbols['gpu_admission']);cpu.reg_write(UC_ARM64_REG_X0+1,start+100)
        stopped=[]
        def hook(machine,pc,size,unused):
            if pc==0x43000000:stopped.append(True);machine.emu_stop()
        cpu.hook_add(UC_HOOK_CODE,hook);cpu.emu_start(symbols['a733_smp_contract_valid'],0,count=1000000)
        assert stopped and bool(cpu.reg_read(UC_ARM64_REG_X0))==(mutation is None),label
        results.append(label)
    # Frozen evidence survives removal of original raw prerequisites.
    for path in fixture.iterdir():
        if path.suffix in ('.json','.txt'):path.unlink()
    prep.validate_bundle(bundle);results.append('frozen-evidence-reused')
    changed=bytearray((bundle/'session.bin').read_bytes());changed[80]^=1;(bundle/'session.bin').write_bytes(changed)
    try:prep.validate_bundle(bundle)
    except (ValueError,RuntimeError):pass
    else:raise AssertionError('changed prepared session accepted')
    results.append('changed-bundle-refused')
    (out/'result.json').write_text(json.dumps({'status':'pass','cases':results,'image_sha256':image_hash,'physical_access':False},indent=2)+'\n')
    print('PASS supervised GPU admission, immutable packaging and refusal cases; '+('unchanged SRAM launcher cases reused' if x.skip_launcher else 'SRAM launcher cases passed')+'; no GPU execution modeled')

if __name__=='__main__':main()
