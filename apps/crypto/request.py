#!/usr/bin/env python3
"""Create one bounded caller-supplied SHA-256 or AES-128 ECB request offline."""
import argparse
import hashlib
from pathlib import Path
import secrets
import struct
MAGIC=0x4153373343525131
HEADER=48
MAXIMUM=2048
OPERATIONS={'sha256':1,'aes-encrypt':2,'aes-decrypt':3}
def runtime(session):
    return len(session)==824 and struct.unpack_from('<Q',session,600)[0] in (2,3)
def decode(raw,session=None):
    if not HEADER<=len(raw)<=HEADER+MAXIMUM:raise ValueError('Invalid crypto request extent')
    magic,version,operation,identity,count,reserved=struct.unpack_from('<QIIQII',raw)
    if magic!=MAGIC or version!=1 or operation not in (1,2,3) or not identity or reserved or count!=len(raw)-HEADER:
        raise ValueError('Invalid crypto request header')
    if (operation==1 and any(raw[32:48])) or (operation!=1 and (not count or count%16)):
        raise ValueError('Invalid crypto key or input extent')
    if session is not None and (not runtime(session) or
            session[760:792]!=hashlib.sha256(raw).digest()):raise ValueError('Crypto request differs from retained session')
    return dict(id=identity,operation=operation,input_bytes=count,output_bytes=32 if operation==1 else count)
def encode(operation,identity,message,key=bytes(16)):
    if len(key)!=16:raise ValueError('AES key must contain16bytes')
    raw=struct.pack('<QIIQII',MAGIC,1,operation,identity,len(message),0)+key+message
    decode(raw);return raw
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--operation',choices=OPERATIONS,required=True)
    p.add_argument('--input',type=Path,required=True);p.add_argument('--key',type=Path)
    p.add_argument('--request-id',type=int);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--template',type=Path);p.add_argument('--session-output',type=Path)
    p.add_argument('--provision-ce-clock',action='store_true',help='Session version3: admitted reset-held/gated CE initialization')
    p.add_argument('--oracle',type=Path,help='Optional expected output for qualification only')
    a=p.parse_args();raw=encode(OPERATIONS[a.operation],a.request_id or secrets.randbits(64) or 1,
        a.input.read_bytes(),a.key.read_bytes() if a.key else bytes(16));a.output.write_bytes(raw)
    if a.template or a.session_output:
        if not (a.template and a.session_output):p.error('--template and --session-output are required together')
        session=bytearray(a.template.read_bytes())
        if len(session)!=824:p.error('Session template must contain824bytes')
        struct.pack_into('<Q',session,600,3 if a.provision_ce_clock else 2);session[760:792]=hashlib.sha256(raw).digest()
        session[792:824]=hashlib.sha256(a.oracle.read_bytes()).digest() if a.oracle else bytes(32)
        a.session_output.write_bytes(session)
    print(hashlib.sha256(raw).hexdigest())
