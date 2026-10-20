#!/usr/bin/env python3
"""Durably retain fixed DisplayPort observations from one admitted session."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import time
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
preparation=module('displayport_host_prepare',ROOT/'apps/displayport/prepare.py')
supervisor=preparation.board.supervisor
require=supervisor.require
MAGIC=0x4153373344505231


def decode(raw,session,*,allocation):
    preparation.session_check(session)
    require(len(raw)==1024,'DisplayPort observation must contain exactly 1024 bytes.')
    magic,version,size,status,error,stage,retained=struct.unpack_from('<Q6I',raw)
    require((magic,version,size)==(MAGIC,1,1024) and status in (0,1,2) and retained in (0,1),
        'DisplayPort observation header differs.')
    epoch,generation,lease,pd_generation,hpd_irq=struct.unpack_from('<5Q',raw,32)
    require((epoch,generation,lease)==struct.unpack_from('<3Q',session,24) and
        raw[72:104]==hashlib.sha256(session).digest() and raw[104:136]==session[48:80],
        'DisplayPort build, lease or sealed session identity differs.')
    allocation_cpu,allocation_physical,allocation_bytes=struct.unpack_from('<3Q',raw,168)
    values=struct.unpack_from('<16I',raw,192)
    keys=('pixel_iova','pitch','width','height','de_state','iommu_state','scanned_masters','guards',
        'polarity','pin_assignment','hpd','padding_valid','core_bytes','reserved0','reserved1','reserved2')
    result=dict(zip(keys,values))
    require(result['core_bytes']==(0 if status==0 else 352) and not any(raw[244:256]) and not any(raw[608:]),
        'DisplayPort copied core or reserved bytes differ.')
    if status==0:
        require((error,stage,retained)==(0,0,1) and not any(raw[56:72]) and not any(raw[136:]),
            'DisplayPort pending response claims unpublished native diagnostics.')
    require(result['polarity'] in (0,1,2) and result['hpd'] in (0,1) and
        result['guards'] in (0,1) and result['padding_valid'] in (0,1),'Invalid DisplayPort observation flags.')
    core_words=struct.unpack_from('<16I',raw,256)
    core_keys=('state','error','stage','retained','aux_requests','aux_defers','cr_attempts','eq_attempts',
        'aux_status','aux_reply','aux_count','phy_status','frame_status','fifo_status','tv_status','edid_bytes')
    core=dict(zip(core_keys,core_words))
    require(core['state']<=5 and core['error']<=11 and core['retained'] in (0,1) and
        core['edid_bytes'] in (0,128,256) and not any(raw[346:352]),'Invalid copied DisplayPort provider result.')
    core.update(dpcd=raw[320:336].hex(),link_status=raw[336:342].hex(),training=raw[342:346].hex(),edid=raw[352:608].hex())
    result.update(status=status,error=error,stage=stage,retained=retained,epoch=epoch,generation=generation,lease=lease,
        pd_generation=pd_generation,hpd_irq=hpd_irq,session_sha256=raw[72:104].hex(),build_id=raw[104:136].hex(),
        pixel_sha256=raw[136:168].hex(),allocation_cpu=allocation_cpu,allocation_physical=allocation_physical,
        allocation_bytes=allocation_bytes,core=core,physical_qualified=False)
    if status==1:success_check(result,session,allocation)
    return result


def success_check(result,session,allocation):
    row=allocation['allocation']
    require((result['allocation_cpu'],result['allocation_physical'],result['allocation_bytes'])==
        (row['cpu'],row['physical'],row['bytes']) and result['pixel_sha256']==allocation['pixel_sha256'] and
        (result['pixel_iova'],result['pitch'],result['width'],result['height'])==(0x10001000,2560,1280,720),
        'DisplayPort copied allocation, pattern or output geometry differs from admission.')
    require((result['error'],result['stage'],result['retained'])==(0,6,1) and
        (result['de_state'],result['iommu_state'],result['scanned_masters'],result['guards'],result['padding_valid'])==
        (3,5,0x100,1,1) and result['pd_generation']==result['generation'] and
        result['polarity'] in (1,2) and result['pin_assignment'] in (4,16) and result['hpd']==1,
        'DisplayPort terminal shutdown, mapping or historical Type-C observation is incomplete.')
    core=result['core'];link=bytes.fromhex(core['link_status'])
    require((core['state'],core['error'],core['stage'],core['retained'])==(4,0,6,1) and
        core['edid_bytes'] in (128,256) and 1<=core['cr_attempts']<=10 and 1<=core['eq_attempts']<=5 and
        all((link[index//2]>>(4*(index%2)))&7==7 for index in range(4)) and link[2]&1 and
        not core['fifo_status']&3 and not core['frame_status']&3 and not core['tv_status']&0x60000000,
        'DisplayPort copied provider result does not establish the stopped trained-link observation.')


def capture_device(device,same,deadline,session,evidence,record,save,*,allocation,now=time.monotonic,pause=time.sleep):
    preparation.session_check(session)
    epoch,generation,lease=struct.unpack_from('<3Q',session,24)
    record.update(displayport_result='unobserved',physical_qualified=False,
        manual_whole_board_power_removal_required=True,
        displayport_identity=dict(epoch=epoch,generation=generation,lease=lease,build_id=session[48:80].hex(),
            session_sha256=hashlib.sha256(session).hexdigest()))
    save()
    def timeout():
        same();remaining=int((deadline-now())*1000)
        require(remaining>0,'Original DisplayPort capture deadline expired.')
        return min(1000,remaining)
    previous_hpd_irq=0
    try:
        for sequence in range(1,201):
            intent=dict(sequence=sequence,command='read-only-observation',write_intent=True,write_completed=False)
            record.setdefault('displayport_requests',[]).append(intent);save()
            count=device.write(0x02,b'\0',timeout=timeout());intent.update(write_completed=True,accepted_bytes=count)
            require(count==1,'Short DisplayPort observation request write.');save()
            # Requesting one byte beyond the fixed reply consumes its terminal
            # ZLP at both FS64 and HS512; no ACK or resource reuse follows.
            raw=bytes(device.read(0x81,1025,timeout=timeout()))
            path=Path(evidence)/f'displayport-{sequence:03}.bin'
            supervisor.durable_frame(path,raw)
            entry=dict(path=path.name,bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest())
            record.setdefault('displayport_frames',[]).append(entry);save()
            result=decode(raw,session,allocation=allocation);entry.update(status=result['status'],error=result['error'],stage=result['stage'])
            require(result['pd_generation'] in (0,generation) and result['hpd_irq']>=previous_hpd_irq,
                'DisplayPort Type-C generation or HPD IRQ sequence moved backward.')
            previous_hpd_irq=result['hpd_irq']
            require(now()<deadline,'DisplayPort reply arrived after the capture deadline.')
            if result['status']==0:
                pause(min(.1,max(0,deadline-now())));continue
            target=Path(evidence)/'copied-observation.json'
            supervisor.durable_frame(target,(json.dumps(result,indent=2)+'\n').encode())
            record['copied_observation']=dict(path=target.name,sha256=hashlib.sha256(target.read_bytes()).hexdigest())
            save();require(now()<deadline,'DisplayPort copied observation persisted after deadline.')
            record['displayport_result']='observed-success' if result['status']==1 else 'observed-failure'
            record['displayport_observation']=result
            try:save()
            except BaseException:
                record['displayport_result']='unobserved';raise
            return result
        raise TimeoutError('DisplayPort bounded observation budget exhausted.')
    except BaseException as error:
        record['displayport_result']='unobserved'
        record['displayport_capture_failure']=dict(type=type(error).__name__,message=str(error))
        save();raise
