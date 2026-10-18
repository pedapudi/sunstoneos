"""Validate per-core cached entry and exact restoration before CPU shutdown."""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('local_host',ROOT/'tools/secondary-local-host.py')
local=importlib.util.module_from_spec(spec);spec.loader.exec_module(local)
require,digest=local.require,local.digest
PROFILE='secondary-cache-return-v1'
HOST_SOURCES=local.HOST_SOURCES+('tools/secondary-cache-host.py',)
MODEL_SOURCES=('checks/secondary-cache-probe.py',)+local.MODEL_SOURCES


def decode(raw,nonce,smc_return):
    result=local.decode(raw,nonce,cached=True)
    require(not any(raw[0x1c00:0x2100]) and not any(raw[0x2800:0x2900]),
            'Unused cache report storage changed.')
    cores=[]
    for core,identity,initialized in zip(range(1,8),result['cores'],result['local_results']):
        offset=0x2000+core*256
        el3=struct.unpack_from('<20Q',raw,offset)
        el1=struct.unpack_from('<32Q',raw,0x2800+core*256)
        require(not any(raw[offset+160:offset+256]),'EL3 report padding changed.')
        if not initialized['initialized']:
            require(not any(el3+el1),'Refused local initialization entered cached execution.')
            cores.append({'core':core,'cached_entry_completed':False,'refused_before_cache':True})
            continue
        binding=(result['epoch'],result['startup_deadline'])
        require(el3[:4]==(3,*binding,1) and el3[19]==0 and el3[4]==identity['sctlr3'],
                'Cached EL3 entry incomplete or has different original controls.')
        require(el3[9:14]==(el3[4]|0x1005,0x80823019,0x421f2000,0x44ff00,el3[8]&~0x200),
                'Cached EL3 translation or security state differs.')
        require(el3[14:19]==el3[4:9],'EL3 controls were not restored.')
        require(el1[:3]==(3,*binding) and el1[4:6]==(identity['sctlr1']|1,identity['tcr1']|0x800080),
                'Cached EL1 entry incomplete or has different prepared controls.')
        require(0x42006000+core*4096<=el1[3]<0x42006000+(core+1)*4096 and not el1[3]&15,
                'Saved EL3 stack does not belong to the target.')
        require(el1[9:23]==(0x733,4,0x30d01805,0x200803019,0x421f4000,0x44ff00,0,
                0x41243000+(core+1)*32768,core,24000000,0x81000000|(core<<8),0,0x5e000733,smc_return) and
                el1[23]&0x3ff==0x3c5 and el1[29:]==(0,8,12),
                'EL1 cached observation or SMC return differs.')
        require(el1[24:29]==el1[4:9],'EL1 controls were not restored.')
        cores.append({'core':core,'cached_entry_completed':True,'refused_before_cache':False,
            'el3_before':el3[4:9],'el3_cached':el3[9:14],'el3_restored':el3[14:19],
            'el1_before':el1[4:9],'el1_observed':el1[9:24],'el1_restored':el1[24:29]})
    result.update(cached_entry_completed=all(c['cached_entry_completed'] for c in cores),cache_results=cores)
    return result


probe=SimpleNamespace(NATIVE=local.probe.NATIVE,REPORT=(local.probe.REPORT[0],0x3000),decode=decode)
def initial_report(nonce):return local.identity.primary.initial_report(nonce)+bytes(0x2f00)
def load(build,image,manifest):return local.identity.load(build,image,manifest,cached=True)
def validate_software(software,manifest_hash):
    require(software.get('schema')=='a733-secondary-cache-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256')==manifest_hash and
            software.get('host_sources')=={n:digest(ROOT/n) for n in HOST_SOURCES},'Cache host evidence differs.')
    require(software.get('host_cases')==dict.fromkeys(('complete-return','transport-failure-retained',
            'invalid-report-restored','changed-input-refused','incomplete-cache-refused','restored-refusal-distinct'),True),
            'Cache host cases incomplete.')
    item=software['instruction_evidence'];path=Path(item['path'])
    require(path.is_absolute() and digest(path)==item['sha256'],'Cache instruction evidence changed.')
    result=json.loads(path.read_text())
    require(result.get('physical_access') is False and result.get('native_sha256')==probe.NATIVE and
            result.get('manifest_sha256')==manifest_hash and result.get('sources')=={n:digest(ROOT/n) for n in MODEL_SOURCES},
            'Cache instruction inputs differ.')
    require({c['case']:c.get('pass') for c in result['cases']}==dict.fromkeys(('success','wrong-el1-core','restore-el3'),True),
            'Cache instruction cases incomplete.')
