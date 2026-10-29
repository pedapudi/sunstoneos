#!/usr/bin/env python3
"""Create one authenticated bounded native hash, HMAC or AES operation offline."""
import argparse
import hashlib
from pathlib import Path
import secrets
import struct
MAGIC=0x4153373343525131
HEADER=48
MAXIMUM=2048
OPERATIONS={'sha256':1,'aes-encrypt':2,'aes-decrypt':3,'hash':4,'hmac':5,'cipher-encrypt':6,'cipher-decrypt':7}
HASHES={'sha1':1,'sha224':2,'sha256':3,'sha384':4,'sha512':5}
MODES={'ecb':0,'cbc':1,'ctr':2,'gcm':3}
EXTENT=2824
def runtime(session):
    return len(session)==824 and struct.unpack_from('<Q',session,600)[0] in (2,3)
def decode(raw,session=None):
    if not HEADER<=len(raw)<=EXTENT:raise ValueError('Invalid crypto request extent')
    magic,version,operation,identity,count,reserved=struct.unpack_from('<QIIQII',raw)
    if magic!=MAGIC or not identity or reserved or count>MAXIMUM:raise ValueError('Invalid crypto request header')
    result=dict(id=identity,operation=operation,input_bytes=count,request_version=version)
    if version==1:
        if operation not in (1,2,3) or count!=len(raw)-HEADER:raise ValueError('Invalid crypto request header')
        if (operation==1 and any(raw[32:48])) or (operation!=1 and (not count or count%16)):
            raise ValueError('Invalid crypto key or input extent')
        result['output_bytes']=32 if operation==1 else count
    elif version==2:
        if len(raw)!=EXTENT or any(raw[32:48]) or any(raw[48+count:2096]):raise ValueError('Invalid extended crypto extent/padding')
        method,mode,key_bytes,iv_bytes,aad_bytes,tag_bytes=struct.unpack_from('<6I',raw,2096)
        if key_bytes>128 or iv_bytes>64 or aad_bytes>512 or any(raw[2120+key_bytes:2248]) or any(raw[2248+iv_bytes:2312]) or any(raw[2312+aad_bytes:2824]):
            raise ValueError('Invalid crypto parameter extent/padding')
        if operation in (4,5):
            if method not in HASHES.values() or mode or iv_bytes or aad_bytes or tag_bytes or (operation==4 and key_bytes):
                raise ValueError('Invalid hash parameters')
            output={1:20,2:28,3:32,4:48,5:64}[method]
        elif operation in (6,7):
            if method or mode not in MODES.values() or key_bytes not in (16,24,32):raise ValueError('Invalid cipher parameters')
            if mode==3:
                if not iv_bytes or not 12<=tag_bytes<=16 or (count>2032 if operation==6 else count<tag_bytes or count-tag_bytes>2032):
                    raise ValueError('Invalid GCM parameters')
                output=count+tag_bytes if operation==6 else count-tag_bytes
            else:
                if aad_bytes or tag_bytes or iv_bytes!=(16 if mode else 0) or not count or (mode!=2 and count%16):
                    raise ValueError('Invalid cipher extent/IV')
                output=count
        else:raise ValueError('Invalid extended crypto operation')
        result.update(output_bytes=output,method=method,mode=mode,key_bytes=key_bytes,iv_bytes=iv_bytes,aad_bytes=aad_bytes,tag_bytes=tag_bytes)
    else:raise ValueError('Invalid crypto request version')
    if session is not None and (not runtime(session) or session[760:792]!=hashlib.sha256(raw).digest()):
        raise ValueError('Crypto request differs from retained session')
    return result

def encode(operation,identity,message,key=bytes(16)):
    if len(key)!=16:raise ValueError('AES key must contain16bytes')
    raw=struct.pack('<QIIQII',MAGIC,1,operation,identity,len(message),0)+key+message
    decode(raw);return raw

def encode_extended(operation,identity,message,*,method='sha256',mode='ecb',key=b'',iv=b'',aad=b'',tag_bytes=None):
    """Fixed wire v2; supplied IV/counter uniqueness remains the caller's job."""
    if isinstance(operation,str):operation=OPERATIONS[operation]
    if operation in (4,5) and mode!='ecb':raise ValueError('Cipher mode is not a hash parameter')
    if operation in (6,7) and method!='sha256':raise ValueError('Hash method is not a cipher parameter')
    method=HASHES[method] if operation in (4,5) else 0
    mode=MODES[mode] if operation in (6,7) else 0
    if tag_bytes is None:tag_bytes=16 if mode==3 else 0
    if len(message)>2048 or len(key)>128 or len(iv)>64 or len(aad)>512:raise ValueError('Extended request too large')
    raw=struct.pack('<QIIQII',MAGIC,2,operation,identity,len(message),0)+bytes(16)+message.ljust(2048,b'\0')
    raw+=struct.pack('<6I',method,mode,len(key),len(iv),len(aad),tag_bytes)+key.ljust(128,b'\0')+iv.ljust(64,b'\0')+aad.ljust(512,b'\0')
    decode(raw);return raw
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--operation',choices=OPERATIONS,required=True)
    p.add_argument('--input',type=Path,required=True);p.add_argument('--key',type=Path)
    p.add_argument('--hash',choices=HASHES,default='sha256');p.add_argument('--mode',choices=MODES,default='ecb')
    p.add_argument('--iv',type=Path);p.add_argument('--aad',type=Path);p.add_argument('--tag-bytes',type=int)
    p.add_argument('--request-id',type=int);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--template',type=Path);p.add_argument('--session-output',type=Path)
    p.add_argument('--provision-ce-clock',action='store_true',help='Session version3: admitted reset-held/gated CE initialization')
    p.add_argument('--oracle',type=Path,help='Optional expected output for qualification only')
    a=p.parse_args();identity=a.request_id or secrets.randbits(64) or 1
    if OPERATIONS[a.operation]<=3:
        if a.iv or a.aad or a.tag_bytes is not None or a.mode!='ecb' or a.hash!='sha256':p.error('Extended parameters require hash/hmac/cipher operation')
        raw=encode(OPERATIONS[a.operation],identity,a.input.read_bytes(),a.key.read_bytes() if a.key else bytes(16))
    else:
        raw=encode_extended(a.operation,identity,a.input.read_bytes(),method=a.hash,mode=a.mode,
            key=a.key.read_bytes() if a.key else b'',iv=a.iv.read_bytes() if a.iv else b'',
            aad=a.aad.read_bytes() if a.aad else b'',tag_bytes=a.tag_bytes)
    a.output.write_bytes(raw)
    if a.template or a.session_output:
        if not (a.template and a.session_output):p.error('--template and --session-output are required together')
        session=bytearray(a.template.read_bytes())
        if len(session)!=824:p.error('Session template must contain824bytes')
        struct.pack_into('<Q',session,600,3 if a.provision_ce_clock else 2);session[760:792]=hashlib.sha256(raw).digest()
        session[792:824]=hashlib.sha256(a.oracle.read_bytes()).digest() if a.oracle else bytes(32)
        a.session_output.write_bytes(session)
    print(hashlib.sha256(raw).hexdigest())
