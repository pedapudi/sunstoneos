#!/usr/bin/env python3
"""Observe one native USB exchange within the existing returning kernel runner."""
import argparse
import errno
import importlib.util
import os
import json
import re
from pathlib import Path
import struct
import sys
import time
import usb.core
import usb.util

ROOT=Path(__file__).resolve().parents[1]
# Strong references prevent implicit PyUSB interface release during retained failure.
_retained_devices=[]

def observe(access,config,previous_identity,deadline,record,save,expected_generation):
    observation={'attempted':False,'complete':False}
    record['native_usb']=observation;save()
    start=record['observation_start_monotonic']
    end=min(deadline,start+1.65)
    admit_until=min(end-0.2,start+1.05)
    while time.monotonic()<end:
        try:
            candidate=access.session_node(config)
            eligible=candidate is not None and access.identity(candidate)!=previous_identity
            if eligible:
                port=access.target(candidate)
                product=(port/'product').read_text().strip()
                revision=(port/'bcdDevice').read_text().strip()
                if revision=='02b3':break
                eligible=(product=='sunstoneos returning USB' and revision=='0100' and
                    os.access(candidate,os.R_OK|os.W_OK) and time.monotonic()<admit_until)
                if eligible:
                    identity=access.identity(candidate);access.require_session(candidate)
                    if access.identity(candidate)!=identity:raise RuntimeError('Native identity changed before exchange.')
        except OSError as error:
            if error.errno not in (errno.ENOENT,errno.ENODEV):raise
            eligible=False
        except ValueError as error:
            if str(error)!='Device disappeared from sysfs.':raise
            eligible=False
        if eligible:
            bus,address=int(candidate.parent.name),int(candidate.name)
            device=usb.core.find(idVendor=0x1f3a,idProduct=0xefe8,bus=bus,address=address)
            if device is None:raise RuntimeError('Selected native USB device disappeared.')
            _retained_devices.append(device)
            observation.update(attempted=True,identity=identity);save()
            try:
                # One write and one read. A timeout is a transport failure;
                # the kernel runner retains its lock through recovery.
                remaining=end-time.monotonic()
                if remaining<=0:raise RuntimeError('Native observation deadline elapsed.')
                timeout=max(1,min(100,int(remaining*1000)))
                if time.monotonic()>=admit_until:
                    observation.update(attempted=False,negative='Native request admission elapsed during setup; no bulk request sent.')
                    save();return
                if device.write(0x02,b'USB?',timeout=timeout)!=4:raise RuntimeError('Incomplete native OUT transfer.')
                remaining=end-time.monotonic()
                if remaining<=0:raise RuntimeError('Native observation deadline elapsed.')
                timeout=max(1,min(100,int(remaining*1000)))
                raw=bytes(device.read(0x81,40,timeout=timeout))
                if time.monotonic()>=end:raise RuntimeError('Native observation deadline elapsed.')
                if len(raw)!=40:raise RuntimeError('Incomplete native IN response.')
                words=struct.unpack('<5Q',raw)
                if words[0]!=0x3154455253425355 or words[1]!=expected_generation or words[2]!=1:raise RuntimeError('Invalid native USB response.')
                observation.update(complete=True,response_hex=raw.hex(),generation=words[1],usb_step=words[3],pmic_stage=words[4]);save()
            except BaseException:
                record['transport_failed']=True;save();raise
            return
        time.sleep(0.01)
    observation['negative']='No native exchange admitted within bounded observation; no protocol request sent.';save()

def functional_result(record,log,generation):
    observed=record.get('native_usb',{})
    values={name:int(value,16) for name,value in re.findall(r'^(USB_[A-Z_]+)=0x([0-9a-f]{16})$',log,re.M)}
    success=(record.get('complete') is True and observed.get('complete') is True and
        observed.get('generation')==generation and
        values=={'USB_OBSERVATION_OUTCOME':0,'USB_RECEIVED':1,'USB_SENT':40,'USB_TRANSFERRED':40,'USB_QUIESCED':1})
    observed['functional_success']=success
    return success

def main(argv=None):
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument('--ram-input',type=Path,required=True)
    parser.add_argument('--evidence',type=Path,required=True)
    args,_=parser.parse_known_args(argv)
    raw=args.ram_input.read_bytes()
    if len(raw)!=96 or struct.unpack_from('<3Q',raw)!=(0x31504e4953425355,1,96):
        raise ValueError('The USB diagnostic input differs from its producer format.')
    generation=struct.unpack_from('<Q',raw,24)[0]
    spec=importlib.util.spec_from_file_location('kernel_return',ROOT/'tools/check-a733-kernel.py')
    kernel=importlib.util.module_from_spec(spec);spec.loader.exec_module(kernel)
    kernel.main(argv,observe=lambda *values:observe(*values,expected_generation=generation))
    session=args.evidence/'session.json';record=json.loads(session.read_text())
    # Kernel completion establishes a different, responsive ROM instance. The
    # old native handles now refer to detached instances; cleanup cannot send
    # SET_INTERFACE into the native session.
    for device in _retained_devices:usb.util.dispose_resources(device)
    _retained_devices.clear()
    success=functional_result(record,(args.evidence/'kernel.log').read_text(),generation)
    session.write_text(json.dumps(record,indent=2)+'\n')
    if not success:raise SystemExit('USB observation incomplete; inspect the returned operation outcome.')
    print('PASS native USB complete OUT/IN observation, quiescence and responsive ROM return.')
if __name__=='__main__':main(sys.argv[1:])
