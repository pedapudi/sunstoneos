#!/usr/bin/env python3
"""New pcie copied-output/session binding, durability and actual access routing."""
import argparse,hashlib,importlib.util,json,sqlite3,struct,sys,time,types
from pathlib import Path
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
    access=module('pcie_routing_access',ROOT/'tools/usb-access.py')
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
        def read(self,ep,n,timeout):assert ep==0x81 and n==192;return frame
    def find(**kw):
        assert kw==dict(idVendor=0x1f3a,idProduct=0xefe8,bus=1,address=3);calls.append(kw);return Device()
    def launch(transaction,observe,bundle,prepared,evidence,record,save,ready,*,result_key):
        assert result_key=='pcie_result';observe(time.monotonic());record['complete']=record[result_key]=='observed-success'
    with ExitStack() as stack:
        stack.enter_context(patch.object(host,'claimed',lambda *a:None));stack.enter_context(patch.object(shared,'module',lambda *a:access))
        stack.enter_context(patch.dict(sys.modules,{'usb_guard':guard}));stack.enter_context(patch.object(shared,'digest',lambda *a:shared.base.FEL_HASH))
        stack.enter_context(patch.object(shared.shutil,'copy2',lambda *a:None));stack.enter_context(patch.object(shared.os,'access',lambda *a:True))
        stack.enter_context(patch.object(shared.subprocess,'run',lambda *a,**k:fel.append(a) or types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')))
        stack.enter_context(patch.object(shared,'launch_once',launch));stack.enter_context(patch.object(shared,'finish_after_observation',lambda *a:None))
        stack.enter_context(patch.object(shared.base,'stop_after_failure',lambda *a:None));stack.enter_context(patch.object(usb.core,'find',find))
        args=['--bundle',str(bundle),'--fel',str(out/'MODEL-fel'),'--evidence',str(out/'routing-capture'),'--database',str(queue(out/'MODEL-db',port.name)),'--claim','MODEL','--owner','MODEL','--supervised-manual-reconnect']
        # A queue bound to another port is refused before the first FEL request.
        other=[x if x!=str(out/'routing-capture') else str(out/'other-port-capture') for x in args]
        other[other.index('--database')+1]=str(queue(out/'other-port-db','0-0'))
        try:host.main(other)
        except ValueError as error:assert 'is not bound to locked USB port' in str(error) and not fel
        else:raise AssertionError('Queue bound to another port accepted')
        assert host.main(args)==0
    assert len(calls)==1 and len(fel)==1

def run(bundle,frame_path,out,routing_only=False):
 out.mkdir(parents=True,exist_ok=False);host=module('pcie_host_checks',ROOT/'apps/pcie/capture.py');host.preparation.validate_bundle(bundle)
 session=(bundle/'session.bin').read_bytes();frame=bytearray(frame_path.read_bytes());frame[16:32]=session[24:40];frame[32:64]=hashlib.sha256(session).digest();frame=bytes(frame)
 if routing_only:
  routing(host,bundle,out,session,frame);(out/'result.json').write_text(json.dumps(dict(status='pass',case='actual-selector-discovery-routing',physical_access=False),indent=2)+'\n');return
 pending=struct.pack('<4I2Q',0x45494350,1,0,0,*struct.unpack_from('<2Q',session,24))+hashlib.sha256(session).digest()+bytes(64)
 calls=[];clock=[1.];record={}
 def exchange(packet,timeout):calls.append(1);assert packet==bytes(1);return pending if len(calls)==1 else frame
 host.capture(exchange,session,out,record,lambda:None,60,now=lambda:clock[0],pause=lambda d:clock.__setitem__(0,clock[0]+d))
 assert record['pcie_result']=='observed-success' and (out/'output.bin').read_bytes()==frame[128:]
 for offset in (0,4,8,12,16,24,32,64,96,128,160,191):
  bad=bytearray(frame);bad[offset]^=1
  try:host.decoder.decode(bad,session)
  except ValueError:pass
  else:raise AssertionError('Frame corruption accepted:'+str(offset))
 altered=bytearray(session);altered[648]^=1
 try:host.decoder.decode(frame,altered)
 except ValueError:pass
 else:raise AssertionError('Different admitted evidence accepted')
 failed=out/'durability-fault';failed.mkdir();record={}
 with patch.object(host.supervisor,'durable_frame',side_effect=OSError('modeled durability fault')):
  try:host.capture(lambda *a:frame,session,failed,record,lambda:None,60,now=lambda:1.)
  except OSError:pass
  else:raise AssertionError('Durability fault accepted')
 assert record['pcie_result']=='unobserved' and 'durable_result' not in record
 routing(host,bundle,out,session,frame)
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=['native-frame-pending-to-durable-success','frame-session-hash-refusals','durability-fault','actual-selector-discovery-routing'],physical_access=False),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('bundle','frame','output'):p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--routing-only',action='store_true')
 a=p.parse_args();run(a.bundle,a.frame,a.output,a.routing_only)
