#!/usr/bin/env python3
"""Only new explicit RTC target/source admission and initialized output frames."""
import copy
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
    out=Path(sys.argv[1]);folder=out/'host-initialization';folder.mkdir()
    tool=module('rtc_calendar_host',ROOT/'tools/check-a733-rtc-supervised.py');prepare=tool.preparation
    raw=(out/'session-template.bin').read_bytes();payload=(out/'initialized-copy.bin').read_bytes()
    epoch,generation=struct.unpack_from('<2Q',raw,24)
    good=struct.pack('<4I2Q',0x31435452,2,1,136,epoch,generation)+hashlib.sha256(payload).digest()+payload
    decoded=tool.decoder.decode(good,raw);assert decoded['initialization_flags']==3
    for name,offset in [('incomplete-stage',152+36),('changed-intent',152+32),('gain-change',152+16)]:
        changed=bytearray(good);changed[offset]^=4 if name=='gain-change' else 1;changed[32:64]=hashlib.sha256(changed[64:]).digest()
        (folder/(name+'.bin')).write_bytes(changed)
        try:tool.decoder.decode(changed,raw)
        except ValueError:pass
        else:raise AssertionError(name)
    record={};tool.capture(lambda *_:good,raw,folder,record,lambda:None,1,now=lambda:0,pause=lambda _:None)
    assert record['copied_rtc_output']['bytes']==136 and (folder/'rtc-snapshot.bin').read_bytes()==payload
    image_hash=hashlib.sha256((out/'build/sunstone-a733-rtc.elf').read_bytes()).hexdigest()
    original=prepare.board.validate_admission;prepare.board.validate_admission=lambda record,image,template,cache,frozen:cache
    try:
        for variant in ('nominal','source-authority','remote-consumer','calendar-authority','target'):
            fact={'schema':'a733-rtc-clock-v1','identity':'modeled-target','port':'modeled-port','image_sha256':image_hash,
                  'reviewed':True,'source_writers_excluded':True,'source_change_authorized':True,
                  'exclusive_osc32k_consumers':True,'remote_source_writers_excluded':True,'onboard_crystal_fitted':True,
                  'calendar_write_authorized':True,'requested_day':20744,'requested_hms':0x123438}
            if variant=='source-authority':fact['source_change_authorized']=False
            if variant=='remote-consumer':fact['exclusive_osc32k_consumers']=False
            if variant=='calendar-authority':fact['calendar_write_authorized']=False
            if variant=='target':fact['requested_day']=20745
            facts={'clock':fact,
                'initial':{'exclusive_rtc_and_rccu_word':True,'irq228_owned_disabled_inactive_unpending':True,'remote_writers_excluded':True,'other_cores_excluded':True,'inherited_alarm_disabled_unpending':True},
                'electrical':{'fitted_board_identified':True,'vcc_rtc_power_stable':True},
                'supervision':{'whole_board_power_removal':True,'permanent_ownership':True,'no_reuse':True,'one_attempt':True}}
            session=bytearray(raw);cache={};record={'schema':'a733-rtc-supervised-entry-v1','identity':'modeled-target','port':'modeled-port'}
            for key,offset in zip(('initial','clock','electrical','supervision'),(632,664,696,728)):
                value={**facts[key],'schema':'a733-rtc-'+key+'-v1','identity':record['identity'],'port':record['port'],'image_sha256':image_hash,'reviewed':True}
                data=json.dumps(value,sort_keys=True).encode();path=(folder/(variant+'-'+key+'.json')).resolve();path.write_bytes(data)
                digest=hashlib.sha256(data).digest();cache[str(path)]=data;session[offset:offset+32]=digest;record[key]={'path':str(path),'sha256':digest.hex()}
            try:prepare.validate_admission(record,image_hash,hashlib.sha256(session).hexdigest(),session,cache,True)
            except ValueError:assert variant!='nominal'
            else:assert variant=='nominal'
    finally:prepare.board.validate_admission=original
    assert prepare.calendar_target(2000,2,29,8,9,10)==(11017,0x08090a)
    try:prepare.calendar_target(2100,2,29,0,0,0)
    except ValueError:pass
    else:raise AssertionError('Invalid Gregorian leap day accepted')
    (out/'host-result.json').write_text(json.dumps({'passed':True,'scope':'Explicit source/calendar authority and copied initialization report; synthetic USB framing, no physical UTC/oscillator proof.'},indent=2)+'\n')
    print('PASS new RTC initialization host boundary')
if __name__=='__main__':main()
