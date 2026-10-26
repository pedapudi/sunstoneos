#!/usr/bin/env python3
"""Execute the actual OS GPU performance bridge with bounded service effects."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from harness import Image
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
image = Image(a.image, layout_names=['board_application', 'a733_board_session', 'pvr_gpu_service_reply'])
with a.image.open('rb') as stream:
    for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
        if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/os/gpu.c'):
            continue
        for die in cu.iter_DIEs():
            if die.tag != 'DW_TAG_variable' or die.attributes.get('DW_AT_name', None) is None or die.attributes['DW_AT_name'].value != b'gpu':
                continue
            members = list(die.get_DIE_from_attribute('DW_AT_type').iter_children())
            gpu_layout = {m.attributes['DW_AT_name'].value.decode(): m.attributes['DW_AT_data_member_location'].value
                          for m in members if m.tag == 'DW_TAG_member'}
            member = next(m for m in members if m.attributes.get('DW_AT_name') and m.attributes['DW_AT_name'].value == b'performance')
            perf_layout = {m.attributes['DW_AT_name'].value.decode(): m.attributes['DW_AT_data_member_location'].value
                           for m in member.get_DIE_from_attribute('DW_AT_type').iter_children() if m.tag == 'DW_TAG_member'}
results = []
for case in ('timely', 'queued-timeout', 'late-reply', 'wrong-task', 'revoked', 'unexpected-pending'):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0x41000000, 0x1200000);uc.mem_map(0x80000000, 0x10000)
    for address, data in image.parts:
        if data: uc.mem_write(address, data)
    def put(address, value, size=4):uc.mem_write(address, int(value).to_bytes(size, 'little'))
    def get(address, size=4):return int.from_bytes(uc.mem_read(address, size), 'little')
    def ret(value=0):
        uc.reg_write(UC_ARM64_REG_X0,value);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    gpu = image.address('gpu', '/apps/os/gpu.c')
    perf = gpu + gpu_layout['performance'];board = image.address('board')
    put(gpu+gpu_layout['started'],1)
    put(board+image.layouts['board_application']['live_until'],1000000,8)
    put(board+image.layouts['board_application']['session']+image.layouts['a733_board_session']['deadline_us'],1000000,8)
    state = dict(time=100, sent=0, receives=0, live=1, task=7, deadline=None)
    if case=='wrong-task':state['task']=9
    hooks = {image.address(n):n for n in ('xTaskGetCurrentTaskHandle','os_thermal_task_handle','board_live',
             'a733_smp_counter','pvr_gpu_service_performance_state','pvr_gpu_service_performance','pvr_gpu_service_collect')}
    def hook(machine,address,size,unused):
        name=hooks.get(address)
        if not name:return
        x=[machine.reg_read(UC_ARM64_REG_X0+i) for i in range(6)]
        if name=='xTaskGetCurrentTaskHandle':ret(state['task'])
        elif name=='os_thermal_task_handle':ret(7)
        elif name=='board_live':ret(state['live'])
        elif name=='a733_smp_counter':ret(state['time']*24)
        elif name=='pvr_gpu_service_performance_state':ret(3)
        elif name=='pvr_gpu_service_performance':
            assert not state['sent'] and x[3]==50100
            state['sent']+=1;state['deadline']=x[3];put(x[5],41,8);ret(1)
        else:
            assert x[1]==41 and x[3]==0
            state['receives']+=1
            if case in ('queued-timeout','revoked'):ret(0);return
            reply=x[2];layout=image.layouts['pvr_gpu_service_reply']
            put(reply+layout['result'],0)
            put(reply+layout['ticket'],41,8)
            put(reply+layout['performance_status'],0 if case=='unexpected-pending' else 1)
            put(reply+layout['performance_point'],600);put(reply+layout['performance_point']+4,800)
            if case=='late-reply':state['time']=50100
            ret(1)
    uc.hook_add(UC_HOOK_CODE,hook)
    def call(start, deadline=50100):
        uc.reg_write(UC_ARM64_REG_SP,0x8000f000);uc.reg_write(UC_ARM64_REG_LR,0x80000000)
        # READ is action0; its output is borrowed until successful completion.
        for i,v in enumerate((0,0,deadline,start,0x80001000)):uc.reg_write(UC_ARM64_REG_X0+i,v)
        uc.emu_start(image.address('os_gpu_perf_action'),0x80000000,count=100000)
        assert uc.reg_read(UC_ARM64_REG_PC)==0x80000000
        return uc.reg_read(UC_ARM64_REG_X0)
    put(0x80001000,0xfeedface)
    result=call(1)
    if case in ('queued-timeout','revoked'):
        assert result==0 and get(perf+perf_layout['ticket'],8)==41
        if case=='revoked':state['live']=0
        else:state['time']=50100
        result=call(0,900000)  # A polling caller cannot renew the captured bound.
    assert result==(1 if case=='timely' else 4), (case,result)
    if case=='timely':assert get(0x80001000)==600 and get(0x80001004)==800 and not get(perf+perf_layout['ticket'],8)
    else:
        assert get(0x80001000)==0xfeedface
        assert get(perf+perf_layout['ticket'],8)==(0 if case=='wrong-task' else 41)
    results.append(dict(case=case,**state))
a.output.mkdir(parents=True,exist_ok=False)
(a.output/'result.json').write_text(json.dumps({'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':results},indent=2)+'\n')
print('PASS actual GPU performance bridge:',len(results),'cases')
