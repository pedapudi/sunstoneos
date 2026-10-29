#!/usr/bin/env python3
"""Capture one admitted supervised HDMI audio; retain resources until full power removal."""
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
supervisor=module('hdmi_audio_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('hdmi_audio_capture_preparation',ROOT/'apps/hdmi-audio/prepare.py')
require,digest=supervisor.require,supervisor.digest
PREREQUISITES={'supervised-ram-recovery','hdmi-audio-supervised-software'}
PRODUCT='sunstoneos HDMI audio capture'
SERIAL='sunstoneos-a733-hdmi-audio'


host=module('hdmi_audio_capture_host',ROOT/'apps/hdmi-audio/capture.py')


def capture_device(device,same,deadline,session,pcm,evidence,record,save):
    return host.capture_device(device,same,deadline,session,pcm,evidence,record,save)


def source_files():
    return [('hdmi-audio-host-tool.py',Path(__file__)),('hdmi-audio-preparation.py',Path(preparation.__file__)),
        ('hdmi-audio-capture.py',Path(host.__file__)),('hdmi-preparation.py',Path(preparation.base.__file__)),
        ('dma-nonsecure-access.py',Path(preparation.security_authority.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared HDMI audio metadata changed during validation.')
    frozen={}
    for name,identity in prepared['files'].items():
        require(Path(name).name==name,'Invalid HDMI audio bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'HDMI audio input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
    session=frozen['source-session.bin']
    require(len(session) in (816,848),'Version1 HDMI audio is offline-only.')
    preparation.image_metadata(frozen['image.elf'],session_size=len(session))
    preparation.session_check(session,frozen['input.pcm'])
    prepared['_frozen']=frozen
    return prepared


def reference_bytes(value):
    return supervisor.reviewed_reference(value,'HDMI audio')


def software_sources():
    return supervisor.device_software_sources(source_files())


def validate_software(value,prepared):
    supervisor.validate_device_software(value,prepared,'a733-hdmi-audio-supervised-software-v1',('admission_cases','launcher_cases','host_capture','native_composition'),
        'HDMI audio',reference_bytes)


def claimed(database,test_id,owner,bundle,prepared):
    supervisor.device_claim(database,test_id,owner,bundle,prepared,'HDMI audio','hdmi-audio-supervised-software',PREREQUISITES,
        reference_bytes,validate_software)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','fel','evidence','database'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--claim',required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--supervised-manual-reconnect',action='store_true',required=True,
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release HDMI audio resources.')
    args=parser.parse_args(argv)
    require(args.evidence.parent.is_dir(),'HDMI audio evidence parent must already exist.')
    bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['source-session.bin'];pcm=frozen['input.pcm']
    entry={path:frozen['entry-evidence-'+str(i)+'.bin'] for i,path in enumerate(json.loads(frozen['entry-index.bin']))}
    def validate_entry(identity,port):
        preparation.validate_entry(admission,prepared['image_sha256'],entry,True)
        require(admission['identity']==identity and admission['port']==port,'HDMI audio entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,pcm,args.evidence,record,save),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0111,capture_seconds=19,result_key='hdmi_audio_result',
        source_files=source_files(),description='Supervised native HDMI audio; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,sqlite3.Error,struct.error) as error:sys.exit(f'Stopped: {error}')
