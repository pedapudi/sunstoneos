"""Validate concurrent cached exchange, NS cleaning and final physical memory."""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cache_host',ROOT/'tools/secondary-cache-host.py')
cache=importlib.util.module_from_spec(spec);spec.loader.exec_module(cache)
require,digest=cache.require,cache.digest
PROFILE='secondary-coherence-return-v1'
HOST_SOURCES=cache.HOST_SOURCES+('tools/secondary-coherence-host.py',)
MODEL_SOURCES=('checks/secondary-coherence-probe.py',)+cache.MODEL_SOURCES


def expected_memory(epoch,interrupts=False):
    page=bytearray(4096)
    for offset,value in ((0,224),(64,224),(128,epoch^223)):struct.pack_into('<Q',page,offset,value)
    for core in range(1,8):struct.pack_into('<6Q',page,0x100+core*128,epoch,epoch,epoch,224,epoch^223,32)
    if interrupts:
        for core in range(1,8):
            struct.pack_into('<8Q',page,0x100+core*128+48,1,1,epoch,epoch,epoch,0,0,0)
    return bytes(page)


def decode(raw,nonce,smc_return,interrupts=False):
    result=cache.decode(raw,nonce,smc_return,coherent=True,interrupts=interrupts)
    require(result['cached_entry_completed'],'Every concurrent participant must complete cached execution.')
    require(not any(raw[0x3000:0x3040]),'Unused primary coherence record changed.')
    cores=[]
    for core in range(1,8):
        values=struct.unpack_from('<8Q',raw,0x3000+core*64)
        require(values==(3,core,result['epoch'],32,224,result['epoch']^223,64,0),
                'Concurrent exchange or per-core NS clean completion differs.')
        cores.append({'core':core,'rounds':32,'atomic_count':224,'cleaned_lines':64})
    require(raw[0x3200:]==expected_memory(result['epoch'],interrupts=interrupts),
            'Physical memory after all targets reached OFF differs from the shared exchange.')
    result.update(concurrent_exchange_completed=True,coherence_results=cores,shared_memory_published_to_poc=True)
    return result

probe=SimpleNamespace(NATIVE=cache.probe.NATIVE,REPORT=(cache.probe.REPORT[0],0x4200),decode=decode)
def initial_report(nonce):return cache.local.identity.primary.initial_report(nonce)+bytes(0x4100)
def load(build,image,manifest):return cache.local.identity.load(build,image,manifest,coherent=True)
def validate_software(software,manifest_hash):
    require(software.get('schema')=='a733-secondary-coherence-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256')==manifest_hash and
            software.get('host_sources')=={n:digest(ROOT/n) for n in HOST_SOURCES},'Coherence host evidence differs.')
    require(software.get('host_cases')==dict.fromkeys(('complete-return','transport-failure-retained',
            'invalid-report-restored','changed-input-refused','incomplete-coherence-refused','wrong-memory-result-refused'),True),
            'Coherence host cases incomplete.')
    item=software['instruction_evidence'];path=Path(item['path'])
    require(path.is_absolute() and digest(path)==item['sha256'],'Coherence instruction evidence changed.')
    result=json.loads(path.read_text())
    require(result.get('physical_access') is False and result.get('native_sha256')==probe.NATIVE and
            result.get('manifest_sha256')==manifest_hash and result.get('sources')=={n:digest(ROOT/n) for n in MODEL_SOURCES},
            'Coherence instruction inputs differ.')
    require({c['case']:c.get('pass') for c in result['cases']}==dict.fromkeys(('success','payload-corruption','restore-el3'),True),
            'Coherence instruction cases incomplete.')
