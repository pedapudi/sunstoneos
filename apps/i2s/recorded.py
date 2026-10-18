#!/usr/bin/env python3
"""Embed a fixed excerpt of the existing recorded PCM development fixture."""
import hashlib
from pathlib import Path
import sys
import wave
source,target=map(Path,sys.argv[1:])
if hashlib.sha256(source.read_bytes()).hexdigest()!='ef445d0243b19b5a38d7d916911eecf1593570ceb2ab7cf4c2bab7bb5be400b4':
    raise SystemExit('recorded PCM source identity differs')
with wave.open(str(source)) as wav:
    if (wav.getnchannels(),wav.getsampwidth(),wav.getframerate(),wav.getcomptype())!=(1,2,16000,'NONE'):
        raise SystemExit('recorded PCM format differs')
    pcm=wav.readframes(8192)
if len(pcm)!=16384:raise SystemExit('recording is too short')
digest=hashlib.sha256(pcm).digest()
text='#include <stdint.h>\nconst uint8_t i2s_recorded_pcm[16384]={'+','.join(map(str,pcm))+'};\n'
text+='const uint8_t i2s_recorded_pcm_digest[32]={'+','.join(map(str,digest))+'};\n'
if not target.exists() or target.read_text()!=text:target.write_text(text)
