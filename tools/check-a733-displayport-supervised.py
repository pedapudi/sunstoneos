#!/usr/bin/env python3
"""Capture one admitted supervised DisplayPort; retain resources until full power removal."""
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
supervisor=module('displayport_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('displayport_capture_preparation',ROOT/'apps/displayport/prepare.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','displayport-supervised-software'}
PRODUCT='sunstoneos native DisplayPort'
SERIAL='sunstoneos-a733-displayport'


host=module('displayport_capture_host',ROOT/'apps/displayport/host.py')


def capture_device(device,same,deadline,session,evidence,record,save,*,allocation):
    return host.capture_device(device,same,deadline,session,evidence,record,save,allocation=allocation)


def source_files():
    return [('displayport-host-tool.py',Path(__file__)),('displayport-preparation.py',Path(preparation.__file__)),
        ('displayport-capture.py',Path(host.__file__)),('displayport-producer.py',ROOT/'apps/displayport/producer.py'),
        ('displayport-sources.json',ROOT/'apps/displayport/sources.json'),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared DisplayPort metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid DisplayPort bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'DisplayPort input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    prepared['_reference']=None
    admission=json.loads(frozen['admission.json'])
    reference=prepared['entry_evidence'][admission['allocation']['path']]
    prepared['_allocation']=json.loads(frozen[reference['name']])
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'DisplayPort')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-displayport-supervised-software-v1',('admission_cases','launcher_cases','host_capture','host_lifecycle'),
        'DisplayPort',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'DisplayPort','displayport-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release DisplayPort resources.')
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items()}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'DisplayPort entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,allocation=prepared['_allocation']),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=19,result_key='displayport_result',
        source_files=source_files(),description='Supervised native DisplayPort; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
