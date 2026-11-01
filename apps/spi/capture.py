#!/usr/bin/env python3
"""Capture one admitted supervised spi observation; retain resources until full power removal."""
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
supervisor=module('spi_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('spi_capture_preparation',ROOT/'apps/spi/prepare.py')
decoder=module('spi_capture_decoder',ROOT/'apps/spi/observe.py')
require,digest=supervisor.require,supervisor.digest
PRODUCT='sunstoneos native SPI'
SERIAL='sunstoneos-a733-spi'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    record.update(spi_retention='whole-board-power-removal',spi_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'SPI capture deadline expired.')
        frame=exchange(bytes(1),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'spi-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('spi_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        require(now()<deadline,'SPI response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        if result['status']==1:
            output=evidence/'output.bin';supervisor.durable_frame(output,frame[128:])
            require(now()<deadline,'SPI output became durable after capture deadline.')
            record['copied_spi_output']={'path':output.name,'bytes':len(frame)-128,'sha256':hashlib.sha256(frame[128:]).hexdigest()}
        record['spi_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['spi_result']='unobserved';record.pop('durable_result',None);record.pop('copied_spi_output',None);raise
        return result
    raise supervisor.CaptureWindowExpired('SPI exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==1,'Incomplete SPI request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'SPI capture deadline expired after write.')
        maximum=4320 if preparation.dma_mode(session) else 224
        frame=bytearray();expected=maximum
        for _ in range(maximum):
            same();remaining=int((deadline-time.monotonic())*1000)
            require(remaining>0,'SPI capture deadline expired during frame.')
            chunk=bytes(device.read(0x81,expected-len(frame),timeout=min(timeout,remaining)))
            require(chunk and len(chunk)<=expected-len(frame),'Invalid SPI fragment.')
            frame.extend(chunk)
            if len(frame)>=128:
                count=struct.unpack_from('<I',frame,12)[0]
                require(count in (0,maximum-128),'Invalid SPI payload extent.')
                expected=128+count
                require(len(frame)<=expected,'Excess SPI payload.')
                if len(frame)==expected:return bytes(frame)
        raise ValueError('SPI frame fragment bound exhausted.')
    return capture(exchange,session,evidence,record,save,deadline)


def source_files():
    return [('spi-host-tool.py',Path(__file__)),('spi-preparation.py',Path(preparation.__file__)),
        ('spi-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared SPI metadata changed during validation.')
    frozen={}
    for name,identity in preparation.board.snapshot_files(prepared).items():
        require(Path(name).name==name,'Invalid SPI bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'SPI input changed before use: '+name)
        frozen[name]=data
    prepared['_prepared_sha256']=hashlib.sha256(metadata).hexdigest()
    prepared['_source_hashes']={name:digest(path) for name,path in software_sources().items()}
    prepared['_sizes']={name:len(data) for name,data in frozen.items()}
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
        help='Use the reviewed whole-board power-removal plan; cable reconnect does not release SPI resources.')
    parser.add_argument('--database', type=Path)
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items() if item['name'] in frozen}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'SPI entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='spi_result',
        source_files=source_files(),description='Supervised native SPI; retain every resource until whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,struct.error) as error:sys.exit(f'Stopped: {error}')
