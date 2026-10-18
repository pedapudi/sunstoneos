#!/usr/bin/env python3
"""Validate a copied RTC result without opening a device."""
import calendar
import datetime
import hashlib
import struct

def decode(raw,session):
    if len(session) not in (768,784) or len(raw)<64:raise ValueError('Truncated RTC input')
    magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
    if (magic,version)!=(0x31435452,1 if len(session)==768 else 2) or status not in (0,1,2):raise ValueError('Unsupported RTC frame')
    if not epoch or not generation or (epoch,generation)!=struct.unpack_from('<2Q',session,24):raise ValueError('RTC identity differs')
    result={'status':status,'bytes':count,'epoch':epoch,'generation':generation}
    if status!=1:
        if count or len(raw)!=64 or any(raw[32:64]):raise ValueError('Private RTC output exposed')
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
