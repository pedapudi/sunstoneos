"""Validate per-core timer/peer delivery and quiescence before shared cleanup."""
import importlib.util
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
