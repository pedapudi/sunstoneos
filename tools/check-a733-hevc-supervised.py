#!/usr/bin/env python3
"""Capture one admitted supervised HEVC; retain resources until full power removal."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
supervisor=module('hevc_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('hevc_capture_preparation',ROOT/'apps/hevc/prepare.py')
decoder=module('hevc_capture_decoder',ROOT/'apps/hevc/observe.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','hevc-supervised-software'}
PRODUCT='sunstoneos native HEVC'
SERIAL='sunstoneos-a733-hevc'


host=module('hevc_capture_host',ROOT/'apps/hevc/host.py')


def capture_device(device,same,deadline,session,evidence,record,save,*,reference=None,upload):
    require(reference is None,'HEVC numerical references remain outside runtime ABI.')
    return host.capture_device(device,same,deadline,session,evidence,record,save,upload=upload)


def source_files():
    return [('hevc-host-tool.py',Path(__file__)),('hevc-preparation.py',Path(preparation.__file__)),
        ('hevc-observer.py',Path(decoder.__file__)),('hevc-capture.py',Path(host.__file__)),('hevc-producer.py',ROOT/'apps/hevc/producer.py'),
        ('hevc-sources.json',ROOT/'apps/hevc/sources.json'),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared HEVC metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid HEVC bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'HEVC input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    preparation.embedded_reference(frozen['image.elf'],frozen['session.bin'])
    prepared['_reference']=None
    prepared['_upload']=frozen['upload.bin']
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'HEVC')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-hevc-supervised-software-v1',('admission_cases','launcher_cases','host_capture','host_lifecycle'),
        'HEVC',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'HEVC','hevc-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release HEVC resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'HEVC entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,reference=prepared['_reference'],upload=prepared['_upload']),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='hevc_result',
        source_files=source_files(),description='Supervised native HEVC; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
