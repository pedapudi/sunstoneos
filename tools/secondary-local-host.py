"""Validate sequential production CPU preparation and restored local GIC state."""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('identity_host',ROOT/'tools/secondary-identity-host.py')
identity=importlib.util.module_from_spec(spec);spec.loader.exec_module(identity)
require,digest=identity.require,identity.digest
PROFILE='secondary-local-return-v1'
HOST_SOURCES=identity.HOST_SOURCES+('tools/secondary-local-host.py',)
MODEL_SOURCES=('checks/secondary-local-probe.py',)+identity.MODEL_SOURCES


def decode(raw,nonce,cached=False,coherent=False,interrupts=False):
    result=identity.decode(raw,nonce,local=True,cached=cached,coherent=coherent,interrupts=interrupts)
    require(not any(raw[0xa00:0xc00]),'Boot CPU local slot changed.')
    cores=[]
    for core in range(1,8):
        r=struct.unpack_from('<64Q',raw,0xa00+core*512)
        before,after=r[17:27],r[27:37]
        require(r[0] in (3,4) and r[1:5]==(result['epoch'],core,result['startup_deadline'],0x03460000+core*0x20000),
                'Local result is incomplete or belongs to another target.')
        require(r[5]==r[6] and not r[5]&1 and r[11:13]==(0,7) and r[14]&7==7 and
                not (r[15]&1 or r[16]&3) and not any(r[45:]),'Local cleanup or disabled group state differs.')
        require(before==after and before[:5]==(0,6,0,0,0) and not before[9]&1 and
                ((before[9]>>2)&1)==((before[9]>>3)&1),'Local state was not restored.')
        if r[0]==3:require(r[7:11]==(2,26,0,11),'Initialization did not reach native GIC_READY.')
        else:require(r[7]==5 and 10<=r[8]<=26 and r[9] and r[10]>=4,'Refusal lacks bounded controller evidence.')
        require(r[40]==r[44]==1,'Register access did not complete.')
        cores.append({'core':core,'initialized':r[0]==3,'refused_and_restored':r[0]==4,
                      'phase':r[8],'error':r[9],'writes':r[10],'cleanup_writes':r[12],
                      'controller_access':dict(zip(('address','value','operation','completed'),r[41:45])),
                      'before':before,'after':after})
    state,before,after,banks,deadline=struct.unpack_from('<5Q',raw,0x1a00)
    saved=struct.unpack_from('<32I',raw,0x1a28);restored=struct.unpack_from('<32I',raw,0x1aa8)
    require(state==4 and before==after and before&0x70==0x30 and not before&(1<<31) and
            1<=banks<=32 and deadline==result['startup_deadline'] and saved==restored and
            not saved[0] and not any(saved[banks:]) and not any(raw[0x1b28:0x1c00]),
            'Distributor restoration incomplete.')
    result.update(local_initialization_completed=all(c['initialized'] for c in cores),
                  local_results=cores,distributor_restored=True)
    return result

probe=SimpleNamespace(NATIVE=identity.probe.NATIVE,REPORT=(identity.probe.REPORT[0],0x1c00),decode=decode)
def initial_report(nonce):return identity.primary.initial_report(nonce)+bytes(0x1b00)
def load(build,image,manifest):return identity.load(build,image,manifest,local=True)
def validate_software(software,manifest_hash):
    require(software.get('schema')=='a733-secondary-local-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256')==manifest_hash and
            software.get('host_sources')=={n:digest(ROOT/n) for n in HOST_SOURCES},'Local host evidence differs.')
    require(software.get('host_cases')==dict.fromkeys(('complete-return','transport-failure-retained',
            'invalid-report-restored','changed-input-refused','incomplete-local-refused','restored-refusal-distinct'),True),
            'Local host cases incomplete.')
    item=software['instruction_evidence'];path=Path(item['path'])
    require(path.is_absolute() and digest(path)==item['sha256'],'Local instruction evidence changed.')
    result=json.loads(path.read_text())
    require(result.get('physical_access') is False and result.get('native_sha256')==probe.NATIVE and
            result.get('manifest_sha256')==manifest_hash and result.get('sources')=={n:digest(ROOT/n) for n in MODEL_SOURCES},
            'Local instruction inputs differ.')
    require({c['case']:c.get('pass') for c in result['cases']}==dict.fromkeys(('success','group-refusal','cleanup-readback'),True),
            'Local instruction cases incomplete.')
