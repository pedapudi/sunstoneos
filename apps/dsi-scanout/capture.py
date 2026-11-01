#!/usr/bin/env python3
"""Capture one admitted supervised dsi observation; copy the retained controller observation."""
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
supervisor=module('dsi_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py')
preparation=module('dsi_capture_preparation',ROOT/'apps/dsi-scanout/prepare.py')
decoder=module('dsi_capture_decoder',ROOT/'apps/dsi-scanout/observe.py')
require,digest=supervisor.require,supervisor.digest
PRODUCT='sunstoneos native DSI'
SERIAL='sunstoneos-a733-dsi'


def capture(exchange,session,evidence,record,save,deadline,now=time.monotonic,pause=time.sleep,expected=None):
    preparation.session_check(session)
    record.update(dsi_retention='whole-board-power-removal',dsi_result='unobserved');save()
    for sequence in range(1,2401):
        require(now()<deadline,'DSI capture deadline expired.')
        frame=exchange(bytes(1),min(500,max(1,int((deadline-now())*1000))))
        path=evidence/f'dsi-{sequence:03}.bin';supervisor.durable_frame(path,frame)
        entry={'path':path.name,'sha256':hashlib.sha256(frame).hexdigest()}
        record.setdefault('dsi_frames',[]).append(entry);save()
        result=decoder.decode(frame,session);entry.update(result)
        require(now()<deadline,'DSI response arrived after capture deadline.')
        if result['status']==0:
            pause(min(.025,max(0,deadline-now())));continue
        if result['status']==1:
            if expected is not None:require((result.get('sequence'),result.get('slot'))==expected,'Replay completion differs from outstanding presentation')
            output=evidence/'output.bin';supervisor.durable_frame(output,frame[128:])
            require(now()<deadline,'DSI output became durable after capture deadline.')
            record['copied_dsi_output']={'path':output.name,'bytes':256,'sha256':hashlib.sha256(frame[128:]).hexdigest()}
        record['dsi_result']='observed-success' if result['status']==1 else 'observed-failure'
        record['durable_result']=entry.copy()
        try:save()
        except BaseException:
            record['dsi_result']='unobserved';record.pop('durable_result',None);record.pop('copied_dsi_output',None);raise
        return result
    raise supervisor.CaptureWindowExpired('DSI exhausted bounded pending-query budget.')


def capture_device(device,same,deadline,session,evidence,record,save,frames=None,slots=(0,1,0)):
    def exchange(packet,timeout):
        same();require(device.write(0x02,packet,timeout=timeout)==1,'Incomplete DSI request.')
        same();remaining=int((deadline-time.monotonic())*1000)
        require(remaining>0,'DSI capture deadline expired after write.')
        return bytes(device.read(0x81,384,timeout=min(timeout,remaining)))
    if struct.unpack_from('<Q',session,600)[0]==1:
        require(frames is None,'Ordinary scanout cannot accept replay frames')
        return capture(exchange,session,evidence,record,save,deadline)
    require(frames is not None and len(frames)==2 and 0<len(slots)<=256 and all(type(s)is int and s in (0,1) for s in slots),'Two frozen replay frames and bounded slot selections required')
    def send(data):
        for offset in range(0,len(data),512):
            same();remaining=int((deadline-time.monotonic())*1000);require(remaining>0,'Replay upload deadline expired')
            fragment=data[offset:offset+512];require(device.write(0x02,fragment,timeout=min(500,remaining))==len(fragment),'Incomplete replay upload')
    uploads=[preparation.profile.upload_bytes(session,slot,frames[slot]) for slot in range(2)]
    for data in uploads:send(data)
    for sequence,slot in enumerate(slots,1):
        send(preparation.profile.input_header(session,2,slot,sequence));child={};directory=evidence/f'present-{sequence:03}';directory.mkdir()
        record.setdefault('presentations',[]).append(child)
        result=capture(exchange,session,directory,child,save,deadline,expected=(sequence,slot))
        require(result['status']==1 and result['sequence']==sequence and result['slot']==slot,'Replay completion differs from requested presentation')
        save()
    record['dsi_result']='observed-success';return result


def source_files():
    return [('dsi-host-tool.py',Path(__file__)),('dsi-preparation.py',Path(preparation.__file__)),
        ('dsi-decoder.py',Path(decoder.__file__)),('board-preparation.py',Path(preparation.board.__file__)),
        ('dsi-value-profile.py',ROOT/'apps/dsi-scanout/profile.py'),('usb-access.py',ROOT/'tools/usb-access.py')]


def freeze_bundle(bundle):
    metadata=(bundle/'prepared.json').read_bytes();prepared=preparation.validate_bundle(bundle)
    require({k:v for k,v in prepared.items() if not k.startswith('_')}==json.loads(metadata),
        'Prepared DSI metadata changed during validation.')
    frozen={}
    for name,identity in preparation.board.snapshot_files(prepared).items():
        require(Path(name).name==name,'Invalid DSI bundle filename.')
        data=(bundle/name).read_bytes();require(hashlib.sha256(data).hexdigest()==identity,'DSI input changed before use: '+name)
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
    parser.add_argument('--present-slot',type=int,choices=(0,1),action='append',help='Replay slot order; default0,1,0')
    parser.add_argument('--claim');parser.add_argument('--owner')
    parser.add_argument('--supervised-manual-reconnect',action='store_true',
        help='Use the reviewed whole-board recovery plan for the retained board providers.')
    parser.add_argument('--database', type=Path)
    args=parser.parse_args(argv);bundle=args.bundle.resolve();prepared=freeze_bundle(bundle);frozen=prepared['_frozen']
    admission=json.loads(frozen['admission.json']);session=frozen['session.bin'];template=frozen['session-template.bin']
    evidence={path:frozen[item['name']] for path,item in prepared['entry_evidence'].items() if item['name'] in frozen}
    def validate_entry(identity,port):
        preparation.validate_admission(admission,prepared['image_sha256'],hashlib.sha256(template).hexdigest(),template,evidence,True)
        require(admission['identity']==identity and admission['port']==port,'DSI entry covers another instance.')
    return supervisor.run_session(args,bundle,prepared,
        lambda:claimed(args.database,args.claim,args.owner,bundle,prepared),validate_entry,
        lambda device,same,deadline,record,save:capture_device(device,same,deadline,session,args.evidence,record,save,
            frames=[frozen[f'frame{slot}.bin'] for slot in range(2)] if struct.unpack_from('<Q',session,600)[0]==2 else None,
            slots=args.present_slot or (0,1,0)),
        product=PRODUCT,serial=SERIAL,bcd_device=0x0100,capture_seconds=59,result_key='dsi_result',
        source_files=source_files(),description='Supervised native DSI controller observation; board providers require whole-board power removal')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,struct.error) as error:sys.exit(f'Stopped: {error}')
