#!/usr/bin/env python3
"""Check cached ledger allocation of USB HID parser state before host publication."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from elftools.elf.elffile import ELFFile
from harness import CACHE, CLANG, environment

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['drivers/usb/a733_xhci_tinyusb.c','drivers/usb/host/hid.c','drivers/usb/host/tusb_config.h',
              'drivers/usb/host/superspeed.py','apps/os/owner-memory.h','include/sunstone_usb_hid.h']


def symbol_bytes(path,name):
    with path.open('rb') as stream:
        symbols=ELFFile(stream).get_section_by_name('.symtab')
        matches=[s['st_size'] for s in symbols.iter_symbols() if s.name==name]
    assert len(matches)==1,(name,matches)
    return matches[0]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    out=parser.parse_args().output.resolve();out.mkdir(parents=True,exist_ok=False)

    def run(label,command):
        r=subprocess.run([str(v) for v in command],capture_output=True,text=True,env=environment())
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==0,label+': '+r.stderr
        return r.stdout

    tiny=CACHE/'sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src'
    run('tinyusb',[sys.executable,ROOT/'drivers/usb/host/superspeed.py','--source',tiny,'--output',out/'tinyusb'])
    flags=['-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-ffunction-sections',
           '-fdata-sections','-DSUNSTONE_USB_HOST_GENERIC=1','-I'+str(out/'tinyusb'),
           '-I'+str(ROOT/'drivers/usb/host'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'apps/os')]
    records=[]
    for count in (24,127):
        prefix=str(count)
        local=[*flags,'-DSUNSTONE_USB_HOST_DEVICES='+prefix+'u']
        legacy=out/(prefix+'-legacy.o')
        run(prefix+'-legacy',[CLANG,*local,'-DA733_USB_MEMORY_END=0x43000000','-c',
                             ROOT/'drivers/usb/a733_xhci_tinyusb.c','-o',legacy])
        local+=['-DSUNSTONE_USB_LEDGER_MEMORY=1']
        objects=[]
        for name,path in (('adapter',ROOT/'drivers/usb/a733_xhci_tinyusb.c'),
                          ('hid',ROOT/'drivers/usb/host/hid.c'),('fixture',ROOT/'checks/usb-hid-ledger.c')):
            obj=out/(prefix+'-'+name+'.o')
            run(prefix+'-'+name,[CLANG,*local,'-c',path,'-o',obj]);objects.append(obj)
        assert symbol_bytes(objects[0],'generic_hid')==8,'HID parser state remains static'
        old_bytes=symbol_bytes(legacy,'generic_hid')
        assert old_bytes>8 and old_bytes%(count*8)==0,(count,old_bytes)
        exe=out/(prefix+'-hid-ledger')
        run(prefix+'-link',[CLANG,*objects,'-Wl,--gc-sections','-o',exe])
        cases=[]
        for case in ('normal','allocation-failure','stack-failure','nongeneric','wrong-owner','clock-failure'):
            record=json.loads(run(prefix+'-'+case,[exe,case]));cases.append(record)
            if case=='normal':assert record['hid_bytes']==old_bytes
        records.append(dict(devices=count,legacy_state_bytes=old_bytes,ledger_pointer_bytes=8,cases=cases,
                            executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest()))
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,profiles=records,
        scope='Actual host adapter binding and HID parser; allocator, owner clock and TinyUSB initialization are modeled. No controller or scheduler execution.'),indent=2)+'\n')
    print('PASS cached USB HID state: 24/127 sizing, no static array, zeroed callback state, allocation and initialization refusals')


if __name__=='__main__':main()
