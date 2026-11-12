#!/usr/bin/env python3
"""Check manifest-sized USB NC leases and pointer aliases using the native constructor."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['cmake/sunstone-image.cmake','include/a733_usb_memory.h','include/a733_memory_uncached.h',
              'apps/os/owner-memory.h']
sys.path.insert(0,str(ROOT/'checks'))
import harness


def build(out,count):
    out.mkdir(parents=True,exist_ok=True)
    (out/'FreeRTOS.h').write_text('#include <stdint.h>\n#include <stddef.h>\n')
    (out/'task.h').write_text('#define taskSCHEDULER_NOT_STARTED 0\nint xTaskGetSchedulerState(void);\n')
    (out/'link.ld').write_text('ENTRY(a733_memory_uncached_init)\nSECTIONS { . = 0x41000000; .text : { *(.text*) } '
        '.rodata : { *(.rodata*) } . = 0x41200000; .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n')
    flags=[str(harness.CLANG),'--target=aarch64-none-elf','-march=armv8-a','-std=c11','-O2','-g','-ffreestanding',
        '-fno-builtin','-fno-stack-protector','-mstrict-align','-mgeneral-regs-only','-Wall','-Wextra','-Werror',
        '-DDEVICE_MEMORY_POOL_SINGLE_THREAD=1','-DSUNSTONE_USB_LEDGER_MEMORY=1','-DSUNSTONE_USB_HOST_GENERIC=1',
        '-DSUNSTONE_USB_HOST_DEVICES='+str(count)+'u','-I'+str(out),'-I'+str(ROOT/'include'),
        '-I'+str(ROOT/'apps/os'),'-I'+str(ROOT/'runtime/include')]
    objects=[]
    for name,path in (('usb',ROOT/'apps/os/usb-memory.c'),('pool',ROOT/'runtime/device_memory_pool.c'),
                      ('fixture',ROOT/'checks/usb-ledger-memory.c')):
        obj=out/(name+'.o')
        r=subprocess.run(flags+['-c',str(path),'-o',str(obj)],capture_output=True,text=True)
        (out/(name+'.log')).write_text(r.stdout+r.stderr);r.check_returncode();objects.append(str(obj))
    elf=out/'usb-ledger-memory.elf'
    r=subprocess.run([str(harness.LLD),'-T',str(out/'link.ld'),*objects,'-o',str(elf)],capture_output=True,text=True)
    (out/'link.log').write_text(r.stdout+r.stderr);r.check_returncode()
    return harness.Image(elf,layout_names=())


class Machine:
    def __init__(self,image):
        self.image=image;self.uc=harness.machine();self.uc.mem_map(0x41000000,0x800000)
        for a,data in image.parts:self.uc.mem_write(a,data)
        self.el=4;self.fail_pool=False;self.trace=[]
        self.decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
        self.uc.hook_add(UC_HOOK_CODE,self.code)

    def code(self,u,address,size,cookie):
        if address==0x417ff000:u.emu_stop();return
        if address==self.image.address('device_memory_pool_init_typed'):
            self.trace.append('pool')
            if self.fail_pool:
                u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR));return
        if address==self.image.address('os_owner_memory_below'):self.trace.append('owner')
        if address==self.image.address('os_usb_memory_alias'):self.trace.append('alias')
        op=next(self.decoder.disasm(bytes(u.mem_read(address,4)),address))
        if op.mnemonic=='mrs':
            dst,reg=op.op_str.lower().split(', ');assert reg=='currentel',reg
            u.reg_write(UC_ARM64_REG_X0+int(dst[1:]),self.el);u.reg_write(UC_ARM64_REG_PC,address+4)

    def call(self,name,*args):
        self.uc.reg_write(UC_ARM64_REG_SP,0x41700000);self.uc.reg_write(UC_ARM64_REG_LR,0x417ff000)
        for i,v in enumerate(args):self.uc.reg_write(UC_ARM64_REG_X0+i,v)
        self.uc.emu_start(self.image.address(name),0,count=100000)
        assert self.uc.reg_read(UC_ARM64_REG_PC)==0x417ff000,name+' did not return'
        return self.uc.reg_read(UC_ARM64_REG_X0)

    def put(self,name,value,size=4):self.uc.mem_write(self.image.address(name),value.to_bytes(size,'little'))
    def words(self):return struct.unpack('<10Q',self.uc.mem_read(self.image.address('fixture_trace'),80))


def native_cases(image,count):
    parent=0x440000+max(0,count-5)*0xa000
    total=(parent+0x300000+0x1fffff)&~0x1fffff
    base=0x50000000;alias=0x7a040000;payload=base+0x40000
    cases=[]
    m=Machine(image)
    assert m.call('a733_memory_uncached_prepare')==1 and m.words()==(0,)*10
    assert m.call('fixture_to_app',payload,1)==0 and m.call('fixture_from_app',alias,1)==0
    p=m.call('a733_memory_uncached_init');assert p
    assert m.trace==['owner','alias','pool'],m.trace
    assert m.words()[:9]==(total,0x200000,1,1<<32,1,1,base,parent,0),m.words()
    assert struct.unpack('<3Q',m.uc.mem_read(p,24))==(base,base,total)
    assert m.call('a733_memory_uncached_init')==p and m.call('a733_usb_memory_parent_base')==base
    assert m.trace==['owner','alias','pool']
    assert m.call('fixture_pool_valid',0)==1
    for variant in range(1,8):assert m.call('fixture_pool_valid',variant)==0,variant
    assert m.call('fixture_pool_valid',0)==1
    assert m.call('fixture_take_parent')==base and m.call('fixture_take_jpeg')==base+parent
    assert int.from_bytes(m.uc.mem_read(image.address('fixture_clean'),4),'little')==0
    cases+=['allocate-bind-publish-once','native-pool-identity-and-backing','parent-and-retained-leases']
    assert m.call('fixture_to_app',payload,0x400000)==alias
    assert m.call('fixture_from_app',alias,0x400000)==payload
    assert m.call('fixture_to_app',payload+0x3fffff,1)==alias+0x3fffff
    for a,z in ((base,1),(payload-1,1),(payload,0),(payload+0x3fffff,2),(payload,1<<63)):
        assert m.call('fixture_to_app',a,z)==0,(hex(a),z)
    for a,z in ((alias-1,1),(alias,0),(alias+0x400000,1),(alias+0x3fffff,2),(alias,(1<<64)-1)):
        assert m.call('fixture_from_app',a,z)==0,(hex(a),z)
    cases.append('payload-only-alias-and-bounds')
    for mode,expected in ((1,['owner']),(2,['owner','alias'])):
        m=Machine(image);m.put('fixture_mode',mode)
        assert not m.call('a733_memory_uncached_init') and m.trace==expected
        assert not m.call('a733_usb_memory_parent_base')
        assert not m.call('a733_memory_uncached_init') and m.trace==expected
        assert not m.call('fixture_to_app',payload,1)
        cases.append('allocation-refused' if mode==1 else 'alias-refused-retains-memory')
    for invalid in (base+4096,(1<<32)-0x200000,(1<<64)-0x200000):
        m=Machine(image);m.put('fixture_base',invalid,8)
        assert not m.call('a733_memory_uncached_init') and m.trace==['owner']
    cases.append('unaligned-and-unreachable-refused')
    m=Machine(image);m.fail_pool=True
    assert not m.call('a733_memory_uncached_init') and m.trace==['owner','alias','pool']
    assert not m.call('a733_usb_memory_parent_base')
    assert not m.call('a733_memory_uncached_init') and m.trace==['owner','alias','pool']
    cases.append('typed-pool-refused-retains-memory')
    m=Machine(image);m.put('fixture_scheduler',1)
    assert not m.call('a733_memory_uncached_init') and not m.trace
    m.put('fixture_scheduler',0);assert m.call('a733_memory_uncached_init')
    cases.append('runtime-construction-refused')
    m=Machine(image);m.el=0
    assert not m.call('a733_memory_uncached_init') and not m.trace
    m.el=4;assert m.call('a733_memory_uncached_init')
    cases.append('unprivileged-construction-refused')
    m=Machine(image);m.put('ss_usb_devices',count%127+1)
    assert not m.call('a733_memory_uncached_init') and not m.trace
    cases.append('manifest-object-count-mismatch-refused')
    m=Machine(image);m.put('ss_usb_devices',0)
    assert m.call('a733_memory_uncached_init') and m.words()[0]==total
    cases.append('no-host-control-uses-shared-count')
    return dict(devices=count,parent_bytes=parent,backing_bytes=total,cases=cases)


def manifest_cases(out):
    spec=importlib.util.spec_from_file_location('usb_ledger_manifest',ROOT/'tools/sunstone-manifest.py')
    tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
    original=json.loads((ROOT/'apps/hello-compute/manifest.json').read_text())
    original['devices']['required']=['usb-storage']
    path=out/'usb-manifest.json';path.write_text(json.dumps(original))
    m=tool.assign(tool.load(path));assert m['usb_devices']==24
    baseline=tool.pack(m);assert 'const unsigned ss_usb_devices = 24u;' in tool.c_tables(m)
    accepted=[]
    for n in (1,24,127):
        raw=json.loads(json.dumps(original));raw['usb']={'devices':n};path.write_text(json.dumps(raw))
        m=tool.assign(tool.load(path));assert m['usb_devices']==n and tool.pack(m)==baseline
        assert 'const unsigned ss_usb_devices = %du;'%n in tool.c_tables(m);accepted.append(n)
    for value in (0,128,-1,True,1.5,'wrong'):
        raw=json.loads(json.dumps(original));raw['usb']={'devices':value};path.write_text(json.dumps(raw))
        harness.refused(lambda:tool.load(path),'usb devices',SystemExit)
    raw=json.loads(json.dumps(original));raw['usb']={'devices':24,'unknown':1};path.write_text(json.dumps(raw))
    harness.refused(lambda:tool.load(path),'unknown usb keys',SystemExit)
    raw=json.loads(json.dumps(original));raw['devices']['required']=[];raw['usb']={'devices':24};path.write_text(json.dumps(raw))
    harness.refused(lambda:tool.load(path),'needs a USB host device',SystemExit)
    assert 'const unsigned ss_usb_devices = 0u;' in tool.c_tables(None)
    return dict(default=24,accepted=accepted,refusals=8,packed_manifest_unchanged=True)


def cmake_cases(out):
    spec=importlib.util.spec_from_file_location('usb_cmake_manifest',ROOT/'tools/sunstone-manifest.py')
    tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
    source=out/'cmake-source';source.mkdir()
    raw=json.loads((ROOT/'apps/hello-compute/manifest.json').read_text())
    directories={}
    for name,count in (('control',0),('host127',127),('host24',24)):
        value=json.loads(json.dumps(raw))
        if count:value['devices']['required']=['usb-storage'];value['usb']={'devices':count}
        directory=source/name;directory.mkdir();path=directory/'input.json';path.write_text(json.dumps(value))
        normalized=tool.public(tool.assign(tool.load(path)))
        (directory/'manifest.json').write_text(json.dumps([normalized]))
        directories[name]=directory
    none=source/'none';none.mkdir();(none/'manifest.json').write_text('[]\n')
    (source/'empty.c').write_text('void unused(void) {}\n')
    text='''cmake_minimum_required(VERSION 3.20)
project(usb_manifest_count C)
include("@ROOT@/cmake/sunstone-image.cmake")
add_library(sunstone-a733-os-core OBJECT empty.c)
add_library(a733-board-usb2-host OBJECT empty.c)
add_library(sunstone-os-settings INTERFACE)
set(SUNSTONE_A733_OS_USB_CLASSES ON)
sunstone_bind_usb_devices("@CONTROL@")
get_target_property(before sunstone-a733-os-core SUNSTONE_USB_DEVICE_COUNT)
if(NOT before STREQUAL "before-NOTFOUND")
  message(FATAL_ERROR "A control without a USB host locked the count")
endif()
sunstone_bind_usb_devices("@HOST127@")
sunstone_bind_usb_devices("@CONTROL@")
sunstone_bind_usb_devices("@NONE@")
foreach(target sunstone-a733-os-core a733-board-usb2-host)
  get_target_property(value ${target} COMPILE_DEFINITIONS)
  if(NOT value STREQUAL "SUNSTONE_USB_HOST_DEVICES=127u")
    message(FATAL_ERROR "${target} has the wrong device count: ${value}")
  endif()
endforeach()
get_target_property(value sunstone-os-settings INTERFACE_COMPILE_DEFINITIONS)
if(NOT value STREQUAL "SUNSTONE_USB_HOST_DEVICES=127u")
  message(FATAL_ERROR "The application settings have the wrong device count")
endif()
if(FIXTURE_CONFLICT)
  sunstone_bind_usb_devices("@HOST24@")
endif()
'''
    for key,path in {'ROOT':ROOT,'CONTROL':directories['control'],'HOST127':directories['host127'],
                     'HOST24':directories['host24'],'NONE':none}.items():text=text.replace('@'+key+'@',str(path))
    (source/'CMakeLists.txt').write_text(text)
    for name,conflict in (('cmake-valid',False),('cmake-conflict',True)):
        command=['cmake','-S',str(source),'-B',str(out/name),'-G','Unix Makefiles','-DCMAKE_MAKE_PROGRAM=/usr/bin/make',
                 '-DCMAKE_C_COMPILER='+str(harness.CLANG),'-DFIXTURE_CONFLICT='+('ON' if conflict else 'OFF')]
        r=subprocess.run(command,capture_output=True,text=True)
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        if conflict:assert r.returncode and 'different usb.devices counts' in r.stderr,r.stderr
        else:r.check_returncode()
    return ['control-before-host127','host127-before-control','no-application-does-not-lock','core-stack-settings-agree',
            'conflicting-host-count-refused']


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    manifest=manifest_cases(out)
    cmake=cmake_cases(out)
    native=[native_cases(build(out/str(n),n),n) for n in (24,127)]
    harness.save(out/'result.json',dict(passed=True,physical_access=False,manifest=manifest,cmake=cmake,native=native,
        scope='Actual AArch64 OS NC-pool constructor, native binding predicate and lease allocator; owner allocation and alias publication are modeled. No controller DMA executes.'))
    print('PASS USB ledger backing: manifest24/127, NC request, publication order, leases, aliases and refusal paths')


if __name__=='__main__':main()
