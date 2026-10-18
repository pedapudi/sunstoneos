#!/usr/bin/env python3
"""Only new RTC copied framing/capture and physical authority refusals."""
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
    out=Path(sys.argv[1]);evidence=out/'host-capture';evidence.mkdir()
    tool=module('rtc_host_tool',ROOT/'tools/check-a733-rtc-supervised.py');decoder=tool.decoder
    session=(out/'session-template.bin').read_bytes();payload=(out/'copied-snapshot.bin').read_bytes()
    epoch,generation=struct.unpack_from('<2Q',session,24)
    header=struct.pack('<4I2Q',0x31435452,1,1,88,epoch,generation)
    good=header+hashlib.sha256(payload).digest()+payload
    pending=struct.pack('<4I2Q',0x31435452,1,0,0,epoch,generation)+bytes(32)
    assert decoder.decode(good,session)['alarm_seen']==1
    failures=[]
    for name,offset in [('magic',0),('identity',16),('digest',32),('calendar',64+4*4)]:
        raw=bytearray(good);raw[offset]^=1
        if name=='calendar':raw[32:64]=hashlib.sha256(raw[64:]).digest()
        (evidence/(name+'.bin')).write_bytes(raw)
        try:decoder.decode(raw,session)
        except (ValueError,OverflowError):failures.append(name)
        else:raise AssertionError(name)
    for raw in (good[:-1],pending+b'\0'):
        try:decoder.decode(raw,session)
        except ValueError:pass
        else:raise AssertionError('extent')
    record={};frames=iter((pending,good));clock=[0.0]
    def now():return clock[0]
    def pause(_):clock[0]+=.001
    saves=[]
    result=tool.capture(lambda *_:next(frames),session,evidence,record,lambda:saves.append(dict(record)),1,now,pause)
    assert result['status']==1 and record['rtc_result']=='observed-success'
    assert (evidence/'rtc-snapshot.bin').read_bytes()==payload
    bad=bytearray(good);bad[32]^=1
    failed=evidence/'failed-hash';failed.mkdir();record={}
    try:tool.capture(lambda *_:bad,session,failed,record,lambda:None,1,now,pause)
    except ValueError:pass
    else:raise AssertionError('hash refusal')
    assert (failed/'rtc-001.bin').read_bytes()==bad and record['rtc_result']=='unobserved'
    late=evidence/'late';late.mkdir();record={};clock[0]=0
    def delayed_save():
        if 'copied_rtc_output' in record:clock[0]=2
    try:tool.capture(lambda *_:good,session,late,record,delayed_save,1,now,pause)
    except ValueError:pass
    else:raise AssertionError('late finalization')
    # The existing common board admission is an explicit modeled boundary.
    # RTC authority target binding and exact preserved evidence bytes run here.
    prepare=tool.preparation
    schemas={'initial':['exclusive_rtc_and_rccu_word','irq228_owned_disabled_inactive_unpending','remote_writers_excluded','other_cores_excluded','inherited_alarm_disabled_unpending'],
             'clock':['external_32768_stable','calendar_initialized','source_writers_excluded'],
             'electrical':['fitted_board_identified','vcc_rtc_power_stable'],
             'supervision':['whole_board_power_removal','permanent_ownership','no_reuse','one_attempt']}
    original=prepare.board.validate_admission;prepare.board.validate_admission=lambda record,image,template,cache,frozen:cache
    try:
        for variant in ('nominal','remote-writer','clock','electrical','reuse'):
            raw=bytearray(session);cache={};record={'schema':'a733-rtc-supervised-entry-v1','identity':'modeled-target','port':'modeled-port'}
            for key,offset in zip(schemas,(632,664,696,728)):
                fact={'schema':'a733-rtc-'+key+'-v1','identity':record['identity'],'port':record['port'],'image_sha256':hashlib.sha256((out/'build/sunstone-a733-rtc.elf').read_bytes()).hexdigest(),'reviewed':True,**{flag:True for flag in schemas[key]}}
                flag={'remote-writer':'remote_writers_excluded','clock':'external_32768_stable','electrical':'vcc_rtc_power_stable','reuse':'no_reuse'}.get(variant)
                if flag in fact:fact[flag]=False
                data=json.dumps(fact,sort_keys=True).encode();path=(evidence/(variant+'-'+key+'.json')).resolve();path.write_bytes(data)
                digest=hashlib.sha256(data).digest();cache[str(path)]=data;raw[offset:offset+32]=digest;record[key]={'path':str(path),'sha256':digest.hex()}
            try:prepare.validate_admission(record,fact['image_sha256'],hashlib.sha256(raw).hexdigest(),raw,cache,True)
            except ValueError:assert variant!='nominal'
            else:assert variant=='nominal'
    finally:prepare.board.validate_admission=original
    (out/'host-result.json').write_text(json.dumps({'passed':True,'mutations':failures,'scope':'Synthetic USB framing with actual native-copied snapshot; not physical USB or RTC evidence.'},indent=2)+'\n')
    print('PASS RTC host framing/capture boundaries')
if __name__=='__main__':main()
