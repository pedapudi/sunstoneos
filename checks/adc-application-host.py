#!/usr/bin/env python3
"""New adc copied-output/session binding, durability and actual access routing."""
import argparse,hashlib,importlib.util,json,sqlite3,struct,sys,time,types
from pathlib import Path
from harness import artifact_refused
from unittest.mock import patch
from contextlib import ExitStack,nullcontext
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def queue(path,port):
    """Model a board queue bound to one physical port, as the launch port check requires."""
    db=sqlite3.connect(path)
    with db:db.execute('CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL)');db.execute("INSERT INTO settings VALUES('port',?)",(port,))
    db.close();return path
def routing(host,bundle,out,session,frame):
    """Actual main and shared selector/lookup; target/transport authority is modeled."""
    import usb.core
    admission=json.loads((bundle/'admission.json').read_text())
    shared=host.supervisor;port=out/'sys'/admission['port'];port.mkdir(parents=True);old=out/'001/002';native=out/'001/003'
    for name,value in [('bcdDevice','0100'),('product',host.PRODUCT),('serial',host.SERIAL)]: (port/name).write_text(value)
    access=module('adc_routing_access',ROOT/'tools/usb-access.py')
    # Real discovery and access-rule selectors operate only on local fixture files.
    for name,value in [('idVendor','1f3a'),('idProduct','efe8'),('busnum','1'),('devnum','3')]: (port/name).write_text(value)
    access.SYS_USB=port.parent;access.DEV_USB=out
    access.board_lock=lambda:nullcontext(old);access.require_session=lambda n:None;access.require_rom=lambda n:None
    access.identity=lambda n:admission['identity'] if n==old else 'native';access.target=lambda n:port;access.quarantine_instance=lambda *a:None
    assert access.session_node(dict(port=admission['port'],sysfs=str(port.resolve())))==native
    assert 'ATTR{idProduct}=="efe8"' in access.session_rule('1-2',1000)
    guard=types.SimpleNamespace(__file__=ROOT/'tools/usb-access.py',check_health=lambda:None,reenumeration=lambda *a:nullcontext())
    calls=[];fel=[]
    class Device:
        bcdDevice=0x0100
        def set_configuration(self,n):assert n==1
        def write(self,ep,data,timeout):assert ep==2 and data==bytes(1);return 1
        def read(self,ep,n,timeout):assert ep==0x81 and n==288;return frame
    def find(**kw):
        assert kw==dict(idVendor=0x1f3a,idProduct=0xefe8,bus=1,address=3);calls.append(kw);return Device()
    def launch(transaction,observe,bundle,prepared,evidence,record,save,ready,*,result_key):
        assert result_key=='adc_result';observe(time.monotonic());record['complete']=record[result_key]=='observed-success'
    with ExitStack() as stack:
        stack.enter_context(patch.object(host,'claimed',lambda *a:None));stack.enter_context(patch.object(shared,'module',lambda *a:access))
        stack.enter_context(patch.dict(sys.modules,{'usb_guard':guard}));stack.enter_context(patch.object(shared,'digest',lambda *a:shared.base.FEL_HASH))
        stack.enter_context(patch.object(shared.shutil,'copy2',lambda *a:None));stack.enter_context(patch.object(shared.os,'access',lambda *a:True))
        stack.enter_context(patch.object(shared.subprocess,'run',lambda *a,**k:fel.append(a) or types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')))
        stack.enter_context(patch.object(shared,'launch_once',launch));stack.enter_context(patch.object(shared,'finish_after_observation',lambda *a:None))
        stack.enter_context(patch.object(shared.base,'stop_after_failure',lambda *a:None));stack.enter_context(patch.object(usb.core,'find',find))
        args=['--bundle',str(bundle),'--fel',str(out/'MODEL-fel'),'--evidence',str(out/'routing-capture'),'--supervised-manual-reconnect']
        assert host.main(args)==0
    assert len(calls)==1 and len(fel)==1

def direct_inputs(out,image=None):
    """Exercise direct electrical/ownership inputs and their concrete refusals."""
    from harness import refused,CLANG,LLD
    import copy
    host=module('adc_direct_host',ROOT/'apps/adc/capture.py');prep=host.preparation
    board_check=module('adc_direct_board_fixture',ROOT/'checks/board-supervised-host.py')
    out=prep.board.unique_directory(out)
    raw=bytearray(board_check.session(2)[0]+bytes(192))
    struct.pack_into('<3Q',raw,592,0x4153373341444131,1,784)
    struct.pack_into('<2Q',raw,616,83,89)
    for offset in (632,664,696,728):raw[offset:offset+32]=bytes([7])*32
    struct.pack_into('<2I',raw,760,0,24000000)
    raw[769]=13;struct.pack_into('<2I',raw,776,1800000,1000)
    target={'identity':'software-fixture','port':'no-physical-port','reservation':97}
    def conditions(facts):
        return {key:{**target,**dict.fromkeys(names,True)} for key,names in facts.items()}
    record={**target,**conditions(prep.FACTS)}
    record['electrical'].update(cldo3_uv=1800000,cldo3_selector=13,source_impedance_ohms=1000)
    record['clocks'].update(configure_adc_clock=0,apb1=0,apb1_hz=24000000)
    prep.session_check(raw)
    assert len(prep.validate_admission(copy.deepcopy(record),'no-reviewed-image','no-reviewed-template',raw))==4
    bad=copy.deepcopy(record);bad['initial']['remote_secure_writers_excluded']=False
    refused(lambda:prep.validate_admission(bad,'','',raw),'Required ADC physical fact absent')
    bad=copy.deepcopy(record);bad['electrical']['cldo3_uv']=3300000
    refused(lambda:prep.validate_admission(bad,'','',raw),'ADC supply/impedance authority differs')
    bad_raw=bytearray(raw);struct.pack_into('<I',bad_raw,760,1<<23)
    refused(lambda:prep.session_check(bad_raw),'Invalid ADC APB1 profile')
    lradc=bytearray(raw);struct.pack_into('<Q',lradc,600,2)
    lr={**target,**conditions(prep.LRADC_FACTS)}
    lr['electrical'].update(record['electrical'])
    lr['clocks'].update(record['clocks'])
    lr['electrical']['lradc_idle_electrical_authority']={**target,
        'basis':'independent-board-electrical-determination','nominal_pullup_uv':1800000,
        'documented_conversion_max_uv':1350000,'safe_idle_overrange_at_admitted_avcc':True,
        'not_inferred_from_saturation_or_keymap':True}
    prep.session_check(lradc);prep.validate_admission(copy.deepcopy(lr),'','',lradc)
    lr['electrical']['lradc_idle_electrical_authority']['safe_idle_overrange_at_admitted_avcc']=False
    refused(lambda:prep.validate_admission(lr,'','',lradc),'LRADC idle overrange lacks independent electrical authority')
    packaged=False
    if image is not None:
        template=out/'session-template.bin';template.write_bytes(raw)
        inputs=out/'inputs.json';inputs.write_text(json.dumps(record))
        prepared=prep.prepare(image,None,inputs,template,out/'prepared',CLANG,LLD)
        assert json.loads((out/'prepared'/'image-input-manifest.json').read_bytes())=={}
        assert len(prepared['entry_evidence'])==4
        artifact_refused(lambda:prep.prepare(image,None,inputs,template,out/'prepared',CLANG,LLD),out/'prepared')
        for name in ('launch.c','launch.S','launch.ld','boot.h','launcher-build.log','launcher-commands.json','image-input-manifest.json'):
            archived=out/'prepared'/name
            if archived.exists():
                archived.write_bytes(archived.read_bytes()+b'changed archived provenance')
                prep.validate_bundle(out/'prepared')
                archived.rename(out/('archived-'+name))
                prep.validate_bundle(out/'prepared')
        snapshot=host.freeze_bundle(out/'prepared')
        assert 'launch.c' not in snapshot['_frozen'] and 'image-input-manifest.json' not in snapshot['_frozen']
        host.claimed(None,None,None,out/'prepared',snapshot)
        runtime=out/'prepared'/'session.bin';runtime.write_bytes(runtime.read_bytes()+b'changed')
        refused(lambda:prep.validate_bundle(out/'prepared'),'ADC bundle input changed')
        refused(lambda:host.claimed(None,None,None,out/'prepared',snapshot),'Prepared input changed before upload')
        packaged=True
    (out/'direct-input-results.json').write_text(json.dumps({'status':'PASS','physical_access':False,
        'cases':['ADC-direct-inputs-without-schema-or-review','remote-writer-refusal','supply-mismatch-refusal',
                 'unsupported-clock-refusal','LRADC-direct-electrical-rating','unsafe-idle-overrange-refusal','archive-mutation-and-removal-accepted','uploaded-session-mutation-refused'],
        'packaged_without_manifest':packaged,'existing_output_refused_without_changes':packaged},indent=2)+'\n')
    print('PASS: direct ADC/LRADC inputs retain ownership, supply, clock and overrange checks')

def run(bundle,frame_path,out,routing_only=False):
 out.mkdir(parents=True,exist_ok=False);host=module('adc_host_checks',ROOT/'apps/adc/capture.py');host.preparation.validate_bundle(bundle)
 session=(bundle/'session.bin').read_bytes();frame=bytearray(frame_path.read_bytes());frame[16:32]=session[24:40];frame[32:64]=hashlib.sha256(session).digest();frame=bytes(frame)
 if routing_only:
  routing(host,bundle,out,session,frame);(out/'result.json').write_text(json.dumps(dict(status='pass',case='actual-selector-discovery-routing',physical_access=False),indent=2)+'\n');return
 pending=struct.pack('<4I2Q',0x31434441,1,0,0,*struct.unpack_from('<2Q',session,24))+hashlib.sha256(session).digest()+bytes(64)
 calls=[];clock=[1.];record={}
 def exchange(packet,timeout):calls.append(1);assert packet==bytes(1);return pending if len(calls)==1 else frame
 host.capture(exchange,session,out,record,lambda:None,60,now=lambda:clock[0],pause=lambda d:clock.__setitem__(0,clock[0]+d))
 assert record['adc_result']=='observed-success' and (out/'output.bin').read_bytes()==frame[128:]
 for offset in (0,4,8,12,16,24,32,64,96,128,160,287):
  bad=bytearray(frame);bad[offset]^=1
  try:host.decoder.decode(bad,session)
  except ValueError:pass
  else:raise AssertionError('Frame corruption accepted:'+str(offset))
 if struct.unpack_from('<Q',session,600)[0]==3:
  assert host.decoder.decode(frame,session)['compare_window']['selected_mask']==session[770]
  for offset in (152,156):
   bad=bytearray(frame);struct.pack_into('<I',bad,offset,16);bad[64:96]=hashlib.sha256(bad[128:]).digest()
   try:host.decoder.decode(bad,session)
   except ValueError:pass
   else:raise AssertionError('Unselected compare event accepted')
 altered=bytearray(session);altered[632]^=1
 try:host.decoder.decode(frame,altered)
 except ValueError:pass
 else:raise AssertionError('Different admitted evidence accepted')
 failed=out/'durability-fault';failed.mkdir();record={}
 with patch.object(host.supervisor,'durable_frame',side_effect=OSError('modeled durability fault')):
  try:host.capture(lambda *a:frame,session,failed,record,lambda:None,60,now=lambda:1.)
  except OSError:pass
  else:raise AssertionError('Durability fault accepted')
 assert record['adc_result']=='unobserved' and 'durable_result' not in record
 routing(host,bundle,out,session,frame)
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=['native-frame-pending-to-durable-success','frame-session-hash-refusals','durability-fault','actual-selector-discovery-routing'],physical_access=False),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('bundle','frame'):p.add_argument('--'+key,type=Path)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--inputs-only',action='store_true');p.add_argument('--image',type=Path)
 p.add_argument('--routing-only',action='store_true')
 a=p.parse_args()
 if a.inputs_only:direct_inputs(a.output,a.image)
 else:
  if a.bundle is None or a.frame is None:p.error('--bundle and --frame are required for capture checks')
  run(a.bundle,a.frame,a.output,a.routing_only)
