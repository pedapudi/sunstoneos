#!/usr/bin/env python3
"""Validate a copied RTC result without opening a device."""
import calendar
import datetime
import hashlib
import struct

def decode(raw,session):
    if len(session) not in (768,784) or len(raw)<64:raise ValueError('Truncated RTC input')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x31435452,struct.unpack_from('<Q',session,600)[0]) or status not in (0,1,2):raise ValueError('Unsupported RTC frame')
    if not epoch or not generation or (epoch,generation)!=struct.unpack_from('<2Q',session,24):raise ValueError('RTC identity differs')
    result={'status':status,'bytes':count,'epoch':epoch,'generation':generation}
    if status!=1:
        if count or len(raw)!=64 or any(raw[32:64]):raise ValueError('Private RTC output exposed')
        return result
    if version==4:
        if count!=2416 or len(raw)!=2480 or hashlib.sha256(raw[64:]).digest()!=raw[32:64]:raise ValueError('RTC service extent/digest differs')
        records,complete,retained,omitted=struct.unpack_from('<4I',raw,64)
        if records>12 or complete not in (0,1) or retained not in (0,1) or (complete and retained):raise ValueError('RTC service terminal state invalid')
        if any(raw[80+records*200:]):raise ValueError('RTC service private unused records exposed')
        seed_day,seed_hms=struct.unpack_from('<2I',session,772)
        seen=set();operations=[]
        for i in range(records):
            record=raw[80+i*200:80+(i+1)*200]
            ident,client,kind,outcome,reserved=struct.unpack_from('<Q4I',record)
            if not ident or ident in seen or client>1 or kind not in (1,2,3) or outcome not in range(1,7) or reserved or any(record[196:]):raise ValueError('RTC service record identity/state invalid')
            seen.add(ident)
            day,hour,minute,second,year,month,date=struct.unpack_from('<7I',record,24)
            if outcome in (1,3) or (kind==3 and outcome==2):
                t=datetime.datetime(year,month,date,hour,minute,second)
                if not 1<=day<=65535 or (t.date()-datetime.date(1970,1,1)).days+1!=day:raise ValueError('RTC service calendar differs')
            if kind==2 and outcome in (1,3):
                report=struct.unpack_from('<14I',record,52)
                target=(seed_day+client)*86400+(seed_hms>>16)*3600+((seed_hms>>8)&63)*60+(seed_hms&63)
                applied=day*86400+hour*3600+minute*60+second
                if report[9]!=6 or report[10]!=2 or report[12]!=(1 if outcome==1 else 3) or not target<=applied<=target+(report[11]+999999)//1000000:raise ValueError('RTC service applied calendar differs')
            if kind==3 and outcome in (1,2):
                alarm=struct.unpack_from('<22I',record,108)
                if (alarm[20],alarm[21])!=((1,0) if outcome==1 else (0,1)):raise ValueError('RTC service alarm completion differs')
                if tuple(alarm[7:14])!=(day,hour,minute,second,year,month,date):raise ValueError('RTC service alarm terminal sample differs')
            operations.append({'id':ident,'client':client,'kind':kind,'outcome':outcome,'day':day})
        if complete:
            expected={0:[(2,1),(1,1),(3,1),(1,1)],1:[(1,1),(2,4),(2,1),(1,1),(3,2),(1,1)]}
            if omitted or records!=10 or any([(x['kind'],x['outcome']) for x in operations if x['client']==c]!=sequence for c,sequence in expected.items()):raise ValueError('RTC native client composition differs')
        elif not retained:raise ValueError('RTC service terminal transcript incomplete')
        result.update(service_complete=complete,service_retained=retained,operations=operations,snapshot_sha256=raw[32:64].hex())
        return result
    if version==3:
        if count!=104 or len(raw)!=168 or hashlib.sha256(raw[64:]).digest()!=raw[32:64]:raise ValueError('Runtime RTC digest/extent differs')
        report=struct.unpack('<14I',raw[64:120]);boot=struct.unpack('<12I',raw[120:])
        before_control,before_status,before_day,before_hms,after_control,after_status,after_day,after_hms,requested,stage,writes,elapsed,outcome,cancel=report
        flags,day,hms,reserved=struct.unpack_from('<4I',session,768)
        if flags not in (4,5) or requested!=2 or outcome not in (1,2,3,4) or cancel not in (0,1):raise ValueError('Runtime RTC outcome differs')
        if boot[8]!=flags or boot[9]!=6 or (boot[4]&0x13)!=0x11 or boot[5]&7!=1:raise ValueError('Runtime source boot incomplete')
        if outcome in (1,3):
            if stage!=6 or writes!=2 or cancel!=int(outcome==3):raise ValueError('Runtime applied state differs')
            final_day=after_day&65535;hh,mm,ss=(after_hms>>16)&31,(after_hms>>8)&63,after_hms&63
            if not final_day or hh>23 or mm>59 or ss>59 or (after_control&0x193)!=0x11 or after_status&7!=1 or (before_control^after_control)&0xc01f:raise ValueError('Runtime calendar/source invalid')
            actual=(final_day-1)*86400+hh*3600+mm*60+ss
            target=(day-1)*86400+(hms>>16)*3600+((hms>>8)&63)*60+(hms&63)
            if not target<=actual<=target+(elapsed+999999)//1000000:raise ValueError('Runtime absolute calendar differs')
        elif outcome==2:
            if writes or not cancel or stage>=6:raise ValueError('Cancellation may have written calendar')
        elif stage>=6:raise ValueError('Retained failure claims complete setting')
        result.update(runtime_outcome=outcome,cancel_seen=cancel,calendar_writes=writes,
                      runtime_stage=stage,snapshot_sha256=raw[32:64].hex())
        return result
    expected=88 if len(session)==768 else 136
    if count!=expected or len(raw)!=64+expected or hashlib.sha256(raw[64:]).digest()!=raw[32:64]:raise ValueError('RTC copy digest/extent differs')
    values=struct.unpack('<22I',raw[64:152])
    times=[]
    for offset in (0,7):
        day,hour,minute,second,year,month,date=values[offset:offset+7]
        if not 1<=day<=65535:raise ValueError('Invalid RTC day')
        dt=datetime.datetime(year,month,date,hour,minute,second,tzinfo=datetime.timezone.utc)
        if (dt.date()-datetime.date(1970,1,1)).days+1!=day:raise ValueError('RTC calendar conversion differs')
        times.append(calendar.timegm(dt.utctimetuple()))
    target_day,target_hms,saved_day,saved_hms,control,clock_status,seen,cancelled=values[14:]
    delay=struct.unpack_from('<I',session,760)[0]
    h,m,s=(target_hms>>16)&31,(target_hms>>8)&63,target_hms&63
    if not 1<=target_day<=65535 or target_hms&~0x1f3f3f or h>23 or m>59 or s>59:raise ValueError('Invalid alarm target')
    target=(target_day-1)*86400+h*3600+m*60+s
    if (control&0x193)!=0x11 or clock_status&7!=1 or (seen,cancelled) not in ((1,0),(0,1)):raise ValueError('Invalid RTC completion')
    if times[1]<times[0] or target<times[0]+delay or (seen and times[1]<target):raise ValueError('RTC time ordering differs')
    if len(session)==784:
        before_control,before_status,before_day,before_hms,after_control,after_status,after_day,after_hms,flags,stage,writes,elapsed=struct.unpack('<12I',raw[152:])
        requested,day,hms,reserved=struct.unpack_from('<4I',session,768)
        if flags!=requested or stage!=6 or not writes or elapsed>=6000000 or (before_control^after_control)&12:
            raise ValueError('RTC initialization result differs')
        if (after_control&0x193)!=0x11 or after_status&7!=1:raise ValueError('RTC source readiness differs')
        if flags&2:
            final_day=after_day&65535;final_hms=after_hms&0x1f3f3f
            final_h,final_m,final_s=final_hms>>16,(final_hms>>8)&63,final_hms&63
            if not final_day or final_h>23 or final_m>59 or final_s>59:raise ValueError('Initialized calendar invalid')
            requested_absolute=(day-1)*86400+(hms>>16)*3600+((hms>>8)&63)*60+(hms&63)
            final_absolute=(final_day-1)*86400+final_h*3600+final_m*60+final_s
            if not requested_absolute<=final_absolute<=requested_absolute+(elapsed+999999)//1000000:
                raise ValueError('Initialized absolute calendar differs')
        result.update(initialization_flags=flags,initialization_writes=writes,initialization_elapsed_us=elapsed,
                      inherited_control=before_control,inherited_status=before_status,inherited_day=before_day,inherited_hms=before_hms)
    result.update(initial=times[0],terminal=times[1],target=target,alarm_seen=seen,cancelled=cancelled,
                  snapshot_sha256=raw[32:64].hex())
    return result
