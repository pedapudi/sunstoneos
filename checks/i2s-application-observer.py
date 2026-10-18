#!/usr/bin/env python3
"""Validate the exact observer identity before any modeled configuration."""
import contextlib
import io
from pathlib import Path
import runpy
import struct
import sys
import tempfile
import types
from unittest.mock import patch
root=Path(__file__).resolve().parents[1]
for mode in range(3):
    calls=[]
    def find(**identity):
        assert identity==dict(idVendor=0x1f3a,idProduct=0xefe8,bus=3,address=65)
        return device
    class Device:
        product='wrong application' if mode==1 else 'sunstoneos native I2S'
        serial_number='wrong serial' if mode==2 else 'sunstoneos-a733-i2s'
        def set_configuration(self):calls.append('configure')
        def write(self,endpoint,data,timeout):
            assert endpoint==2 and data==struct.pack('<III',2,0,0);calls.append('cancel');return len(data)
        def read(self,endpoint,count,timeout):
            assert endpoint==0x81
            return struct.pack('<6I2Q',0x53493241,1,3,0,0,16384,7,8)+bytes(32)
    device=Device();core=types.ModuleType('usb.core');core.find=find
    usb=types.ModuleType('usb');usb.core=core
    with tempfile.TemporaryDirectory() as tmp,patch.dict(sys.modules,{'usb':usb,'usb.core':core}),patch.object(sys,'argv',[
        'observe.py','--bus','3','--address','65','--epoch','7','--generation','8',
        '--session-sha256','00'*32,'--output',str(Path(tmp)/'pcm'),'--cancel']),contextlib.redirect_stdout(io.StringIO()):
        try:runpy.run_path(str(root/'apps/i2s/observe.py'),run_name='__main__')
        except SystemExit as error:
            assert mode and str(error)=='selected device is not the native I2S application'
        else:assert mode==0
    assert calls==(['configure','cancel'] if not mode else [])
print('PASS observer exact I2S identity and pre-configuration refusal')
