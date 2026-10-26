#!/usr/bin/env python3
"""Actual linked thermal producer, window hooks, admission and USB report pages.

Reuses the NPU check's bounded native machine for MMIO/task/USB boundaries.
THS readings and task identity are modeled; the policy, immutable rows, report
copy, USB0 exchange and host collector execute their actual implementations.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from harness import Image, hook_addresses, refused
from unicorn.arm64_const import UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec);spec.loader.exec_module(value)
    return value


native = module('thermal_native_machine', ROOT / 'checks/os-npu.py')
tool = module('thermal_os_runner', ROOT / 'tools/check-a733-os-supervised.py')
report = tool.thermal_report
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--session', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--default-image', type=Path)
p.add_argument('--entry-only', action='store_true', help='only host launch profile routing')
p.add_argument('--capacity-only', action='store_true', help='only the maximum final-page boundary')
a = p.parse_args();a.output.mkdir(parents=True, exist_ok=False)
elf = a.build / 'sunstone-a733-os.elf'
image = Image(elf, layout_names=(*native.NAMES, 'os_thermal_service', 'os_thermal_readings',
    'os_thermal_window', 'os_thermal_report', 'os_thermal_reading', 'a733_thermal_sample', 'a733_thermal_policy'))
session = bytearray(a.session.read_bytes())
struct.pack_into('<I', session, 804, 70000)
epoch, generation = struct.unpack_from('<2Q', session, 24)


class Machine(native.Machine):
    def __init__(self):
        super().__init__(image, session, npu=False)
        self.current=1;self.sequence=0;self.hot=False;self.rx=b''
        self.thermal=image.address('thermal',unit='apps/os/thermal.c')
        self.put(self.thermal+self.L['os_thermal_service']['handle'],1)
        self.put(self.thermal+self.L['os_thermal_service']['cpu_critical_millicelsius'],70000,4)
        additions=('a733_thermal_poll','a733_thermal_read','gicv3_spi_mask','gicv3_spi_rearm',
                   'os_cpu_clock_step','mimus_board_generation')
        for name in additions:self.hooks[image.address(name)]=name
        hook_addresses(self.u,{image.address(name) for name in additions},self.hook)

    def hook(self, cpu, at, size, unused):
        name=self.hooks.get(at)
        r=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(3)]
        if name=='a733_thermal_poll':self.ret(2)
        elif name=='a733_thermal_read':
            self.sequence+=1
            layout=self.L['a733_thermal_sample'];address=r[2]
            self.u.mem_write(address,bytes(48))
            self.put(address+layout['observed_us'],self.now)
            self.put(address+layout['sequence'],self.sequence,4)
            self.put(address+layout['valid'],31,1)
            self.u.mem_write(address+layout['millicelsius'],struct.pack('<5i',50000,50000,50000,70000 if self.hot else 50000,50000))
            self.ret(4)
        elif name in ('gicv3_spi_mask','gicv3_spi_rearm','os_cpu_clock_step','mimus_board_generation'):self.ret(1)
        elif name=='vTaskDelay':self.suspended=True;cpu.emu_stop()
        else:super().hook(cpu,at,size,unused)

    def exchange_frame(self, packet):
        stopped, frame=self.exchange(packet)
        assert not stopped and frame
        return frame

    def write(self, endpoint, packet, timeout):
        assert endpoint==2 and timeout>0
        self.rx=self.exchange_frame(bytes(packet));return len(packet)

    def read(self, endpoint, size, timeout):
        assert endpoint==0x81 and timeout>0
        part,self.rx=self.rx[:min(size,31)],self.rx[min(size,31):]
        return part

    def complete_window(self, rows=64):
        self.call('os_thermal_task',0,budget=100000)
        self.call('os_thermal_record_begin')
        for _ in range(rows-1):
            self.now+=100000;self.call('os_thermal_task',0,budget=100000)
        self.now+=1;self.call('os_thermal_record_end',1)


def capacity_case():
    # Exercise the maximum final page independently of sampling duration. Seed
    # the already validated row layout to the allocation boundary, then run the
    # actual native copy and USB route. This is a bounds fixture, not sensor data.
    maximum=Machine();maximum.complete_window()
    row_base=image.address('readings',unit='apps/os/thermal.c')+image.layouts['os_thermal_readings']['rows']
    row_data=bytes((i*7)&255 for i in range(report.MAX_ROWS*report.ROW_BYTES))
    maximum.u.mem_write(row_base,row_data)
    window_base=image.address('window',unit='apps/os/thermal.c')+image.layouts['os_thermal_window']['header']
    maximum.put(window_base+8,report.MAX_BYTES,4);maximum.put(window_base+12,report.MAX_ROWS,4)
    frame=maximum.exchange_frame(struct.pack('<BBHIQ',7,0,1024,report.MAX_BYTES-1024,1))
    assert struct.unpack_from('<III',frame,8)==(1,1024,report.MAX_BYTES) and frame[64:]==row_data[-1024:]


def entry_case():
    # Exercise the launch entry's routing without invoking readiness or transport.
    # The collector itself was exercised against the native USB exchange above.
    class View:
        def __enter__(self):return a.output, {}
        def __exit__(self, *unused):return False
    calls=[]
    legacy={'cpu_clock':True}
    zero=bytearray(session);struct.pack_into('<I',zero,804,0)
    tool.preparation.manifest_cpu_clock(legacy,zero)
    refused(lambda:tool.preparation.manifest_cpu_clock(legacy,session),'does not support a session CPU critical trip')
    current=json.loads((a.build/'os-image/link-check.json').read_text())
    assert current['thermal_report'] is True and current['cpu_critical_trip'] is True
    tool.preparation.manifest_cpu_clock(current,session)
    plain=bytearray(session[:808]);struct.pack_into('<Q',plain,608,808)
    tool.preparation.manifest_cpu_clock({'cpu_clock':False,'cpu_critical_trip':True,'thermal_report':False},plain)
    for manifest in ({'cpu_clock':False,'cpu_critical_trip':True,'thermal_report':True},
                     {'cpu_clock':False,'cpu_critical_trip':1},
                     {'cpu_clock':False,'cpu_critical_trip':True,'thermal_report':1}):
        refused(lambda m=manifest:tool.preparation.manifest_cpu_clock(m,plain),'Invalid thermal image capabilities')
    saved=(tool.freeze_bundle,tool.combined.launch_view,tool.supervisor.run_session,
           tool.combined.capture_device,tool.read_thermal_report)
    def run_session(args,view,launch,ready,entry,capture,**unused):
        record={};capture(None,lambda:None,59,record,lambda:None);return record
    def captured(*unused):calls.append('recognition')
    def thermal_capture(*unused):calls.append('thermal')
    try:
        tool.combined.launch_view=lambda value:View()
        tool.supervisor.run_session=run_session
        tool.combined.capture_device=captured
        tool.read_thermal_report=thermal_capture
        argv=['--bundle',str(a.output),'--fel','unused','--evidence',str(a.output),
              '--database','unused','--claim','unused','--owner','model','--supervised-manual-reconnect']
        for enabled, capable in ((False,False),(True,False),(True,True)):
            manifest={'cpu_clock':enabled}
            if capable:manifest['thermal_report']=True
            selected={'_combined_frozen':{'board/image-input-manifest.json':json.dumps(manifest).encode()}}
            tool.freeze_bundle=lambda path,count:selected
            calls.clear();tool.main(argv)
            assert calls==(['recognition','thermal'] if capable else ['recognition'])
        def capture_failed(*unused):
            calls.append('recognition');raise ValueError('modeled recognition capture failure')
        tool.combined.capture_device=capture_failed;calls.clear()
        refused(lambda:tool.main(argv),'modeled recognition capture failure')
        assert calls==['recognition']
    finally:
        (tool.freeze_bundle,tool.combined.launch_view,tool.supervisor.run_session,
         tool.combined.capture_device,tool.read_thermal_report)=saved


if a.entry_only:
    entry_case()
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,
        cases=['host-entry-selects-report-only-after-clock-profile-capture'],physical_access=False),indent=2)+'\n')
    print('PASS host entry selects thermal capture only for completed CPU-clock capture')
    raise SystemExit(0)

if a.capacity_only:
    capacity_case()
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,
        cases=['full-capacity-final-page-copy-within-allocation'],
        image_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),physical_access=False),indent=2)+'\n')
    print('PASS native thermal report full-capacity final page')
    raise SystemExit(0)

cases=[]
m=Machine()
assert m.call('os_session_valid',m.session)==1
for value in (0,110000,110001,0xffffffff):
    m.put(m.session+804,value,4)
    assert m.call('os_session_valid',m.session)==int(value<=110000)
m.put(m.session+804,70000,4);m.put(m.session+802,1,1)
assert m.call('os_session_valid',m.session)==0
cases.append('linked-native-session-70C-and-vendor-bound')
m=Machine()
refused(lambda:report.capture(m.exchange_frame,epoch,generation),'pending')
cases.append('pending-window-no-unbounded-wait')
m.complete_window()
raw=report.capture(m.exchange_frame,epoch,generation)
decoded=report.decode(raw,epoch,generation)
assert decoded['rows']==64 and decoded['recognition_generation']==1 and report.classify(decoded)=='complete'
assert decoded['cpu_critical_millicelsius']==70000
(a.output/'native-thermal-report.bin').write_bytes(raw)
cases.append('actual-native-window-paged-through-USB-and-decoded')
# Later rows and later recognitions cannot change the published range/bytes.
m.now+=100000;m.call('os_thermal_task',0,budget=100000)
m.call('os_thermal_record_begin');m.call('os_thermal_record_end',0)
assert raw==report.capture(m.exchange_frame,epoch,generation)
cases.append('first-window-immutable-after-new-samples-and-recognition')
for offset,count,expected in ((len(raw),1,1),(len(raw)-1,2,1),(0,72,2),(0xffffffff,1,1)):
    frame=m.exchange_frame(struct.pack('<BBHIQ',7,0,count,offset,expected))
    assert struct.unpack_from('<II',frame,8)==(2,0)
cases.append('page-extent-and-recognition-generation-refusals')
capacity_case()
cases.append('full-capacity-final-page-copy-within-allocation')
for count,flags in ((0,0),(1025,0),(72,1)):
    bad=Machine();stopped,frame=bad.exchange(struct.pack('<BBHIQ',7,flags,count,0,0))
    assert stopped and not frame
cases.append('malformed-operation-7-stops-without-response')
# The terminal policy row precedes board_fail; no diagnostic can promise USB
# after the failure. Neither the window nor the underlying rows are reused.
fault=Machine();fault.call('os_thermal_task',0,budget=100000);fault.call('os_thermal_record_begin')
fault.now+=25000;fault.hot=True;fault.call('os_thermal_task',0,budget=100000)
base=image.address('readings',unit='apps/os/thermal.c');layout=image.layouts['os_thermal_readings']
count=fault.get(base+layout['count'],4);assert count==2 and fault.failed
row=fault.u.mem_read(base+layout['rows']+56*(count-1),56)
assert row[54]==2 and struct.unpack_from('<i',row,20)[0]==70000
stopped,frame=fault.exchange(struct.pack('<BBHIQ',7,0,72,0,0));assert stopped and not frame
cases.append('terminal-row-retained-before-failure-with-no-USB-delivery')
# Host classification refuses incomplete thermal evidence without changing
# the separately retained recognition outcome.
for offset,value,outcome in ((56,1,'overflow'),(68,0,'recognition-failed')):
    changed=bytearray(raw);struct.pack_into('<I',changed,offset,value)
    assert report.classify(report.decode(changed,epoch,generation))==outcome
changed=bytearray(raw);changed[72+52]=0
assert report.classify(report.decode(changed,epoch,generation))=='incomplete'
cases.append('overflow-and-incomplete-evidence-not-complete')
prepared={'_combined_frozen':{'board/session.bin':bytes(session),
    'board/image-input-manifest.json':json.dumps({'cpu_clock':True,'thermal_report':True}).encode()}}
assert tool.thermal_profile(prepared)
assert not tool.thermal_profile({'_combined_frozen':{'board/image-input-manifest.json':b'{"cpu_clock": false}'}})
record={'mimus_board_result':'observed-success'}
collector=a.output/'collector';collector.mkdir()
tool.read_thermal_report(m,lambda:None,59,prepared,collector,record,lambda:None,now=lambda:1)
assert record['thermal_report']['state']=='complete' and record['mimus_board_result']=='observed-success'
assert (collector/'thermal-report.bin').read_bytes()==raw
record_short={'mimus_board_result':'observed-success'}
tool.read_thermal_report(m,lambda:None,1.05,prepared,collector,record_short,lambda:None,now=lambda:1)
assert record_short['thermal_report']['state']=='not-requested'
cases.append('host-durable-separate-artifact-profile-and-deadline')
entry_case()
cases.append('host-entry-selects-report-only-after-clock-profile-capture')
if a.default_image:
    default=Image(a.default_image,layout_names=native.NAMES)
    baseline=native.Machine(default,session[:808],npu=False)
    assert 'os_thermal_report_copy' not in default.symbols
    stopped,frame=baseline.exchange(struct.pack('<BBHIQ',7,0,72,0,0));assert stopped and not frame
    cases.append('non-CPU-clock-profile-refuses-operation-7')
(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,
    image_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),physical_access=False),indent=2)+'\n')
print('PASS native thermal report, admission and host capture:',len(cases),'cases')
