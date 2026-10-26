#!/usr/bin/env python3
"""Exact linked session admission before borrow; platform outcomes are modeled."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from harness import Image, refused
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X3, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--legacy-image',type=Path)
    p.add_argument('--capabilities-only',action='store_true',help='Only old/current host image capability admission')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    if a.capabilities_only:
        assert a.legacy_image is not None
        path=Path(__file__).resolve().parents[1]/'apps/os/prepare.py'
        spec=importlib.util.spec_from_file_location('rate_preparation',path);prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)
        def check(image):return prep.image_manifest_check(json.loads((image.parent/'os-image/link-check.json').read_text()),image.read_bytes())
        assert check(a.image) is False
        refused(lambda:check(a.legacy_image),'Linked image lacks the firmware fast-mode capability')
        result={'passed':True,'physical_access':False,'cases':['current-fast-image','legacy-slow-image-refused'],
                'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
                'legacy_sha256':hashlib.sha256(a.legacy_image.read_bytes()).hexdigest()}
        (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('Passed two linked image capability cases');return
    image=Image(a.image,{'a733_radio_session','a733_radio_session_config','a733_radio_platform','a733_radio_platform_ops','sunxi_sdio'})
    s,c,r,o,h=(image.layouts[n] for n in ('a733_radio_session','a733_radio_session_config','a733_radio_platform','a733_radio_platform_ops','sunxi_sdio'))
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x41000000,0x1200000);cpu.mem_map(0x80000000,0x100000)
    for address,data in image.parts:cpu.mem_write(address,data)
    subject,platform,returned,current=0x80010000,0x80080000,0x80000000,0x80000004
    names=('xTaskGetCurrentTaskHandle','a733_radio_platform_poll','a733_radio_platform_borrow','a733_radio_platform_retain','sunxi_sdio_stop')
    callbacks={image.address(n):n for n in names};calls=[];platform_result=2;borrow_ok=1
    def put(address,value,bytes_=8):cpu.mem_write(address,int(value).to_bytes(bytes_,'little'))
    def get(address):return int.from_bytes(cpu.mem_read(address,4),'little')
    def hook(cpu,address,size,unused):
        if address==current:value=1
        elif address in callbacks:
            name=callbacks[address];calls.append(name)
            if name=='xTaskGetCurrentTaskHandle':value=1
            elif name=='a733_radio_platform_poll':value=platform_result
            elif name=='a733_radio_platform_borrow':
                put(cpu.reg_read(UC_ARM64_REG_X3),platform+r['card']);value=borrow_ok
            else:value=3
        else:return
        cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    cpu.hook_add(UC_HOOK_CODE,hook);results=[]
    cases=[('fast-ready',1,1,25000000,2,1,0,1),('slow-fallback',1,0,400000,2,1,4,0),
           ('width-missing',1,0,25000000,2,1,4,0),('rate-slow',1,1,400000,2,1,4,0),
           ('rate-unsupported',1,1,50000000,2,1,4,0),('unrequested-slow',0,0,400000,2,1,0,1),
           ('platform-busy',1,0,400000,1,1,1,0),('platform-retained',1,0,400000,3,1,4,0),
           ('borrow-refused',1,1,25000000,2,0,4,1)]
    for name,requested,wide,hz,platform_result,borrow_ok,expected,borrows in cases:
        cpu.mem_write(0x80000000,bytes(0x100000));calls.clear()
        put(subject+s['self'],subject);put(subject+s['platform'],platform)
        put(subject+s['config']+c['owner_task'],1);put(platform+r['ops']+o['current'],current)
        put(platform+r['four_bit'],requested,4);put(platform+r['host']+h['wide'],wide,1)
        put(platform+r['host']+h['card_hz'],hz,4)
        for reg,value in ((UC_ARM64_REG_X0,subject),(UC_ARM64_REG_SP,0x800ff000),(UC_ARM64_REG_LR,returned)):
            cpu.reg_write(reg,value)
        cpu.emu_start(image.address('a733_radio_session_poll','drivers/net/a733_radio_session.c'),returned,count=10000)
        assert cpu.reg_read(UC_ARM64_REG_PC)==returned,name
        assert cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff==expected,(name,calls)
        assert calls.count('a733_radio_platform_borrow')==borrows,(name,calls)
        failed=expected==4
        assert get(subject+s['fault'])==failed and calls.count('sunxi_sdio_stop')==failed
        assert calls.count('a733_radio_platform_retain')==failed
        assert get(subject+s['stage'])==int(expected==0 and borrows==1)
        results.append({'name':name,'result':expected,'calls':list(calls),'fault':failed})
    (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,
        'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'layouts':image.layouts,'cases':results,
        'scope':'Actual linked PLATFORM-to-ADOPT decision. Owner/platform callbacks modeled; no transport setup or hardware execution.'},indent=2)+'\n')
    print('Passed',len(results),'linked session rate-admission cases')

if __name__=='__main__':main()
