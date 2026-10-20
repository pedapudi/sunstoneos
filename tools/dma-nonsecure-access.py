"""Validate the fixed external DMA0 authority used by native DMA consumers.

Resolvers supply the exact frozen byte buffers; this module performs no I/O.
Declarations require independently reviewed physical evidence, not an MMIO probe.
"""
import hashlib
import json
from pathlib import PurePath

NUMBERS=('epoch','generation','lease','controller','service_channels','dma_evidence','dma_handoff')
DIGESTS=('configuration_sha256','session_template_sha256')
FACTS=('controller_access_nonsecure_el1','descriptor_data_access_nonsecure',
       'valid_after_reset_release','security_writers_excluded_until_recovery')

def validate(data,expected,evidence,require):
    security=json.loads(data)
    require(isinstance(security,dict) and security.get('schema')=='a733-dma-nonsecure-access-v1' and
            expected.get('identity') and security.get('identity')==expected['identity'] and
            all(isinstance(expected.get(k),str) and expected[k] and security.get(k)==expected[k]
                for k in ('port','image_sha256')),
            'DMA security authority covers a different image or target.')
    require(security.get('physical_authority') is True and security.get('modeled') is False and
            security.get('reviewed') is True,'DMA security requires reviewed physical authority.')
    for name in NUMBERS:
        value=expected[name]
        require(type(value) is int and 0<value<1<<64 and type(security.get(name)) is int and
                security[name]==value,'DMA security session/controller binding differs: '+name)
    require(expected['controller']==0x04601000,'DMA security requires the fixed DMA0 controller.')
    mask=security.get('nonsecure_channels')
    require(type(mask) is int and 0<mask<=0xffff and 0<security['service_channels']<=0xffff and
            security['service_channels']&mask==security['service_channels'],
            'DMA security evidence omits selected nonsecure channels.')
    for name in DIGESTS:
        if name in expected:
            require(type(expected[name]) is str and len(expected[name])==64 and
                    security.get(name)==expected[name],'DMA security digest binding differs: '+name)
    require(all(security.get(name) is True for name in FACTS),
            'DMA security access/lifetime authority is incomplete.')
    require(isinstance(security.get('evidence'),list) and security['evidence'],
            'DMA security observations are missing.')
    for reference in security['evidence']:
        require(isinstance(reference,dict) and set(reference)=={'path','sha256'} and
                isinstance(reference['path'],str) and PurePath(reference['path']).is_absolute(),
                'Invalid DMA security evidence reference.')
        blob=evidence(reference)
        require(isinstance(blob,bytes) and hashlib.sha256(blob).hexdigest()==reference['sha256'],
                'DMA security supporting evidence changed.')
    return security
