#!/usr/bin/env python3
"""Capture one admitted supervised deinterlace; retain resources until full power removal."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
supervisor=module('deinterlace_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('deinterlace_capture_preparation',ROOT/'apps/deinterlace/prepare.py')
require,digest=supervisor.require,supervisor.digest
PRODUCT='sunstoneos native deinterlace'
SERIAL='sunstoneos-a733-deinterlace'


host=module('deinterlace_capture_host',ROOT/'apps/deinterlace/capture.py')


def capture_device(device,same,deadline,session,evidence,record,save,*,upload):
    return host.capture_device(device,same,deadline,session,evidence,record,save,upload=upload)


def source_files():
    return [('deinterlace-host-tool.py',Path(__file__)),('deinterlace-preparation.py',Path(preparation.__file__)),
        ('deinterlace-capture.py',Path(host.__file__)),('deinterlace-producer.py',ROOT/'apps/deinterlace/producer.py'),
        ('deinterlace-sources.json',ROOT/'apps/deinterlace/sources.json'),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared deinterlace metadata changed during validation.')
    frozen={}
    for name,identity in preparation.board.snapshot_files(prepared).items():
        require(Path(name).name==name,'Invalid deinterlace bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'deinterlace input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    preparation.embedded_reference(frozen['image.elf'],frozen['session.bin'])
    prepared['_reference']=None
    prepared['_upload']=frozen['upload.bin']
    prepared['_frozen']=frozen
    return prepared




def software_sources():
    paths=dict(source_files())
    paths.update({'supervised-host-tool.py':Path(supervisor.__file__),
                  'base-host-tool.py':Path(supervisor.base.__file__),
                  'observer-host-tool.py':Path(supervisor.observer.__file__)})
    return paths




def claimed(database, test_id, owner, bundle, prepared):
    """Check the input snapshot immediately before upload."""
    expected = preparation.board.snapshot_files(prepared)
    if '_prepared_sha256' in prepared:
        expected['prepared.json'] = prepared['_prepared_sha256']
    for name, identity in expected.items():
        require(Path(name).name == name and digest(bundle / name) == identity,
                'Prepared input changed before upload: ' + name)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim');parser.add_argument('--owner')
    parser.add_argument('--supervised-manual-reconnect',action='store_true',
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release deinterlace resources.')
    parser.add_argument('--database', type=Path)
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items() if item['name'] in frozen}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'deinterlace entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,upload=prepared['_upload']),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=19,result_key='deinterlace_result',
        source_files=source_files(),description='Supervised native deinterlace; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,struct.error) as error:sys.exit(f'Stopped: {error}')
