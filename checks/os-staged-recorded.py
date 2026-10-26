#!/usr/bin/env python3
"""Execute actual AArch64 readiness, result-copy and USB0 protocol for OS workload2.

USB providers, RTOS and inference completion are modeled. The frame producer and
host capture are actual code. No engine, physical device or queue is accessed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import struct
from harness import Image, Steps, LLD
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--resume', action='store_true')
parser.add_argument('--startup-only', action='store_true', help='Only actual recorded startup/trigger and stop boundaries')
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=args.resume)
steps = Steps(out, 'commands.json')
db = json.loads((args.build / 'compile_commands.json').read_text())

def flags(suffix):
    unit = next(x for x in db if x['file'].endswith(suffix) and 'sunstone-a733-os.dir' in x['command'])
    parts = shlex.split(unit['command'])
    return [parts[0], '-I' + str(out), *parts[1:parts.index('-o')]]

cflags = flags('/os-image/application.c')
cppflags = flags('/os-image/recognition-placed.cc')
shutil.copyfile(args.build / 'os-image/application.c', out / 'application-native.c')
shutil.copyfile(ROOT / 'checks/os-staged-usb-base.h', out / 'mimus-usb-base.c')
(out / 'recognition-fixture.cc').write_text('#include "' + str(args.build.resolve() / 'os-image/recognition-placed.cc') + '"\n' + '''
extern "C" void fixture_result(unsigned completed)
{
    used=5;std::memcpy(transcript,"test\\n",5);state.store(1);generation.store(1);
    overlap.store(completed);acknowledged.store(0);stopped.store(false);
}
''')
objects = []
for name, source, selected in (
        ('application', ROOT / 'checks/os-staged-recorded.c', cflags),
        ('recognition', out / 'recognition-fixture.cc', cppflags),
        ('memory', ROOT / 'runtime/memory.c', cflags)):
    obj = out / (name + '.o'); objects.append(obj)
    steps.command('compile-' + name + '-' + hashlib.sha256(source.read_bytes()).hexdigest()[:12], [*selected, '-c', source, '-o', obj])
image_path = out / 'runtime.elf'
entries = ('fixture_init', 'fixture_start', 'fixture_query', 'fixture_case')
steps.command('link', [LLD, '-Ttext=0x41000000', '--entry=fixture_init', '--gc-sections',
                       *['--undefined=' + e for e in entries], *objects, '-o', image_path])
image = Image(image_path)
with image_path.open('rb') as stream:
    elf = ELFFile(stream)
    segments = [(int(s['p_vaddr']), int(s['p_memsz']), s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']

class Native:
    def __init__(self, workload, start=True):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM); pages = set()
        for address, size, data in segments:
            for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
                if page not in pages:self.uc.mem_map(page, 4096); pages.add(page)
            self.uc.mem_write(address, data)
        self.uc.mem_map(0x50000000, 0x10000)
        self.uc.hook_add(UC_HOOK_INTR, lambda machine, interrupt, unused: machine.emu_stop())
        self.call('fixture_init', workload)
        if start:self.call('fixture_start')
        self.pending = b''; self.ops = []; self.frames = []; self.mutate = None

    def word(self, name):return int.from_bytes(self.uc.mem_read(image.address(name), 4), 'little')
    def setword(self, name, value):self.uc.mem_write(image.address(name), struct.pack('<I', value))
    def call(self, name, value=0, expected=1):
        self.setword('fixture_status', 0)
        self.uc.reg_write(UC_ARM64_REG_SP, 0x5000f000); self.uc.reg_write(UC_ARM64_REG_X0, value)
        self.uc.emu_start(image.address(name), 0, count=200000)
        status = self.word('fixture_status')
        assert status == expected, (name, value, status, expected)
    def write(self, endpoint, packet, timeout):
        assert endpoint == 2 and timeout > 0 and len(packet) == 16
        self.ops.append(packet[0]);self.uc.mem_write(image.address('fixture_packet'), packet)
        self.call('fixture_query')
        frame = bytes(self.uc.mem_read(image.address('fixture_response'), self.word('fixture_response_bytes')))
        if self.mutate:frame = self.mutate(packet, frame)
        self.pending = frame;self.frames.append(frame)
        return len(packet)
    def read(self, endpoint, size, timeout):
        assert endpoint == 0x81 and timeout > 0
        part, self.pending = self.pending[:min(size, 31)], self.pending[min(size, 31):]
        return part

cases = {}
def checked(name, action):action();cases[name] = True

def native_case(workload, case, start=True):
    n = Native(workload, start);n.call('fixture_case', case)

if args.startup_only:
    for name, case, started in [('before-ready',0,False), ('ready-no-host',1,True),
                                ('automatic-trigger',11,True), ('failure-closes',2,True)]:
        native_case(2, case, started); cases[name] = True
    sources = ['checks/os-staged-recorded.py','checks/os-staged-recorded.c','checks/os-staged-usb-base.h',
               'apps/mimus-board/application.c','apps/mimus-board/recognition.cc','apps/mimus-board/keyboard.c',
               'apps/os/application.patch','apps/os/os.h']
    result = dict(passed=True, physical_access=False, cases=cases,
                  os_image_sha256=hashlib.sha256((args.build/'sunstone-a733-os.elf').read_bytes()).hexdigest(),
                  image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),
                  sources={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in sources},
                  scope='Actual AArch64 recorded readiness and automatic trigger; USB providers, scheduler and inference completion modeled.')
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS recorded startup:', len(cases)); raise SystemExit(0)

for label, workload, case, start in (
        ('staged-before-usb-start',2,0,False), ('staged-usb-ready-no-host-access',2,1,True),
        ('staged-fatal-after-ready',2,2,True), ('staged-expired',2,3,True),
        ('staged-wrong-storage-state',2,4,True), ('staged-copy-without-storage-overlap',2,5,True),
        ('storage-overlap-required',0,6,True), ('keyboard-overlap-required',4,6,True),
        ('storage-reattachment-refused',0,7,True), ('keyboard-reattachment-refused',4,7,True),
        ('storage-stale-attachment',0,8,True), ('keyboard-stale-attachment',4,8,True)):
    checked(label, lambda w=workload,c=case,s=start:native_case(w,c,s))
for label, mode in (('staged-failed-usb-start',0),('staged-lost-health-after-ready',1),('staged-sector-request-retains',2)):
    n = Native(2, start=mode!=0)
    if mode==0:n.setword('fixture_start_failed',1);n.call('fixture_start', expected=2)
    elif mode==1:n.call('fixture_case',9,expected=2)
    else:n.uc.mem_write(image.address('fixture_packet'),struct.pack('<BBHIQ',2,0,512,0,0));n.call('fixture_query',expected=2)
    n.call('fixture_case',10);cases[label]=True

spec = importlib.util.spec_from_file_location('staged_capture', ROOT / 'tools/check-a733-mimus-board-supervised.py')
tool = importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)

def refused(action):
    try:action()
    except (ValueError,OSError):return
    raise AssertionError('refusal required')

for workload in (0,2,4):
    native = Native(workload)
    raw = bytearray(808);struct.pack_into('<2Q',raw,24,0x1234,9);struct.pack_into('<I',raw,400,workload)
    prepared = {'workload':workload,'image_sha256':hashlib.sha256(image_path.read_bytes()).hexdigest(),
                '_combined_files':{'prepared.json':'model-no-bundle'},'_combined_frozen':{'board/session.bin':bytes(raw)},
                '_recognitions':1}
    directory = out / ('capture-' + str(workload));directory.mkdir();record = {}
    tool.generation_count(1,workload)
    tool.capture_device(native,lambda:None,59,prepared,directory,record,lambda:None,now=lambda:1,pause=lambda _:None)
    assert record['mimus_board_result']=='observed-success'
    outputs = record['recognitions'][0]['copied_outputs']
    assert set(outputs) == ({'transcript.txt'} if workload==2 else {'transcript.txt','sector.bin','trigger.bin'} if workload==4 else {'transcript.txt','sector.bin'})
    assert (directory / outputs['transcript.txt']['path']).read_bytes()==b'test\n'
    assert native.ops == ([0,1,0] if workload==2 else [0,1,2,3,0,4] if workload==4 else [0,1,2,0])
    if workload==2:assert native.word('fixture_typec_reads')==native.word('fixture_typec_revokes')==native.word('fixture_usb2_reads')==0
    (directory / 'record.json').write_text(json.dumps(record,indent=2)+'\n')
    cases['actual-native-capture-workload-' + str(workload)] = True

for label, offset, value in (('storage-success-claim',24,1),('frame-version',4,3),('epoch',32,0),
                             ('result-failure',8,2),('result-extent',16,16385)):
    n = Native(2)
    def mutate(packet,frame,o=offset,v=value):
        data=bytearray(frame);struct.pack_into('<I',data,o,v);return bytes(data)
    n.mutate=mutate
    checked('staged-refuses-'+label,lambda:refused(lambda:tool.host.capture(
        tool.host.device_exchange(n,lambda:None,59,lambda _:None,now=lambda:1),0x1234,9,59,now=lambda:1,pause=lambda _:None,staged=True)))
n=Native(2)
checked('storage-mode-refuses-staged-frame',lambda:refused(lambda:tool.host.capture(
    tool.host.device_exchange(n,lambda:None,59,lambda _:None,now=lambda:1),0x1234,9,59,now=lambda:1,pause=lambda _:None)))
checked('staged-repeated-capture-refused',lambda:refused(lambda:tool.generation_count(2,2)))
n=Native(2)
def final_failure(packet,frame):
    if len(n.ops)==3:
        data=bytearray(frame);struct.pack_into('<I',data,8,2);return bytes(data)
    return frame
n.mutate=final_failure
checked('staged-final-status-failure',lambda:refused(lambda:tool.host.capture(
    tool.host.device_exchange(n,lambda:None,59,lambda _:None,now=lambda:1),0x1234,9,59,now=lambda:1,pause=lambda _:None,staged=True)))
assert n.ops==[0,1,0]
sources = ['apps/os/os.h','apps/os/prepare.py','apps/os/application.patch',
           'apps/os/recognition-placement.patch','apps/mimus-board/application.c','apps/mimus-board/application.h',
           'apps/mimus-board/recognition.cc','apps/mimus-board/usb0.c','apps/mimus-board/host.py',
           'tools/check-a733-mimus-board-supervised.py','checks/os-staged-recorded.c',
           'checks/os-staged-usb-base.h','checks/os-staged-recorded.py']
result = dict(passed=True,physical_access=False,cases=cases,
              image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),
              os_image_sha256=hashlib.sha256((args.build/'sunstone-a733-os.elf').read_bytes()).hexdigest(),
              sources={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in sources},
              scope='Actual AArch64 application/readiness/copy/USB exchange and host capture; inference completion, USB providers and scheduler modeled.')
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS staged recorded native/host checks:',len(cases))
