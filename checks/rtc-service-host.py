#!/usr/bin/env python3
"""New RTC service copied transcript and registered-client authority boundaries."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    out=Path(sys.argv[1]);folder=out/'host-service';folder.mkdir()
    tool=module('rtc_service_host',ROOT/'tools/check-a733-rtc-supervised.py');prepare=tool.preparation
    raw=(out/'session-template.bin').read_bytes();epoch,generation=struct.unpack_from('<2Q',raw,24)
    def frame(payload):return struct.pack('<4I2Q',0x31435452,4,1,2416,epoch,generation)+hashlib.sha256(payload).digest()+payload
    results={}
    for variant,label in [('composition','observed-success'),('gic-delay','observed-success'),('partial','observed-retained-failure')]:
        path=folder/variant;path.mkdir();payload=(out/(variant+'-service-output.bin')).read_bytes();record={}
        result=tool.capture(lambda *_:frame(payload),raw,path,record,lambda:None,1,now=lambda:0,pause=lambda _:None)
        assert record['rtc_result']==label and (path/'rtc-snapshot.bin').read_bytes()==payload
        results[variant]=result
    nominal=(out/'composition-service-output.bin').read_bytes()
    for label,offset,value,size in [('duplicate-id',216,int.from_bytes(nominal[16:24],'little'),8),('client',24,2,4),('private-tail',2412,1,4),('incomplete',4,0,4),('calendar',16+24,0,4)]:
        payload=bytearray(nominal);payload[offset:offset+size]=value.to_bytes(size,'little');changed=frame(payload);(folder/(label+'.bin')).write_bytes(changed)
        try:tool.decoder.decode(changed,raw)
        except ValueError:pass
        else:raise AssertionError(label)
    try:tool.capture(lambda *_:None,raw,folder,{},lambda:None,1,now=lambda:0,cancel=True)
    except ValueError:pass
    else:raise AssertionError('unscoped host cancel')
    pending=struct.pack('<4I2Q',0x31435452,4,0,0,epoch,generation)+bytes(32)
    assert tool.decoder.decode(pending,raw)['status']==0
    changed=pending[:-1]+b'\1';(folder/'private-pending.bin').write_bytes(changed)
    try:tool.decoder.decode(changed,raw)
    except ValueError:pass
    else:raise AssertionError('private pending')
    old=prepare.board.validate_admission;prepare.board.validate_admission=lambda record,image,template,cache,frozen:cache
    image_hash=hashlib.sha256((out/'build/sunstone-a733-rtc.elf').read_bytes()).hexdigest()
    try:
        for variant in ('nominal','client-authority','writer-lifetime'):
            facts={'clock':{'external_32768_stable':True,'source_writers_excluded':True,'exclusive_calendar_writer_lifetime':True,'remote_calendar_writers_excluded':True,'registered_native_calendar_clients_authorized':variant!='client-authority','calendar_write_authorized':True,'requested_day':20744,'requested_hms':0x123438},
               'initial':{'exclusive_rtc_and_rccu_word':True,'irq228_owned_disabled_inactive_unpending':True,'remote_writers_excluded':True,'other_cores_excluded':True,'inherited_alarm_disabled_unpending':True},
               'electrical':{'fitted_board_identified':True,'vcc_rtc_power_stable':True},
               'supervision':{'whole_board_power_removal':True,'permanent_ownership':True,'no_reuse':True,'one_attempt':True}}
            if variant=='writer-lifetime':facts['clock']['exclusive_calendar_writer_lifetime']=False
            session=bytearray(raw);cache={};record={'schema':'a733-rtc-supervised-entry-v1','identity':'modeled-target','port':'modeled-port'}
            for key,offset in zip(('initial','clock','electrical','supervision'),(632,664,696,728)):
                value={**facts[key],'schema':'a733-rtc-'+key+'-v1','identity':record['identity'],'port':record['port'],'image_sha256':image_hash,'reviewed':True}
                data=json.dumps(value,sort_keys=True).encode();path=(folder/(variant+'-'+key+'.json')).resolve();path.write_bytes(data)
                digest=hashlib.sha256(data).digest();cache[str(path)]=data;session[offset:offset+32]=digest;record[key]={'path':str(path),'sha256':digest.hex()}
            try:prepare.validate_admission(record,image_hash,hashlib.sha256(session).hexdigest(),session,cache,True)
            except ValueError:assert variant!='nominal'
            else:assert variant=='nominal'
    finally:prepare.board.validate_admission=old
    (out/'host-result.json').write_text(json.dumps({'passed':True,'outcomes':results,'scope':'Actual real-FreeRTOS service payloads; synthetic USB framing and modeled common physical authority. Native serializer source/link only; no physical operation.'},indent=2)+'\n')
    print('PASS RTC service transcript, copied outcomes, identity and native-client authority')
if __name__=='__main__':main()
