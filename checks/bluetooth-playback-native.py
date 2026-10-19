#!/usr/bin/env python3
"""Run changed playback/USB/codec/provider instructions with explicit device models."""
import argparse,hashlib,importlib.util,json,shutil,struct,traceback
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__)
for n in ('image','producer','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--case',action='append',choices=['two-periods','usb-ownership','revocation','publication','partial-deadline','host-two','host-stop','host-duplex','profile-gates'])
a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=True)
image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest()
if (out/'results.json').exists():assert json.loads((out/'results.json').read_text())['image_sha256']==image_hash,'Refused different-image result reuse'
base=module('playback_usb_model',ROOT/'checks/bluetooth-usb-native.py');base.ELF_HASH=image_hash
shutil.copy2(a.image,out/'bluetooth.elf')
old=Path.home()/'.cache/sunstoneos/evidence/bluetooth-usb-native-20261018'
for n in ('transcript.bin','snapshot.bin'):shutil.copy2(old/n,out/n)
profile=bytearray((a.producer/'duplex-copied.pcm.profile.bin').read_bytes());struct.pack_into('<2I',profile,8,2,752);profile=profile[:752];struct.pack_into('<I',profile,624,4)
session=bytearray((old/'admitted-session.bin').read_bytes());session[320:352]=hashlib.sha256(profile).digest()
(out/'profile.bin').write_bytes(profile);(out/'admitted-session.bin').write_bytes(session)
caller=(a.producer/'duplex-copied.pcm.caller.bin').read_bytes();admission=json.loads((a.producer/'duplex-copied.pcm.admission.json').read_text())
assert len(caller)==4800 and caller[:2400]!=caller[2400:]
with a.image.open('rb') as f:
 elf=ELFFile(f);dw=elf.get_dwarf_info();syms={};private={};vars={};layout={}
 for s in elf.get_section_by_name('.symtab').iter_symbols():
  if s.name in ('bluetooth_playback_mode','bluetooth_headset_wideband','bluetooth_headset_repeated','bluetooth_playback_step','bluetooth_playback_write','bluetooth_playback_commit','bluetooth_playback_copy','bluetooth_playback_ack','bluetooth_playback_stop','bluetooth_playback_end','bluetooth_playback_closed','bluetooth_playback_done','bluetooth_playback_cancelled','bt_sco_close','bluetooth_msbc_init','bluetooth_msbc_byte','bluetooth_msbc_encode','malloc','free','vPortEnterCritical','vPortExitCritical','vTaskEnterCritical','vTaskExitCritical','a733_smp_build_id'):
   assert s.name not in syms;sym=int(s['st_value']);syms[s.name]=sym
 def resolved(d):
  while d.tag in ('DW_TAG_typedef','DW_TAG_const_type','DW_TAG_volatile_type','DW_TAG_atomic_type'):d=d.get_DIE_from_attribute('DW_AT_type')
  return d
 def members(d):
  return {c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in resolved(d).iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name'in c.attributes}
 for cu in dw.iter_CUs():
  unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
  for d in cu.iter_DIEs():
   name=d.attributes.get('DW_AT_name');name=name.value.decode() if name else None
   if unit.endswith('/drivers/bluetooth/bt_sco.c') and d.tag=='DW_TAG_subprogram' and name in ('prepare','credits_valid','credits_commit','disconnect_valid','disconnect_commit') and 'DW_AT_low_pc'in d.attributes:private[name]=int(d.attributes['DW_AT_low_pc'].value)
   if unit.endswith('/apps/bluetooth/playback.c') and d.tag=='DW_TAG_variable' and name=='playback':
    ops=DWARFExprParser(dw.structs).parse_expr(d.attributes['DW_AT_location'].value);assert len(ops)==1
    vars['playback']=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dw.get_addr(cu,ops[0].args[0]);t=resolved(d.get_DIE_from_attribute('DW_AT_type'));layout=members(t)
    for c in t.iter_children():
     if c.attributes.get('DW_AT_name',type('V',(),{'value':b''})()).value==b'slots':
      slot=resolved(c.get_DIE_from_attribute('DW_AT_type')).get_DIE_from_attribute('DW_AT_type');layout['slot_size']=int(resolved(slot).attributes['DW_AT_byte_size'].value);layout['slot_fields']=members(slot)
   if unit.endswith('/apps/bluetooth/usb0.c') and d.tag=='DW_TAG_variable' and name in ('playback_delivered','playback_transfer','playback_payload_bytes','playback_header_active'):
    ops=DWARFExprParser(dw.structs).parse_expr(d.attributes['DW_AT_location'].value);vars[name]=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dw.get_addr(cu,ops[0].args[0])
assert len(private)==5 and 'slot_fields'in layout
observe=module('playback_observe',ROOT/'apps/bluetooth/observe.py');results=json.loads((out/'results.json').read_text())['cases'] if (out/'results.json').exists() else {}
def create(name):
 m=base.Machine(out,name);m.s.update(syms);m.s.update({'sco_'+k:v for k,v in private.items()});m.pb=vars['playback'];m.pf=layout;m.owner=0x889900
 m.field('bluetooth_application','bluetooth','owner',m.owner,8);m.field('bluetooth_application','bluetooth','deadline',10000000,8)
 m.field('bluetooth_application','bluetooth','firmware_generation',admission['firmware'],8);m.field('bluetooth_application','bluetooth','uart_generation',admission['uart'],8)
 m.allocations=[];m.critical=0;m.stop_publication=False
 def hooks(cpu,address,size,unused):
  if address==syms['malloc']:
   pointer=0x50a00000+len(m.allocations)*0x10000;m.allocations.append(pointer);m.ret(pointer)
  elif address==syms['free']:raise AssertionError('Session codec state unexpectedly freed')
  elif address==syms.get('vPortEnterCritical',syms.get('vTaskEnterCritical')):
   if m.stop_publication:
    m.stop_publication=False;m.write(m.pb+layout['cancelled'],1);m.write(m.pb+layout['sealed'],1)
   m.critical+=1;m.ret()
  elif address==syms.get('vPortExitCritical',syms.get('vTaskExitCritical')):assert m.critical;m.critical-=1;m.ret()
  elif address in (0x50fff100,0x50fff200):m.ret(1)
  elif address==0x50fff300:
   ptr=cpu.reg_read(UC_ARM64_REG_X0+2);m.cpu.mem_write(ptr,struct.pack('<QI4x',admission['security'],0));m.ret(1)
 m.cpu.hook_add(UC_HOOK_CODE,hooks)
 return m
def packet(op,ticket=0,offset=0,payload=b''):return struct.pack('<BBHIQ',0x81,op,len(payload),offset,ticket)+payload
def slot(m,i):return m.pb+layout['slots']+i*layout['slot_size']
def upload(m,ticket):
 m.task=0x123400;ptr=0x50de0000;data=caller[(ticket-1)*2400:ticket*2400];m.cpu.mem_write(ptr,data)
 for offset,n in ((0,1024),(1024,1024),(2048,352)):assert m.call('bluetooth_playback_write',m.s['bluetooth'],ticket,offset,ptr+offset,n)==1
 m.cpu.mem_write(ptr+4096,hashlib.sha256(data).digest());assert m.call('bluetooth_playback_commit',m.s['bluetooth'],ticket,ptr+4096)==1
 return ptr

def provider(m):
 # Admission values come from the actual native-source producer's authenticated
 # HFP/eSCO sequence. Pointer translation and already-open device state are modeled.
 v=m.variables['voice'];vf=v['members'];v=v['address'];q=v+vf['audio'];acl=v+vf['acl'];ag=v+vf['ag'];rf=v+vf['rfcomm'];reg=v+vf['registry'];l2=v+vf['l2cap'];hci=m.s['bluetooth']+m.t['bluetooth_application']['hci']
 def put(t,p,n,x,width=8):m.write(p+m.t[t][n],x,width)
 enums=m.report['enums'];put('bt_sco',q,'self',q);put('bt_sco',q,'initialized',1,4);put('bt_sco',q,'acl',acl)
 for n,x,w in [('capacity',admission['capacity'],4),('credits',admission['capacity'],4),('mtu',admission['mtu'],4),('next_packet',1,8),('active_tx',8,4),('binding.current',0x50fff100,8),('binding.security',0x50fff300,8)]:put('bt_sco',q,n,x,w)
 put('bt_acl',acl,'hci',hci);put('bt_acl',acl,'state',enums['BT_ACL_ACTIVE'],4);put('bt_acl',acl,'synchronous_context',q)
 for n,x,w in [('self',hci,8),('acl_owner',acl,8),('synchronous_owner',q,8),('synchronous_ready',1,4),('transport.state',enums['BT_H4_ACTIVE'],4),('transport.binding.uart',0x50b00000,8)]:put('bt_hci',hci,n,x,w)
 link=q+m.t['bt_sco']['links']
 for n,x,w in [('generation',admission['audio'],8),('phase',enums['BT_SCO_OPEN'],4),('ag',ag,8),('acl_generation',admission['acl'],8),('channel',7,8),('security.generation',admission['security'],8),('handle_valid',1,4),('handle',admission['handle'],2),('codec',2,4)]:put('bt_sco_link',link,n,x,w)
 parent=acl+m.t['bt_acl']['links']
 for n,x,w in [('generation',admission['acl'],8),('state',enums['BT_ACL_CONNECTED'],4),('handle_valid',1,4)]:put('bt_acl_link',parent,n,x,w)
 for n,x,w in [('phase',enums['BT_HFP_AG_READY'],4),('rfcomm',rf,8),('registry',reg,8),('link',admission['acl'],8),('channel',7,8),('local_codecs',6,4),('hf_features',128,4),('selected_codec',2,4)]:put('bt_hfp_ag',ag,n,x,w)
 for n,x,w in [('phase',enums['BT_RFCOMM_OPEN'],4),('registry',reg,8),('link',admission['acl'],8),('channel',7,8),('l2cap',l2,8)]:put('bt_rfcomm',rf,n,x,w)
 put('bt_l2cap',l2,'acl',acl);put('bt_rfcomm_registry',reg,'session',rf);put('bt_rfcomm_registry',reg,'authorize_service',0x50fff200)
 m.q=q;m.hci=hci;m.put=put;return q

def transfer(m):
 q=m.q;tx=q+m.t['bt_sco']['tx'];size=m.report['sizes']['bt_sco_packet'];frames=[]
 m.task=m.owner
 for turn in range(100):
  now=1000+turn*7500;m.put('bt_hci',m.hci,'transport.now',now)
  assert m.call('bluetooth_playback_step',m.s['bluetooth'],q,admission['acl'],admission['audio'],now,9000000)==1
  for i in range(8):
   t=tx+i*size
   if m.word(t+m.t['bt_sco_packet']['state'])!=m.report['enums']['BT_SCO_QUEUED']:continue
   length=m.word(t+m.t['bt_sco_packet']['length'],8);assert length==60;frames.append(bytes(m.cpu.mem_read(t+m.t['bt_sco_packet']['bytes'],length)))
   # Model acceptance of all UART bytes, then run the actual provider observation and
   # actual validated completed-packet dispatcher, rather than replacing send.
   m.put('bt_sco_packet',t,'state',m.report['enums']['BT_SCO_PARTIAL'],4);m.put('bt_sco',q,'active_tx',i,4);m.put('bt_sco',q,'credits',admission['capacity']-1,4)
   m.put('bt_hci',m.hci,'transport.tx_payload',t+m.t['bt_sco_packet']['bytes']);m.put('bt_hci',m.hci,'transport.tx_offset',length,8);m.put('bt_hci',m.hci,'transport.tx_length',length,8)
   assert m.call('sco_prepare',q)==1
   ptr=0x50de9000;m.cpu.mem_write(ptr,struct.pack('<HH',admission['handle'],1));assert m.call('sco_credits_valid',q,ptr,1)==1;m.call('sco_credits_commit',q,ptr,1)
  if m.word(slot(m,1)+layout['slot_fields']['state'])==2:break
 assert len(frames)==20 and m.word(slot(m,0)+layout['slot_fields']['state'])==2 and m.word(slot(m,1)+layout['slot_fields']['state'])==2,(len(frames),m.word(slot(m,0)+layout['slot_fields']['state']),m.word(slot(m,1)+layout['slot_fields']['state']))
 return b''.join(frames)

for name in a.case or ['two-periods','usb-ownership','revocation','publication','partial-deadline']:
 if name in results:assert results[name]['passed'];continue
 profile=bytearray((out/'profile.bin').read_bytes());session=bytearray((out/'admitted-session.bin').read_bytes())
 m=create(name)
 try:
  if name=='profile-gates':
   original=(a.producer/'duplex-copied.pcm.profile.bin').read_bytes();checked=[]
   for version,mode,expected in [(1,4,(0,0,0)),(1,5,(0,0,0)),(2,1,(0,0,0)),(3,1,(0,0,0)),(3,2,(0,0,1)),(3,3,(0,1,1)),(2,4,(1,1,0)),(3,5,(1,1,1)),(2,5,(0,0,0)),(3,4,(0,0,0))]:
    data=bytearray(original);struct.pack_into('<2I',data,8,version,{1:624,2:752,3:832}[version]);struct.pack_into('<I',data,624,mode);m.cpu.mem_write(0x43000000,bytes(data))
    def forbidden(cpu,access,address,size,value,unused):
     assert not(version==1 and address<0x43000000+832 and address+size>0x43000000+624),'Read unauthenticated v1 headset suffix'
    hook=m.cpu.hook_add(UC_HOOK_MEM_READ,forbidden)
    actual=tuple(m.call(fn,m.s['bluetooth']) for fn in ('bluetooth_playback_mode','bluetooth_headset_wideband','bluetooth_headset_repeated'));assert actual==expected,(version,mode,actual)
    if version==1:assert m.call('bluetooth_playback_write',m.s['bluetooth'],1,0,0x50de0000,1)&0xffffffff==0xffffffff
    m.cpu.hook_del(hook);checked.append({'version':version,'mode':mode,'classification':actual})
   for mode in (4,5):
    fresh=create(name+'-'+str(mode));data=bytearray(original);struct.pack_into('<I',data,624,mode)
    if mode==4:struct.pack_into('<2I',data,8,2,752)
    fresh.cpu.mem_write(0x43000000,bytes(data));upload(fresh,1);assert fresh.word(fresh.pb+layout['accepted'],8)==1 and fresh.word(slot(fresh,0)+layout['slot_fields']['state'])==1
   results[name]={'passed':True,'classifications':checked,'v1_suffix_read':False,'both_new_modes_accept_exact_pcm_commit':True,'scope':'Actual final linked version/mode predicates and copied-input publication only; unchanged codec/provider/USB evidence retained on its closed earlier ELF.'}
  elif name=='two-periods':
   assert m.call('bluetooth_msbc_init')==1;upload(m,1);upload(m,2);assert m.call('bluetooth_playback_end',m.s['bluetooth'],0)==1;q=provider(m)
   raw=transfer(m);(out/'caller-encoded.msbc').write_bytes(raw)
   # Compare encoder payloads with the actual stack producer. That producer has
   # one initial silence frame, so its H2 sequence is one ahead of this prebuffered case.
   reference=(a.producer/'duplex-copied.pcm.outgoing.bin').read_bytes();assert len(reference)>=1260
   assert all(raw[i*60+2:i*60+60]==reference[(i+1)*60+2:(i+1)*60+60] for i in range(20))
   m.task=0x123400;ptr=0x50df0000
   for ticket in (1,2):
    assert m.call('bluetooth_playback_copy',m.s['bluetooth'],ptr,ptr+1024)==1
    r=m.t['bluetooth_playback_result'];assert m.word(ptr+r['ticket'],8)==ticket and m.word(ptr+r['encoded_samples'],8)==1200 and m.word(ptr+r['submitted_bytes'],8)==600 and m.word(ptr+r['uart_packets'],8)==m.word(ptr+r['released_packets'],8)==10
    assert bytes(m.cpu.mem_read(ptr+r['sha256'],32))==hashlib.sha256(caller[(ticket-1)*2400:ticket*2400]).digest()
    assert m.call('bluetooth_playback_ack',m.s['bluetooth'],ticket)==1
   results[name]={'passed':True,'frames':20,'actual_provider_send_and_credit_callbacks':True,'allocations':len(m.allocations)}
  elif name=='publication':
   ptr=0x50de0000;m.cpu.mem_write(ptr,caller[:2400]);
   for offset,n in ((0,1024),(1024,1024),(2048,352)):assert m.call('bluetooth_playback_write',m.s['bluetooth'],1,offset,ptr+offset,n)==1
   m.cpu.mem_write(ptr+4096,hashlib.sha256(caller[:2400]).digest());m.stop_publication=True
   assert m.call('bluetooth_playback_commit',m.s['bluetooth'],1,ptr+4096)&0xffffffff==0xffffffff and not m.word(m.pb+layout['accepted'],8) and not m.word(slot(m,0)+layout['slot_fields']['state'])
   results[name]={'passed':True,'modeled_critical_section_stop_before_publication':True}
  elif name=='partial-deadline':
   m.enumerate();m.request(packet(1,1,0,b'x')[:16]);m.time+=200000;assert not m.tick();assert not m.word(m.pb+layout['accepted'],8)
   results[name]={'passed':True,'unpublished_partial_request_retained':True}
  elif name=='revocation':
   m.enumerate();m.field('bluetooth_application','bluetooth','failed',1);m.request(packet(0));frame=m.drain(332);value=observe.decode_playback(frame,session,profile)
   assert value['state']==2 and not any(frame[64:192]) and not any(frame[200:296]);results[name]={'passed':True,'suppressed_failure_response':True}
  elif name=='usb-ownership':
   m.enumerate()
   def exchange_input(data):
    for start in range(0,len(data),512):m.request(data[start:start+512])
    return observe.decode_playback(m.drain(332),session,profile)
   for ticket in (1,2):
    data=caller[(ticket-1)*2400:ticket*2400]
    for offset in (0,1024,2048):assert exchange_input(packet(1,ticket,offset,data[offset:offset+1024]))['outcome']==1
    assert exchange_input(packet(2,ticket,payload=hashlib.sha256(data).digest()))['outcome']==1
   assert exchange_input(packet(4))['outcome']==1
   assert m.call('bluetooth_msbc_init')==1;provider(m);transfer(m);m.task=0x123400
   m.request(packet(0));assert m.word(slot(m,0)+layout['slot_fields']['state'])==2 and not m.word(vars['playback_delivered'],8)
   # Queued USB bytes alone cannot authorize ACK; calling the copied API does
   # not replace the actual USB delivery guard in the private parser.
   assert m.word(vars['playback_transfer'],8)==1;frame=m.drain(332);value=observe.decode_playback(frame,session,profile);assert value['ticket']==1 and m.word(vars['playback_delivered'],8)==1
   m.request(packet(3,1));frame=m.drain(332);value=observe.decode_playback(frame,session,profile);assert value['ticket']==2 and m.word(slot(m,0)+layout['slot_fields']['state'])==0
   m.request(packet(3,1));frame=m.drain(332);value=observe.decode_playback(frame,session,profile);assert value['outcome']==3 and m.word(slot(m,1)+layout['slot_fields']['state'])==2
   results[name]={'passed':True,**m.observation()}
  elif name in ('host-two','host-stop','host-duplex'):
   host=module('playback_host_integration',ROOT/'tools/check-a733-bluetooth-supervised.py')
   if name=='host-duplex':
    profile=bytearray((a.producer/'duplex-copied.pcm.profile.bin').read_bytes());struct.pack_into('<I',profile,720,24000);session[320:352]=hashlib.sha256(profile).digest()
    m.cpu.mem_write(0x43000000,bytes(profile));m.cpu.mem_write(m.s['board']+m.t['board_application']['session'],bytes(session[:592]));m.field('board_application','board','session.deadline_us',10000000,8)
    voice=m.variables['voice']['address'];vf=m.variables['voice']['members'];m.write(voice+vf['segment_app'],m.s['bluetooth'],8);m.write(voice+vf['app'],m.s['bluetooth'],8)
    m.write(voice+vf['segment_head'],2,8);m.write(voice+vf['segment_tail'],2,8);adopted=0
    def adopt_transcript():
     global adopted
     if not m.word(m.variables['terminal']['address']) and adopted<2:
      adopted+=1;prior=Path.home()/'.cache/sunstoneos/evidence/bluetooth-msbc-20261019/recognizer'
      text=(prior/f'segment-{adopted}.txt').read_bytes();snapshot=(prior/f'segment-{adopted}.snapshot.bin').read_bytes()
      m.cpu.mem_write(m.service+m.sf['log'],text);m.cpu.mem_write(m.service+m.sf['segment'],snapshot);m.write(m.service+m.sf['used'],len(text));m.write(m.variables['terminal']['address'],1)
    adopt_transcript()
   assert m.call('bluetooth_msbc_init')==1;provider(m);m.enumerate();closed=False;forced=False
   def owner_progress():
    global closed
    q=m.q;m.task=m.owner;now=m.time;m.put('bt_hci',m.hci,'transport.now',now)
    assert m.call('bluetooth_playback_step',m.s['bluetooth'],q,admission['acl'],admission['audio'],now,9000000)==1
    tx=q+m.t['bt_sco']['tx'];size=m.report['sizes']['bt_sco_packet']
    for i in range(8):
     t=tx+i*size
     if m.word(t+m.t['bt_sco_packet']['state'])!=m.report['enums']['BT_SCO_QUEUED']:continue
     length=m.word(t+m.t['bt_sco_packet']['length'],8)
     m.put('bt_sco_packet',t,'state',m.report['enums']['BT_SCO_PARTIAL'],4);m.put('bt_sco',q,'active_tx',i,4);m.put('bt_sco',q,'credits',admission['capacity']-1,4)
     m.put('bt_hci',m.hci,'transport.tx_payload',t+m.t['bt_sco_packet']['bytes']);m.put('bt_hci',m.hci,'transport.tx_offset',length,8);m.put('bt_hci',m.hci,'transport.tx_length',length,8)
     assert m.call('sco_prepare',q)==1
     ptr=0x50de9000;m.cpu.mem_write(ptr,struct.pack('<HH',admission['handle'],1));assert m.call('sco_credits_valid',q,ptr,1)==1;m.call('sco_credits_commit',q,ptr,1)
    if not closed and (m.call('bluetooth_playback_done',m.s['bluetooth']) or m.call('bluetooth_playback_cancelled',m.s['bluetooth'])):
     assert m.call('bt_sco_close',q,admission['audio'],9000000)==1
     link=q+m.t['bt_sco']['links'];m.put('bt_sco_link',link,'phase',m.report['enums']['BT_SCO_DISCONNECT_WAIT'],4);m.put('bt_sco_link',link,'acknowledgement',100)
     ptr=0x50de9000;record=ptr+1024;m.cpu.mem_write(ptr,bytes((5,4,0,admission['handle'],0,0x13)))
     m.write(record+m.t['bt_h4_record']['storage.bytes'],ptr,8);m.write(record+m.t['bt_h4_record']['serial'],101,8)
     assert m.call('sco_disconnect_valid',q,record)==1;m.call('sco_disconnect_commit',q,record);m.call('bluetooth_playback_closed',m.s['bluetooth']);closed=True
    m.task=0x123400
   def exchange(data,timeout):
    global forced
    assert timeout>0
    if name=='host-stop' and not forced and data[1]==1 and struct.unpack_from('<Q',data,8)[0]==2:
     m.task=m.owner;m.call('bluetooth_playback_stop',m.s['bluetooth']);m.task=0x123400;forced=True
    if data[0]==0x80:
     assert name=='host-duplex';adopt_transcript()
     m.request(data);extent=272+(struct.unpack_from('<H',data,2)[0] if data[1]==1 else 0);frame=m.drain(extent)
     if closed and adopted==2 and not m.word(m.variables['terminal']['address']):m.write(m.variables['finished']['address'],1)
     owner_progress();return frame
    for start in range(0,len(data),512):m.request(data[start:start+512])
    frame=m.drain(332);owner_progress()
    if name=='host-duplex' and closed:m.write(voice+vf['segment_disconnected'],1)
    return frame
   destination=out/name;destination.mkdir(exist_ok=True);record={}
   result=host.capture_playback(exchange,session,profile,destination,record,lambda:None,8.,caller,bytes(m.cpu.mem_read(syms['a733_smp_build_id'],32)).hex(),lambda:m.time/1000000,lambda seconds:None)
   assert (name!='host-duplex' or len(result['transcripts'])==2) and len(result['periods'])==(1 if name=='host-stop' else 2) and record['bluetooth_result']==('observed-failure' if name=='host-stop' else 'observed-success')
   results[name]={'passed':True,'periods':len(result['periods']),'host_result':record['bluetooth_result'],'actual_usb_and_owner_code':True,'matched_disconnect_callback':closed}
  else:raise AssertionError(name)
 except BaseException:
  failure=out/(name+'-failure.log')
  if failure.exists():shutil.copy2(failure,out/('prior-'+hashlib.sha256(failure.read_bytes()).hexdigest()+'.log'))
  failure.write_text(traceback.format_exc());raise
 (out/'results.json').write_text(json.dumps({'image_sha256':image_hash,'physical_access':False,'cases':results,'scope':'Actual linked private playback, libsbc, SCO send/admission/result/credit callbacks, USB serializer/TinyUSB. Scheduler/critical exclusion, UART byte acceptance, controller state/events and DCD/provider-ready effects modeled; no physical transport/rendering qualification.'},indent=2)+'\n')
print('PASS selected actual linked caller playback cases')
