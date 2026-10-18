#!/usr/bin/env python3
"""New runtime-only RTC outcome frames and retained writer authority."""
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
    out=Path(sys.argv[1]);folder=out/'host-runtime';folder.mkdir()
    tool=module('rtc_runtime_host',ROOT/'tools/check-a733-rtc-supervised.py');prepare=tool.preparation
    raw=(out/'session-template.bin').read_bytes();epoch,generation=struct.unpack_from('<2Q',raw,24)
    def frame(payload):return struct.pack('<4I2Q',0x31435452,3,1,104,epoch,generation)+hashlib.sha256(payload).digest()+payload
    results={}
    for variant,label in [('applied','observed-success'),('report-roll','observed-success'),('early-cancel','observed-cancelled'),('late-cancel','observed-applied-after-cancel'),('partial-day','observed-retained-failure')]:
        path=folder/variant;path.mkdir();payload=(out/(variant+'-runtime-copy.bin')).read_bytes();record={}
        session=(out/'rollover-session-template.bin').read_bytes() if variant=='report-roll' else raw
        result=tool.capture(lambda *_:frame(payload),session,path,record,lambda:None,1,now=lambda:0,pause=lambda _:None)
        assert record['rtc_result']==label and record['copied_rtc_output']['bytes']==104
        assert (path/'rtc-snapshot.bin').read_bytes()==payload;results[variant]=result
    for variant,offset,value in [('early-cancel',40,1),('late-cancel',52,0),('partial-day',36,6)]:
        payload=bytearray((out/(variant+'-runtime-copy.bin')).read_bytes());struct.pack_into('<I',payload,offset,value)
        changed=frame(payload);(folder/(variant+'-invalid.bin')).write_bytes(changed)
        try:tool.decoder.decode(changed,raw)
        except ValueError:pass
        else:raise AssertionError(variant)
    original=prepare.board.validate_admission;prepare.board.validate_admission=lambda record,image,template,cache,frozen:cache
    image_hash=hashlib.sha256((out/'build/sunstone-a733-rtc.elf').read_bytes()).hexdigest()
    try:
        for variant in ('nominal','writer-lifetime','remote-writer','calendar-target','stable-source'):
            facts={'clock':{'external_32768_stable':True,'source_writers_excluded':True,'exclusive_calendar_writer_lifetime':True,
                    'remote_calendar_writers_excluded':True,'calendar_write_authorized':True,'requested_day':20744,'requested_hms':0x123438},
                'initial':{'exclusive_rtc_and_rccu_word':True,'irq228_owned_disabled_inactive_unpending':True,'remote_writers_excluded':True,'other_cores_excluded':True,'inherited_alarm_disabled_unpending':True},
                'electrical':{'fitted_board_identified':True,'vcc_rtc_power_stable':True},
                'supervision':{'whole_board_power_removal':True,'permanent_ownership':True,'no_reuse':True,'one_attempt':True}}
            field={'writer-lifetime':'exclusive_calendar_writer_lifetime','remote-writer':'remote_calendar_writers_excluded','stable-source':'external_32768_stable'}.get(variant)
            if field:facts['clock'][field]=False
            if variant=='calendar-target':facts['clock']['requested_day']=20745
            session=bytearray(raw);cache={};record={'schema':'a733-rtc-supervised-entry-v1','identity':'modeled-target','port':'modeled-port'}
            for key,offset in zip(('initial','clock','electrical','supervision'),(632,664,696,728)):
                value={**facts[key],'schema':'a733-rtc-'+key+'-v1','identity':record['identity'],'port':record['port'],'image_sha256':image_hash,'reviewed':True}
                data=json.dumps(value,sort_keys=True).encode();path=(folder/(variant+'-'+key+'.json')).resolve();path.write_bytes(data)
                digest=hashlib.sha256(data).digest();cache[str(path)]=data;session[offset:offset+32]=digest;record[key]={'path':str(path),'sha256':digest.hex()}
            try:prepare.validate_admission(record,image_hash,hashlib.sha256(session).hexdigest(),session,cache,True)
            except ValueError:assert variant!='nominal'
            else:assert variant=='nominal'
    finally:prepare.board.validate_admission=original
    (out/'host-result.json').write_text(json.dumps({'passed':True,'outcomes':results,'scope':'Actual native runtime reports, synthetic USB framing, modeled common physical authority. No native USB/UTC/timing qualification.'},indent=2)+'\n')
    print('PASS distinct runtime RTC outcomes and retained writer admission')
if __name__=='__main__':main()
