#!/usr/bin/env python3
"""Decode indexed copied AnnexB outputs; numerical encoding qualification is separate."""
import hashlib,struct
HEADER=160
MAXIMUM=1048576

def decode(raw,session,ordinal):
 if len(raw)<HEADER or len(session)!=856 or ordinal not in (0,1):raise ValueError('H264 encode extent differs.')
 magic,version,status,n,epoch,generation=struct.unpack_from('<4I2Q',raw)
 if (magic,version)!=(0x45433248,1) or status not in (0,1,2):raise ValueError('H264 encode wire header differs.')
 if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:64]!=session[768:800]:raise ValueError('H264 encode result identity differs.')
 if struct.unpack_from('<11I',raw,96)!=(ordinal,2,0 if ordinal else 1,640,480,640,66,31,26,30,1) or any(raw[140:160]):raise ValueError('H264 encode metadata differs.')
 if status!=1:
  if n or len(raw)!=HEADER or any(raw[64:96]):raise ValueError('Non-success output includes unqualified bytes.')
  return dict(status=status,ordinal=ordinal,bytes=0,numerical_qualified=False)
 if not 5<n<=MAXIMUM or len(raw)!=HEADER+n:raise ValueError('H264 encode copied length differs.')
 data=raw[HEADER:]
 if data[:4]!=b'\0\0\0\1' or hashlib.sha256(data).digest()!=raw[64:96]:raise ValueError('Copied AnnexB content/digest differs.')
 if (data[4]&31)!=(1 if ordinal else 7):raise ValueError('Copied picture prefix differs.')
 return dict(status=1,ordinal=ordinal,bytes=n,sha256=hashlib.sha256(data).hexdigest(),width=640,height=480,profile=66,qp=26,numerical_qualified=False)

def query(ordinal):
 if ordinal not in (0,1):raise ValueError('Invalid output ordinal.')
 return bytes((0,ordinal))

def ack(raw,session,ordinal):
 if decode(raw,session,ordinal)['status']!=1:raise ValueError('Cannot ACK absent output.')
 return b'\1'+struct.pack('<I',ordinal)+session[32:40]+raw[64:96]
