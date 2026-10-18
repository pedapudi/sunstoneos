#!/usr/bin/env python3
"""Only new crypto copied-result and authenticated-admission boundaries; no hardware."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def run(session_path,frame_path,out):
    out.mkdir(parents=True,exist_ok=False);host=module('crypto_host_checks',ROOT/'tools/check-a733-crypto-supervised.py');session=session_path.read_bytes();frame=frame_path.read_bytes();reference=host.REFERENCE
    pending=struct.pack('<4I2Q',0x54505943,1,0,0,*struct.unpack_from('<2Q',session,24))+session[760:824]+bytes(32)
    calls=[];clock=[1.];record={}
    def exchange(packet,timeout):assert packet==bytes(1) and 0<timeout<=500;calls.append(1);return pending if len(calls)==1 else frame
    host.capture(exchange,session,out,record,lambda:None,60,now=lambda:clock[0],pause=lambda seconds:clock.__setitem__(0,clock[0]+seconds))
    assert record['crypto_result']=='observed-success' and (out/'output.bin').read_bytes()==reference
    for offset in (0,4,8,12,16,24,32,64,96,128,170,287):
        bad=bytearray(frame);bad[offset]^=1
        if offset>=128:bad[96:128]=hashlib.sha256(bad[128:]).digest()
        try:host.decoder.decode(bad,session)
        except ValueError:pass
        else:raise AssertionError('Corruption accepted at'+str(offset))
    for size in (0,127,287,289):
        try:host.decoder.decode((frame+bytes(1))[:size],session)
        except ValueError:pass
        else:raise AssertionError('Wrong length accepted')
    for offset in (592,600,608,632,664,696,728,760,792):
        bad=bytearray(session);bad[offset:offset+32 if offset>=632 else offset+8]=bytes(32 if offset>=632 else 8)
        try:host.preparation.session_check(bad)
        except ValueError:pass
        else:raise AssertionError('Session refusal missing')
    fail_dir=out/'durability-fault';fail_dir.mkdir();failure={}
    with patch.object(host.supervisor,'durable_frame',side_effect=OSError('modeled durability failure')):
        try:host.capture(lambda *a:frame,session,fail_dir,failure,lambda:None,60,now=lambda:1.)
        except OSError:pass
        else:raise AssertionError('Durability failure accepted')
    assert failure['crypto_result']=='unobserved' and 'durable_result' not in failure
    # Complete admission logic is tested with frozen, explicitly modeled facts.
    base_record=dict(schema='a733-crypto-supervised-entry-v1',identity='MODEL-NOT-A-BOARD',port='MODEL',image_sha256='ab'*32)
    cache={};raw=bytearray(session)
    facts={
      'initial':['all_secure_nonsecure_ce_clients_excluded','remote_firmware_excluded','prior_ce_dma_independently_resolved','secure_bank_read_authorized','irq200_201_both_worlds_owned_masked','boot_lineage_reviewed'],
      'memory':['private_cacheable_page','no_physical_aliases','ce_physical_ram_reachable','exclusive_cache_lines','permanent_page_reservation'],
      'clocks':['observed_ce400_peri0','stable_ref_peri0_fabric','no_clock_reset_writers','ce_local_gates_resets_ready'],
      'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse','one_attempt']}
    for (key,fields),offset in zip(facts.items(),(632,664,696,728)):
        data=json.dumps(dict(schema='a733-crypto-'+key+'-v1',identity='MODEL-NOT-A-BOARD',port='MODEL',image_sha256='ab'*32,reviewed=True,**{k:True for k in fields})).encode();path=str((out/(key+'-MODEL.json')).resolve());cache[path]=data;raw[offset:offset+32]=hashlib.sha256(data).digest();base_record[key]=dict(path=path,sha256=hashlib.sha256(data).hexdigest())
    with patch.object(host.preparation.board,'validate_admission',return_value=cache):
        host.preparation.validate_admission(base_record,'ab'*32,'cd'*32,raw,cache,True)
        for key,fields in facts.items():
            original=cache[base_record[key]['path']]
            for field in fields:
                fact=json.loads(original);fact[field]=False;data=json.dumps(fact).encode();cache[base_record[key]['path']]=data
                changed=json.loads(json.dumps(base_record));changed[key]['sha256']=hashlib.sha256(data).hexdigest();bad=bytearray(raw);offset=dict(initial=632,memory=664,clocks=696,supervision=728)[key];bad[offset:offset+32]=hashlib.sha256(data).digest()
                try:host.preparation.validate_admission(changed,'ab'*32,'cd'*32,bad,cache,True)
                except ValueError:pass
                else:raise AssertionError('Missing physical fact accepted:'+field)
                cache[base_record[key]['path']]=original
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,scope='Actual copied native frame pending-to-durable success; header/session/vector/payload/hash/length refusals; durability failure; every new physical admission fact refused independently. Shared lifecycle reused, common base validation mocked only for narrow extension cases.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('session','frame','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.session,a.frame,a.output)
