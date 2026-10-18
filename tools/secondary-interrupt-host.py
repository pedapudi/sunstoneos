"""Validate per-core timer/peer delivery and quiescence before shared cleanup."""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('coherence_host',ROOT/'tools/secondary-coherence-host.py')
coherence=importlib.util.module_from_spec(spec);spec.loader.exec_module(coherence)
require,digest=coherence.require,coherence.digest
PROFILE='secondary-interrupt-return-v1'
HOST_SOURCES=coherence.HOST_SOURCES+('tools/secondary-interrupt-host.py',)
MODEL_SOURCES=('checks/secondary-interrupt-probe.py',)+coherence.MODEL_SOURCES


def expected_memory(epoch):return coherence.expected_memory(epoch,interrupts=True)


def decode(raw,nonce,smc_return):
    result=coherence.decode(raw,nonce,smc_return,interrupts=True)
    # The inherited whole-page comparison requires one SGI, one timer IRQ,
    # matching ready/sent/done epochs and disabled, empty interrupt state.
    result.update(interrupt_delivery_completed=True,
        interrupt_results=[{'core':c,'sgi_count':1,'timer_count':1,
            'group_enabled':False,'pending':0,'active':0} for c in range(1,8)])
    return result


probe=SimpleNamespace(NATIVE=coherence.probe.NATIVE,REPORT=coherence.probe.REPORT,decode=decode)
def initial_report(nonce):return coherence.initial_report(nonce)
def load(build,image,manifest):return coherence.cache.local.identity.load(build,image,manifest,interrupts=True)
def validate_software(software,manifest_hash):
    require(software.get('schema')=='a733-secondary-interrupt-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256')==manifest_hash and
            software.get('host_sources')=={n:digest(ROOT/n) for n in HOST_SOURCES},'Interrupt host evidence differs.')
    require(software.get('host_cases')==dict.fromkeys(('complete-return','transport-failure-retained',
            'invalid-report-restored','changed-input-refused','incomplete-interrupts-refused','nonquiescent-interrupts-refused'),True),
            'Interrupt host cases incomplete.')
    item=software['instruction_evidence'];path=Path(item['path'])
    require(path.is_absolute() and digest(path)==item['sha256'],'Interrupt instruction evidence changed.')
    result=json.loads(path.read_text())
    require(result.get('physical_access') is False and result.get('native_sha256')==probe.NATIVE and
            result.get('manifest_sha256')==manifest_hash and result.get('sources')=={n:digest(ROOT/n) for n in MODEL_SOURCES},
            'Interrupt instruction inputs differ.')
    require({c['case']:c.get('pass') for c in result['cases']}==dict.fromkeys(('success','wrong-irq','pending-at-stop'),True),
            'Interrupt instruction cases incomplete.')
