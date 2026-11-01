#!/usr/bin/env python3
"""Exercise tools/board.py with a fake FEL and board; no USB access.

The model covers ownership, transfers, WDT recovery and the durable queue.
Optional --hello prepares a real generic CPU-return ELF without a board.
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import types
from unittest.mock import patch

if not __debug__:
    raise RuntimeError('Checks require assertions.')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import board
import board_images as images

rows = []


def passed(name):
    rows.append({'name':name,'passed':True})
    print('PASS',name)


def arguments(root, *extra, submit=False):
    parser = argparse.ArgumentParser()
    board.run_options(parser,submit)
    parser.add_argument('--evidence',type=Path)
    if submit:
        parser.add_argument('--worker',default='worker')
        parser.add_argument('--hypothesis',default='the changed input reaches completion')
    image = root/'source.bin'
    if not image.exists():
        image.write_bytes(b'MODEL ONLY - never launch')
    values = [str(image),'--port','3-1','--evidence',str(root/'run')]
    if submit:
        values += ['--kind','probe','--expect','complete']
    return parser.parse_args(values+list(extra))


class Model:
    """Sparse fake memory and metadata, independent of production class state."""
    def __init__(self, variant):
        self.variant = variant
        self.number = 1
        self.pending = False
        self.lock = False
        self.memory = {}
        self.registers = {board.crumbs.WDT_MODE:0,board.crumbs.WDT_CONFIG:1,board.crumbs.WDT_OUTPUT:2,
                          **dict(zip((0x20,0x24,0x28,0x3c,0x40),(0xeb00008f,0xe3a00911,0xe3a0193d,0xe3a0605c,0xea00004d)))}
        self.calls = []
        self.quarantined = []
        self.launched = False
        self.clock = 0
        self.arm_failed = False
        self.permissions_waited = False

    @property
    def node(self):
        return Path(f'/MODEL/003/{self.number:03}')

    def discover(self, port):
        assert port=='3-1'
        if self.pending and self.variant!='no-return':
            self.number += 1
            self.pending = False
        return self.node

    @contextlib.contextmanager
    def board_lock(self,node):
        assert not self.lock
        self.lock = True
        try:
            yield node
        finally:
            self.lock = False

    def target(self,node):
        assert self.lock
        return Path('/MODEL/3-1')

    def identity(self,node):
        assert self.lock
        assert node==self.node
        return [1,self.number,3,4,5]

    def require_session(self,node):
        assert self.lock

    def require_rom(self,node):
        assert self.lock

    def quarantine_instance(self,node,reason,identity):
        assert self.lock
        self.quarantined.append((node,reason,identity))

    def now(self):
        return self.clock

    def sleep(self,seconds):
        self.clock += seconds

    def process(self,command,**kw):
        assert self.lock and command[1]=='-d'
        assert command[2]==f'3:{self.number}'
        args = command[3:]
        if self.variant == 'fel-after-return' and self.launched and args == ['readl', '0x20']:
            return types.SimpleNamespace(returncode=1, stdout='', stderr='injected post-return transport failure')
        self.calls.append(args)
        out = []
        i = 0
        while i<len(args):
            op = args[i]
            if op=='version':
                out.append('soc=00001903(A733)');i+=1
            elif op=='readl':
                out.append(hex(self.registers.get(int(args[i+1],0),0)));i+=2
            elif op=='writel':
                address,value=int(args[i+1],0),int(args[i+2],0)
                if self.variant=='stuck-disarm' and self.launched and address==board.crumbs.WDT_MODE and value==board.crumbs.KEY:
                    self.pending=True;i+=3;continue
                self.registers[address] = value&0xffff if address in (board.crumbs.WDT_MODE,board.crumbs.WDT_CONFIG,board.crumbs.WDT_OUTPUT) else value
                if address==board.crumbs.WDT_RESTART and self.registers[board.crumbs.WDT_MODE]==0x41:
                    self.pending=True
                if address==board.crumbs.WDT_MODE and value&1 and self.variant=='partial-arm' and not self.arm_failed:
                    self.arm_failed=True
                    self.pending=True
                    return types.SimpleNamespace(returncode=1,stdout='',stderr='injected failure after mode write')
                i+=3
            elif op=='write':
                address=int(args[i+1],0)
                data=Path(args[i+2]).read_bytes()
                for n,b in enumerate(data):self.memory[address+n]=b
                i+=3
            elif op=='read':
                address,size=int(args[i+1],0),int(args[i+2],0)
                data=bytes(self.memory.get(address+n,0) for n in range(size))
                if self.variant=='readback' and self.launched is False and address==0x41000000:
                    data=bytes([data[0]^1])+data[1:]
                Path(args[i+3]).write_bytes(data)
                i+=4
            elif op=='exe':
                self.launched=True
                self.pending=True
                self.registers[board.crumbs.RTC+0x100]=0x5c0d0001
                self.registers[board.crumbs.RTC+0x104]=(0x0410 if self.variant=='watchdog' else board.crumbs.RETURN_ROM)<<16
                if self.variant not in ('watchdog','stuck-disarm'):self.registers[board.crumbs.WDT_MODE]=0
                if self.variant == 'watchdog-before-entry':
                    self.registers[board.crumbs.RTC+0x100] = 0
                    self.registers[board.crumbs.WDT_MODE] = 0xb1
                elif self.variant == 'missing-return':
                    self.registers[board.crumbs.RTC+0x104] = 0
                elif self.variant == 'wrong-return-session':
                    self.registers[board.crumbs.RTC+0x100] ^= 1
                i+=2
            else:
                raise AssertionError(op)
        return types.SimpleNamespace(returncode=0,stdout='\n'.join(out),stderr='')


def lifecycle(root,variant,restore=False):
    root.mkdir()
    args=arguments(root)
    args.restore_rails=restore
    if variant=='mismatch':args.expect='a different output'
    model=Model(variant)
    owners=[]
    class Backend(board.Live):
        def __init__(self,*values):
            super().__init__(*values,access=model,process=model.process,now=model.now,sleep=model.sleep)
            owners.append(self)
        def dram(self):
            if variant=='dram-failure':raise board.DramFailure('training rejected')
        rail_phases = None
        def rails(self, phase='rails'):
            if self.rail_phases is None:self.rail_phases=[]
            self.rail_phases.append(phase)
            if variant == 'restore-failure' and phase == 'restore-rails':
                raise ValueError('Rail helper refused post-return supplies')
            if variant=='rail-refusal':raise ValueError('Rail helper refused CPU supplies')
        def live_state(self):
            words={hex(0x8871000+0x1000*d+o):v for d in range(3)
                   for o,v in ((0,board.state.BOOT_PLL[d]),(0x1c,board.state.BOOT_MUX[d]))}
            if variant=='restore-clock':words['0x8871000']=0
            return {'words':words}
        def prepare(self,info):
            return {'epoch':1,'generation':2,'watchdog':{'mode':0xb1,'session_seconds':1},
                    'uploads':[(0x41000000,args.image)],'launch':['exe','0x52000']}
        def capture(self,plan):
            if variant=='fel-after-return':self.tx('readl','0x20')
            if variant=='host-error':raise TypeError('injected decoder bug after normal return')
            if variant=='invalid-report':
                with patch.object(images,'capture',side_effect=ValueError('Application result belongs to another session')):
                    return super().capture(plan)
            if variant=='journal-error':
                self.journal_error=OSError('journal full after return')
            return {'valid':True,'payload':b'complete','report':{'state':4}}
    if variant in ('decoder-failure', 'decoder-invalid'):
        args.decoder=root/'application-decoder.py'
        args.decoder.write_text('def decode(payload):\n    return {"failed": True}\ndef failures(decoded):\n    return ["native job failed"]\n' if variant=='decoder-failure' else
                                'def decode(payload):\n    raise ValueError("malformed payload")\n')
    if variant in ('decoder-expected', 'decoder-wrong-expected'):
        args.decoder=root/'application-decoder.py'
        args.decoder.write_text('def decode(payload):\n    return {"bytes": len(payload)}\n'
                                'def expected(inputs):\n    raise AssertionError("Explicit expectation must override inference")\n')
        args.expect_result={'bytes':8 if variant=='decoder-expected' else 9}
    info={'entry_guard':True,'descriptor':{},'watchdog':{'mode':0xb1,'session_seconds':1}}
    with patch.object(images,'inspect',return_value=info),patch.object(images,'policy'), \
         patch.object(board.os,'access',return_value=True),contextlib.redirect_stdout(io.StringIO()), \
         (patch.object(board.crumbs,'parse_return',side_effect=ValueError('injected return decoder bug')) if variant=='return-decoder' else contextlib.nullcontext()):
        result=board.run_image(args,Backend)
    assert not model.lock
    assert json.loads(Path(result['result_path']).read_text())['outcome']==result['outcome']
    return result,model,owners[0]


def transport_cases(out):
    for variant in ('normal','mismatch','watchdog','no-return','host-error','dram-failure','rail-refusal','readback','partial-arm','journal-error','invalid-report','stuck-disarm','return-decoder'):
        result,model,owner=lifecycle(out/variant,variant)
        expected={'normal':'returned','mismatch':'result-mismatch','watchdog':'watchdog-reset','no-return':'no-return',
                  'host-error':'host-error','dram-failure':'refused-before-launch','rail-refusal':'refused-before-launch',
                  'readback':'result-mismatch','partial-arm':'host-error','journal-error':'host-error','invalid-report':'result-mismatch','stuck-disarm':'host-error','return-decoder':'host-error'}[variant]
        assert result['outcome']==expected,(variant,result)
        assert bool(model.quarantined)==(variant in ('no-return','stuck-disarm')),(variant,model.quarantined)
        if variant=='stuck-disarm':
            assert 'disarm readback stayed enabled' in result['first_failure']['detail']
        if variant in ('host-error','journal-error'):
            assert result['normal_return'] and not owner.armed
        if variant=='watchdog':
            assert result['dram_lost'] and result['breadcrumbs']['last']['name']!='RETURN_ROM'
            assert result['first_failure']['site']==result['breadcrumbs']['last']['name']
            assert not any(c[0]=='read' and int(c[1],0)>=0x41000000 for c in model.calls[model.calls.index(['exe','0x52000'])+1:])
        if variant=='partial-arm':
            assert result['transport_recovered'] and not owner.armed
        if variant=='no-return':
            # One fresh-instance wait, never an automatic second reset.
            assert sum('0x16aa0041' in c for c in model.calls)==1
        passed(variant)
    for variant,expected in (
            ('watchdog-before-entry','watchdog-reset'),('missing-return','host-error'),
            ('wrong-return-session','host-error'),('fel-after-return','host-error'),
            ('decoder-failure','result-mismatch'),('decoder-invalid','result-mismatch'),
            ('restore-failure','host-error'),('restore-clock','host-error'),
            ('normal','returned'),('watchdog','watchdog-reset'),('mismatch','result-mismatch')):
        result,model,owner=lifecycle(out/('restore-'+variant),variant,restore=True)
        assert result['outcome']==expected,(variant,result)
        assert bool(model.quarantined)==(variant in ('fel-after-return','missing-return','wrong-return-session')),(variant,model.quarantined)
        assert bool(result.get('rails_restored'))==(variant=='normal')
        assert owner.rail_phases==(['rails','restore-rails'] if variant in ('normal','restore-failure') else ['rails'])
        if variant=='watchdog-before-entry':
            assert result['dram_lost'] and not result['breadcrumbs']['session']
        if variant in ('missing-return','wrong-return-session','watchdog-before-entry'):
            assert not any(c[0]=='read' and int(c[1],0)>=0x41000000 for c in model.calls[model.calls.index(['exe','0x52000'])+1:])
        passed('restore-and-classification-'+variant)
    root=out/'journal-armed';root.mkdir()
    args=arguments(root);model=Model('normal');record={}
    owner=board.Live(args,root,record,access=model,process=model.process,now=model.now,sleep=model.sleep)
    with patch.object(board.os,'access',return_value=True),owner.locked():
        owner.arm(0x41)
        with patch.object(board,'save',side_effect=OSError('disk full')):
            owner.journal()
            owner.finish_pending()
        assert not owner.armed and owner.journal_error
    assert not model.quarantined
    passed('journal-failure-while-armed-recovers-without-quarantine')
    # A transport error before any native launch makes this instance unknown.
    root=out/'transport-error';root.mkdir();args=arguments(root);model=Model('normal');record={}
    owner=board.Live(args,root,record,access=model,process=lambda *a,**k:types.SimpleNamespace(returncode=1,stdout='',stderr='failed'),now=model.now,sleep=model.sleep)
    try:
        with owner.locked():owner.tx('version')
    except board.TransportFailure:pass
    else:raise AssertionError('transport error accepted')
    assert model.quarantined and len(model.calls)==0
    passed('transport-error-quarantines-unknown-instance')


def decoded_expectation_cases(out):
    for variant in ('decoder-expected', 'decoder-wrong-expected'):
        result, model, owner=lifecycle(out/variant, variant, restore=True)
        good=variant=='decoder-expected'
        assert result['outcome']==('returned' if good else 'result-mismatch'),result
        assert bool(result.get('rails_restored'))==good
        assert owner.rail_phases==(['rails','restore-rails'] if good else ['rails'])
        assert result['normal_return'] and not model.quarantined
        passed(variant)


def queue_cases(out):
    root=out/'queue';root.mkdir();queue=board.Queue(root/'state')
    args=arguments(root,submit=True)
    jobs=[]
    for n in range(3):
        args.hypothesis=f'input {n} reaches completion'
        jobs.append(queue.submit(args))
    first=queue.claim('3-1');assert first['id']==jobs[0]['id']
    assert queue.claim('3-1') is None
    queue.complete(first['id'],{'outcome':'returned','first_failure':None})
    second=queue.claim('3-1');assert second['id']==jobs[1]['id']
    queue.complete(second['id'],{'outcome':'no-return','first_failure':{'site':'wait'}})
    assert queue.claim('3-1') is None and (queue.root/'paused-3-1.json').exists()
    queue.resume('3-1');assert not (queue.root/'paused-3-1.json').exists()
    third=queue.claim('3-1');assert third['id']==jobs[2]['id']
    queue.complete(third['id'],{'outcome':'returned','first_failure':None})
    duplicate=queue.submit(args)
    assert json.loads(Path(duplicate['result_path']).read_text())['outcome']=='refused-before-launch'
    assert queue.status()['boards']['3-1']['pause']
    ledger=[json.loads(line) for line in (queue.root/'ledger.jsonl').read_text().splitlines()]
    assert len([e for e in ledger if e['event']=='entry'])==4
    assert len([e for e in ledger if e['event']=='result'])==4
    passed('queue-order-exclusion-no-return-pause-resume-duplicate-ledger')
    for rule in ('dram','site','quarantine'):
        q=board.Queue(root/rule)
        for n in range(3 if rule=='site' else 2):
            # New image hashes avoid the independent duplicate rule.
            args.image.write_bytes(f'MODEL {rule} {n}'.encode())
            args.hypothesis='same hypothesis'
            job=q.submit(args);assert q.claim('3-1')
            result={'outcome':'host-error','stage':'dram' if rule=='dram' else 'capture',
                    'first_failure':{'site':'0x41012340'},'quarantined':rule=='quarantine'}
            q.complete(job['id'],result)
            if rule=='quarantine':break
        assert q.status()['boards']['3-1']['pause'],rule
        passed('queue-pause-'+rule)
    q=board.Queue(root/'new-hypothesis')
    for n in range(3):
        args.image.write_bytes(f'MODEL changed {n}'.encode());args.hypothesis=f'new explanation {n}'
        job=q.submit(args);assert q.claim('3-1');q.complete(job['id'],{'outcome':'host-error','first_failure':{'site':'same'}})
    assert not q.status()['boards']['3-1']['pause']
    passed('new-hypothesis-allows-same-failure-site')
    q=board.Queue(root/'freeze');args.hypothesis='freeze input';job=q.submit(args)
    Path(job['image']).write_bytes(b'mutated')
    assert q.claim('3-1') is None
    assert json.loads(Path(job['result_path']).read_text())['first_failure']['detail'].startswith('Queued input bytes changed')
    passed('queued-image-changes-refused')
    q=board.Queue(root/'any');args.port='any';args.hypothesis='any board';job=q.submit(args)
    assert q.claim('3-3')['id']==job['id'] and q.claim('3-1') is None
    passed('any-port-is-assigned-once')
    q=board.Queue(root/'backstop');args.port='3-1'
    for n in range(41):
        args.image.write_bytes(f'MODEL hour {n}'.encode());args.hypothesis=f'case {n}';job=q.submit(args)
    assert json.loads(Path(job['result_path']).read_text())['first_failure']['detail'].startswith('Runaway backstop')
    passed('generous-hourly-backstop')


def policy_cases(out):
    root=out/'policy';root.mkdir();args=arguments(root)
    info={'generic':False,'descriptor':{}}
    args.storage_write=True
    try:images.policy(info,args)
    except ValueError as e:assert 'requires --approved-by coordinator' in str(e)
    else:raise AssertionError('storage write accepted')
    passed('unapproved-storage-write-refused')
    args.storage_write=False
    info['descriptor']={'operating_points':[{'block':'gpu','mhz':1200,'mv':960}]}
    try:images.policy(info,args)
    except ValueError as e:assert 'requires --approved-by coordinator' in str(e)
    else:raise AssertionError('unreviewed point accepted')
    args.approved_by='coordinator';images.policy(info,args)
    passed('unreviewed-operating-point-requires-coordinator')


def daemon_cases(out):
    root=out/'daemon';root.mkdir()
    args=arguments(root,submit=True)
    q=board.Queue(root/'recover');job=q.submit(args);assert q.claim('3-1')
    run=Path(job['directory'])/'run';run.mkdir()
    board.save(run/'result.json',{'outcome':'returned','first_failure':None})
    lease=Path(job['directory'])/'runner-process.json'
    identity=board.process_identity(os.getpid());board.save(lease,identity)
    with q.locked() as state:
        q.pause(state,'3-3','Fixture pause on the other board.')
    with contextlib.redirect_stdout(io.StringIO()):
        assert board.main(['--queue',str(q.root),'resume','3-3']) == 0
    boards=q.status()['boards']
    assert boards['3-1']['pause'] is None and len(boards['3-1']['running']) == 1
    assert boards['3-3']['pause'] is None
    passed('cli-resume-other-port-does-not-collect-or-pause-live-run')
    q.recover()
    boards=q.status()['boards']
    assert boards['3-1']['running'], 'A live child keeps its claim even after writing a result.'
    assert boards['3-1']['pause'] is None, 'Daemon recovery skips live children without pausing them.'
    with patch.object(board,'process_alive',return_value=False):
        with contextlib.redirect_stdout(io.StringIO()):
            assert board.main(['--queue',str(q.root),'resume','3-3']) == 0
        assert q.status()['boards']['3-1']['running'], 'Only the daemon collects stopped orphans.'
        q.recover()
    assert not q.status()['boards']['3-1']['running']
    assert json.loads(Path(job['result_path']).read_text())['outcome']=='returned'
    passed('daemon-recovery-waits-for-child-and-collects-run-result')
    q=board.Queue(root/'reversed')
    jobs=[]
    for port in ('3-3','3-1'):
        args.port=port;args.hypothesis='reversed '+port;jobs.append(q.submit(args))
    active=[q.claim(port) for port in ('3-1','3-3')]
    q.recover(j['id'] for j in active)
    assert not any(v['pause'] for v in q.status()['boards'].values())
    args.port='3-1'
    passed('daemon-excludes-both-active-children-in-reversed-order')
    q=board.Queue(root/'serve')
    for n in range(3):
        args.image.write_bytes(f'MODEL daemon {n}'.encode());args.hypothesis=f'daemon preparation {n}'
        job=q.submit(args)
        if n==0:Path(job['image']).write_bytes(b'mutated queued image')
    # Real runner child processes refuse these non-image fixtures in preflight.
    # Even an attempted discovery is a defect; these have no watchdog marker.
    q.serve(once=True)
    state=json.loads((q.root/'state.json').read_text())
    assert all(j['state']=='done' and j['result']['outcome']=='refused-before-launch' for j in state['jobs'])
    assert all(not j['result'].get('launch_attempted') for j in state['jobs'])
    passed('daemon-drains-after-input-refusal-and-records-child-results')


def artifacts(out):
    root=out/'qualification';root.mkdir()
    source=ROOT/'checks/board-runner-probe.S'
    subprocess.run(['/usr/bin/clang-21','--target=aarch64-none-elf','-c',str(source),'-o',str(root/'watchdog-probe.o')],check=True)
    subprocess.run([str(board.CACHE/'host/usr/bin/ld.lld-21'),'-Ttext=0x41000000','--oformat=binary',
                    str(root/'watchdog-probe.o'),'-o',str(root/'watchdog-probe.bin')],check=True)
    image=root/'watchdog-probe.bin'
    board.save(Path(str(image)+'.board.json'),{'schema':'sunstone-board-probe-v1','purpose':'watchdog-reset-diagnostic','image_sha256':images.sha(image),
        'entry':0x41000000,'report_address':0x42000000,'storage_write':False,'operating_points':[],
        'watchdog':{'mode':0x41,'session_seconds':1}})
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    uc.mem_map(0x41000000,4096);uc.mem_write(0x41000000,image.read_bytes())
    uc.mem_map(board.crumbs.RTC,4096)
    uc.emu_start(0x41000000,0x41000000+len(image.read_bytes()),count=20)
    tag,last=struct.unpack('<2I',uc.mem_read(board.crumbs.RTC+0x100,8))
    assert tag==0x5c0d0001 and last==0x01010000
    decoded=board.crumbs.decode(dict(tag=tag,last=last,time=0,fault=0,esr=0,elr=0,far=0),1)
    assert decoded['session'] and decoded['last']['name']=='EL3_ENTRY'
    import zlib
    (root/'hello-expected.bin').write_bytes(struct.pack('<IIQ',zlib.adler32(b'SunstoneOS board control\n'),0,len(b'SunstoneOS board control\n')))
    passed('watchdog-probe-real-instructions-and-control-expectation')


def offline(out,hello):
    source=json.loads((board.state.EVIDENCE/'sol-resume-20261031/port-3-3/cpu-thermal-state/session.json').read_text())
    words={}
    for t in source['transactions']:
        c=t['command'];addresses=[int(c[i+1],16) for i,x in enumerate(c) if x=='readl']
        words.update({hex(a):int(v,16) for a,v in zip(addresses,t['stdout'].split())})
    root=out/'offline';root.mkdir()
    args=arguments(root);args.image=hello
    plan=images.prepare(args,images.inspect(hello,'cpu-return'),{'words':words},root/'hello')
    records=images.load('check_generic_session',ROOT/'apps/os/host.py')
    raw=(root/'hello/session.bin').read_bytes();decoded=records.session_decode(raw)
    assert decoded['devices']==0 and decoded['flags']==records.BIND_SID
    assert records.INPUTS in decoded['extents'] and plan['format']=='generic'
    assert not (root/'hello/assets').exists()
    assert (root/'hello/launch.bin').stat().st_size<=0xc00
    passed('real-hello-typed-session-inputs-and-armed-launcher')
    args.kind='gpu-return';args.image=board.CACHE/'build/gpu-bisect-1031/crumbs/ladder-wdt4/2-firmware/sunstone-a733-os.elf'
    plan=images.prepare(args,images.inspect(args.image,args.kind),{'words':words},root/'gpu')
    assert plan['format']=='legacy' and plan['watchdog']==images.inspect(args.image,args.kind)['watchdog'] and 'gpu' in plan['geometry']
    passed('real-gpu-ladder-session-and-armed-launcher')



def copied_return_report(epoch):
    # Mirror the native return report fields: header, per-core retirement,
    # device-drained epoch and secondary power-off acknowledgment.
    raw=bytearray(8880)
    struct.pack_into('<8Q',raw,0,0x3154525133333741,3,len(raw),epoch,1000,4,0,0)
    for core in range(8):
        offset=688+core*1024
        struct.pack_into('<7Q',raw,offset,epoch,epoch,epoch,epoch,epoch if core else 0,epoch if core else 0,0)
        struct.pack_into('<Q',raw,offset+104,epoch)
        struct.pack_into('<Q',raw,offset+768,epoch if core else 0)
    return bytes(raw)


def gpu_capture_cases(out, gpu_images):
    """Run the real generic capture after fake FEL return with exact ELF layouts."""
    require_images = bool(gpu_images)
    assert require_images, 'gpu-capture requires --gpu-image for each real ELF under test'
    for index,image in enumerate(gpu_images):
        info=images.inspect(image,'gpu-return')
        geo=info['report_geometry'];gpu=geo['gpu_receipt']
        assert info['generic'] and 'thermal' in geo
        diagnostic=gpu.get('bisect')
        host=images.load('capture_fixture_host',ROOT/'apps/os/host.py')
        limit=host.image_manifest(image)['manifest']['output_limit']
        variants=['normal','foreign-output','foreign-gpu','foreign-thermal','thermal-unknown','thermal-incomplete','short-gpu']
        if diagnostic:
            variants+=['wrong-diagnostic','normal-for-diagnostic']
        else:
            variants+=['zero-jobs','diagnostic-for-normal']
        for variant in variants:
            root=out/f'gpu-{index}-{variant}';root.mkdir()
            args=arguments(root);args.image=image;args.kind='gpu-return';args.expect=None
            epoch,generation=1,2
            output=bytearray(56+limit);payload=b'complete'
            struct.pack_into('<7Q',output,0,0x31544c5553455241,1,len(output),epoch,generation,1,len(payload))
            output[56:56+len(payload)]=payload
            if variant=='foreign-output':struct.pack_into('<Q',output,24,epoch+1)
            version=3 if diagnostic else 1
            jobs=0 if diagnostic else 7
            flags=0 if diagnostic and diagnostic['stage']==1 else 63
            final=(diagnostic['stage']<<32)|diagnostic['limit'] if diagnostic else 960000
            if variant=='wrong-diagnostic':final^=1
            if variant=='normal-for-diagnostic':version,final=1,960000
            if variant=='diagnostic-for-normal':version,final=3,2<<32
            if variant=='zero-jobs':jobs=0
            receipt=struct.pack('<8Q',0x3152475033333741,version,64,
                                epoch+1 if variant=='foreign-gpu' else epoch,generation,jobs,flags,final)
            thermal=geo['thermal'];w=bytearray(thermal['window']['bytes']);r=bytearray(thermal['readings']['bytes'])
            # Native thermal.c publishes a 72-byte header plus existing rows.
            struct.pack_into('<4I5Q4I',w,0,0x5448524d,1,128,1,
                             epoch+1 if variant=='foreign-thermal' else epoch,generation,1,100,200,0,100000,56,1)
            for field,value in [('first',0),('collecting',1),('ready',1)]:
                struct.pack_into('<I',w,thermal['window']['members'][field],value)
            struct.pack_into('<Q5i6I4B',r,0,100,*([50000]*5),*([0]*5),1,31,0,0,0)
            struct.pack_into('<I',r,thermal['readings']['members']['count'],1)
            if variant=='thermal-unknown':struct.pack_into('<I',w,thermal['window']['members']['ready'],0)
            if variant=='thermal-incomplete':struct.pack_into('<Q',r,0,101)
            objects=[(geo['report_address'],copied_return_report(epoch)),(geo['output_address'],output),
                     (gpu['address'],receipt),(thermal['window']['address'],w),(thermal['readings']['address'],r)]
            model=Model('normal')
            for address,data in objects:model.memory.update({address+n:b for n,b in enumerate(data)})
            class Backend(board.Live):
                def __init__(self,*values):
                    super().__init__(*values,access=model,process=model.process,now=model.now,sleep=model.sleep)
                def dram(self):pass
                def rails(self):pass
                def prepare(self,info):
                    return {'epoch':epoch,'generation':generation,'watchdog':{'mode':0xb1,'session_seconds':1},
                            'uploads':[],'launch':['exe','0x52000'],'format':'generic','geometry':geo}
                def read(self,address,size,path):
                    data=super().read(address,size,path)
                    if variant=='short-gpu' and address==gpu['address']:
                        data=data[:-1];path.write_bytes(data)
                    return data
            with patch.object(board.os,'access',return_value=True),contextlib.redirect_stdout(io.StringIO()):
                result=board.run_image(args,Backend)
            invalid=variant != 'normal'
            assert result['outcome']==('result-mismatch' if invalid else 'returned'),(variant,result)
            report=result['return_report']
            assert report['secondaries_off']==7 and not report['qualification_passed']
            assert result['normal_return'] and not model.lock and not model.quarantined
            assert (root/'run/return-report.bin').read_bytes()==copied_return_report(epoch)
            assert (root/'run/output-record.bin').read_bytes()==output
            assert (root/'run/gpu-report.bin').exists() and (root/'run/thermal-window.bin').read_bytes()==w
            assert (root/'run/thermal-readings.bin').read_bytes()==r
            if variant=='foreign-output':
                assert report['output_capture_status']=='malformed' and report['gpu_capture_status']=='decoded'
                assert (root/'run/output.bin').read_bytes()==b''
            else:assert (root/'run/output.bin').read_bytes()==payload
            if variant in ('foreign-thermal','thermal-unknown'):
                assert report['thermal_capture_status']=='malformed' and 'thermal_error' in report
            elif variant=='thermal-incomplete':
                assert report['thermal_capture_status']=='decoded' and report['thermal']['classification']=='incomplete'
                assert 'thermal_error' in report
            else:
                assert report['thermal']['classification']=='complete'
                assert not report['thermal']['qualification_passed']
            if variant=='normal':
                assert report['gpu_capture_status']=='decoded' and not report['gpu']['qualification_passed']
                if diagnostic:assert report['gpu']['jobs']==0 and not report['gpu']['completed_gpu_work']
                else:assert report['gpu']['jobs']==7 and report['gpu']['completed_gpu_work']
            passed(f'generic-gpu-{index}-{variant}-preserves-native-return-and-raw-captures')
        inventory=board.references(args)
        for path in ('tools/board_images.py','apps/os/cpu-return-host.py','apps/os/gpu-return.py',
                     'apps/os/cpu-return-thermal.py','apps/os/thermal-report.py','apps/os/host.py'):
            assert inventory[str((ROOT/path).resolve())]==images.sha(ROOT/path)
        passed(f'generic-gpu-{index}-freezes-all-capture-decoder-inputs')


def report_cases(out,hello):
    root=out/'reports';root.mkdir()
    host=images.load('checked_return_report',ROOT/'apps/os/cpu-return-host.py')
    cpu=images.load('checked_cpu_layout',ROOT/'apps/cpu-runtime-qualification/host.py')
    epoch=123
    raw=bytearray(cpu.REPORT_BYTES)
    struct.pack_into('<8Q',raw,0,cpu.MAGIC,3,cpu.REPORT_BYTES,epoch,1000,4,0,0)
    for core in range(8):
        offset=cpu.CPU_OFFSET+core*cpu.CPU_BYTES
        struct.pack_into('<7Q',raw,offset,epoch,epoch,epoch,epoch,epoch if core else 0,epoch if core else 0,0)
        struct.pack_into('<Q',raw,offset+104,epoch)
        struct.pack_into('<Q',raw,offset+768,epoch if core else 0)
    assert host.decode_return_report(raw,epoch)['secondaries_off']==7
    for variant,reason in [('foreign','No successful CPU return report'),('drain','CPU 3 normal shutdown incomplete'),('short','Incomplete CPU return report')]:
        changed=bytearray(raw)
        if variant=='foreign':struct.pack_into('<Q',changed,24,epoch+1)
        elif variant=='drain':struct.pack_into('<Q',changed,cpu.CPU_OFFSET+3*cpu.CPU_BYTES+104,0)
        else:changed=changed[:-1]
        try:host.decode_return_report(changed,epoch)
        except ValueError as error:assert reason in str(error),(variant,error)
        else:raise AssertionError(variant+' accepted')
    passed('generic-retirement-report-success-session-drain-and-length')
    if hello:
        # Move the report symbol to MMIO. Structural preflight must refuse it
        # without constructing a transport owner or resetting a board.
        import io as _io
        from elftools.elf.elffile import ELFFile
        data=bytearray(hello.read_bytes());elf=ELFFile(_io.BytesIO(data));table=elf.get_section_by_name('.symtab')
        for index,symbol in enumerate(table.iter_symbols()):
            if symbol.name=='a733_qualification_report':
                struct.pack_into('<Q',data,table['sh_offset']+index*table['sh_entsize']+8,0x02050000)
                break
        else:raise AssertionError('report symbol absent')
        args=arguments(root);args.image=root/'malformed.elf';args.image.write_bytes(data)
        with contextlib.redirect_stdout(io.StringIO()):
            result=board.run_image(args,lambda *a:(_ for _ in ()).throw(AssertionError('preflight opened transport')))
        assert result['outcome']=='refused-before-launch' and 'CPU return Secure Device allocations differ' in result['first_failure']['detail']
        passed('malformed-report-mmio-address-refused-before-transport')
        try:images.inspect(hello,'npu')
        except ValueError as error:assert 'NPU kind requires the linked OS NPU service' in str(error)
        else:raise AssertionError('CPU image relabeled as NPU accepted')
        passed('npu-kind-refuses-image-without-native-npu-service')
        try:images.inspect(hello,'gpu-return')
        except ValueError as error:assert 'GPU kind requires the linked GPU return receipt' in str(error)
        else:raise AssertionError('CPU image relabeled as GPU accepted')
        passed('gpu-kind-refuses-image-without-linked-gpu-receipt')
        geo=images.inspect(hello,'cpu-return')['report_geometry']
        assert 'gpu_receipt' not in geo and 'thermal' not in geo
        limit=host.host.image_manifest(hello)['manifest']['output_limit']
        for foreign in (False,True):
            control=root/('cpu-foreign-output' if foreign else 'cpu-normal-output');control.mkdir()
            args=arguments(control);args.image=hello;args.kind='cpu-return';args.expect=None
            model=Model('normal')
            output=struct.pack('<7Q',0x31544c5553455241,1,56+limit,2 if foreign else 1,2,1,8)+b'complete'+bytes(limit-8)
            for address,data in [(geo['report_address'],copied_return_report(1)),(geo['output_address'],output)]:
                model.memory.update({address+n:b for n,b in enumerate(data)})
            class Backend(board.Live):
                def __init__(self,*values):
                    super().__init__(*values,access=model,process=model.process,now=model.now,sleep=model.sleep)
                def dram(self):pass
                def rails(self):pass
                def prepare(self,info):
                    return {'epoch':1,'generation':2,'watchdog':{'mode':0xb1,'session_seconds':1},
                            'uploads':[],'launch':['exe','0x52000'],'format':'generic','geometry':geo}
            with patch.object(board.os,'access',return_value=True),contextlib.redirect_stdout(io.StringIO()):
                result=board.run_image(args,Backend)
            assert result['outcome']==('result-mismatch' if foreign else 'returned'),result
            report=result['return_report']
            assert report['secondaries_off']==7 and report['thermal_capture_status']=='absent'
            assert report['output_capture_status']==('malformed' if foreign else 'decoded')
            assert result['normal_return'] and not model.lock and not model.quarantined
            assert (control/'run/output-record.bin').read_bytes()==output
            assert (control/'run/output.bin').read_bytes()==(b'' if foreign else b'complete')
            passed('generic-cpu-capture-'+('foreign' if foreign else 'normal'))


def owned_ddr_cases(out):
    for failed in (False,True):
        root=out/('owned-ddr-failure' if failed else 'owned-ddr-success');root.mkdir()
        args=arguments(root)
        build=root/'build';build.mkdir()
        files={}
        for name,size in [('boot0-head.bin',12288),('boot0-body.bin',233472),('ddr-entry.bin',64),('cache-off.bin',40)]:
            path=build/name;path.write_bytes(bytes([size%251])*size)
            files[name]={'size':size,'sha256':images.sha(path)}
        board.save(build/'manifest.json',{'files':files,'clock_point_word':15})
        args.ddr_build=build;args.training_dir=root/'records';args.training_dir.mkdir()
        (args.training_dir/'damaged.bin').write_bytes(b'invalid training record')
        args.fel=root/'fel';args.fel.write_bytes(b'MODEL FEL')
        class DDRModel(Model):
            def process(self,command,**kwargs):
                commands=command[3:];filtered=[];i=0
                while i<len(commands):
                    if commands[i]=='exe':
                        assert self.registers[board.crumbs.WDT_MODE]&1,'Every DRAM helper is host supervised.'
                        assert int(commands[i+1],0) in (0x71000,0x85000)
                        i+=2
                    else:
                        width={'read':4,'write':3,'readl':2,'writel':3,'version':1}[commands[i]]
                        filtered+=commands[i:i+width];i+=width
                return super().process(command[:3]+filtered,**kwargs)
        model=DDRModel('normal')
        model.registers.update({0x08871000:0xf8802600,0x0887101c:0x03010000,
            0x08872000:0xf8802700,0x0887201c:0x03000000,0x08873000:0xf8802600,0x0887301c:0x03010105})
        mailbox=struct.pack('<7I',0x53524444,5,4096,7 if failed else 0,0,0,0)+bytes(100)
        model.memory.update({0x86000+n:b for n,b in enumerate(mailbox)})
        owner=board.Live(args,root,{},access=model,process=model.process,now=model.now,sleep=model.sleep)
        with patch.object(board.os,'access',return_value=True),owner.locked():
            try:owner.dram()
            except board.DramFailure as error:
                assert failed and 'did not succeed' in str(error)
            else:assert not failed
            assert not owner.armed and not owner.failed
        assert not model.quarantined
        assert len(owner.record['dram_record_rejections'])==1 and owner.record['dram_training_record'] is None
        assert json.loads((root/'dram/session.json').read_text())['complete']==(not failed)
        passed('actual-ddr-owner-injection-'+('failure' if failed else 'success'))


def rail_cases(out):
    clocks=[]
    for name,pll,mux,mhz in [('CPU-L',0xf8802600,0x03010000,494),('CPU-B',0xf8802700,0x03000000,1014),('DSU',0xf8802600,0x03010105,494)]:
        for phase in ('before','after'):
            clocks.append(f'{name} {phase} pll={pll:#x} mux={mux:#x} mhz={mhz} pat0=0 pat1=0 lfm=0')
    common='\n'.join(clocks)+'\n'
    supply='SUPPLY dcdc5_uv=1000000 dcdc3_uv=800000 bin=4 dcxo_hz=26000000\n'
    log=common+supply+'VOLT dcdc5_selector=30 dcdc5_uv=800000\n'+board.state.RAIL_COMPLETE+'\n'
    assert images.rail_result((0x534b4552,5,0),log)['dcdc5_uv']==800000
    already=common+supply.replace('dcdc5_uv=1000000','dcdc5_uv=800000')+board.state.RAIL_COMPLETE+'\n'
    assert images.rail_result((0x534b4552,5,0),already)['already_normalized']
    try:images.rail_result((0x534b4552,5,1),already.replace('dcdc3_uv=800000','dcdc3_uv=1000000'))
    except ValueError as error:assert 'refused' in str(error)
    else:raise AssertionError('unsafe rail helper refusal accepted')
    passed('rail-normalization-idempotence-and-unsafe-supply-refusal')
    for variant in ('normal','expired','boundary','delayed-disarm','suspend'):
        expired=variant!='normal'
        root=out/('rail-'+variant);root.mkdir();args=arguments(root)
        args.rail_helper=root/'pinned-model.bin';args.rail_helper.write_bytes(b'MODEL PINNED HELPER')
        raw=bytearray(16384)
        encoded=log.encode();struct.pack_into('<4Q',raw,0,0x534b4552,5,0,len(encoded));raw[256:256+len(encoded)]=encoded
        class RailModel(Model):
            suspended=0
            def discover(self,port):
                if self.pending:
                    self.clock+={'normal':6,'expired':20,'boundary':14,'delayed-disarm':6,'suspend':6}[variant]
                    if variant=='suspend':self.suspended=20
                return super().discover(port)
            def process(self,command,**kwargs):
                if command[3:]==['reset64','0x41000000']:
                    assert self.registers[board.crumbs.WDT_MODE]==0xb1
                    self.pending=True
                    self.memory.update({0x42000000+n:b for n,b in enumerate(raw)})
                    return types.SimpleNamespace(returncode=0,stdout='',stderr='')
                result=super().process(command,**kwargs)
                if variant=='delayed-disarm' and command[3:]==board.crumbs.return_arguments():self.clock+=8
                return result
        model=RailModel('normal')
        model.registers[0x07090160]=0x10008002
        owner=board.Live(args,root,{},access=model,process=model.process,now=model.now,sleep=model.sleep,
                         elapsed_now=lambda:model.clock+model.suspended)
        with patch.object(board.state,'RAIL_HELPER_SHA256',images.sha(args.rail_helper)),patch.object(board.os,'access',return_value=True),owner.locked():
            try:owner.rails()
            except ValueError as error:assert expired and 'watchdog' in str(error)
            else:assert not expired
        assert not owner.armed and not model.quarantined
        if expired:assert not (root/'rails/rail-result.bin').exists(),'Never read DRAM after possible WDT expiry.'
        else:assert owner.record['rails']['dcdc5_uv']==800000
        passed('pinned-rail-helper-'+variant)


def generic_npu_case(out, hello):
    root=out/'generic-npu';root.mkdir()
    args=arguments(root,'--kind','npu');args.image=hello
    original_symbols=images.symbols
    def generic_symbols(image):
        # The real hello ELF exercises the generic session producer; only
        # the linked-service identity is modeled for NPU kind selection.
        return dict(original_symbols(image),os_npu_client_model=(1,1))
    with patch.object(images,'symbols',generic_symbols):
        info=images.inspect(hello,'npu')
        assert info['generic'] and not info.get('finite_npu')
        source=json.loads((board.state.EVIDENCE/'sol-resume-20261031/port-3-3/cpu-thermal-state/session.json').read_text())
        words={}
        for transaction in source['transactions']:
            command=transaction['command']
            addresses=[int(command[i+1],16) for i,value in enumerate(command) if value=='readl']
            words.update({hex(address):int(value,16) for address,value in zip(addresses,transaction['stdout'].split())})
        plan=images.prepare(args,info,{'words':words},root/'bundle')
        assert plan['format']=='generic'
        queued=arguments(root,'--kind','npu','--template',str(root/'bundle'),submit=True);queued.image=hello
        job=board.Queue(root/'queue').submit(queued)
        assert (Path(job['directory'])/'template/session.bin').read_bytes()==(root/'bundle/session.bin').read_bytes()
        reached=[]
        def backend(*args):
            reached.append(True)
            raise RuntimeError('modeled stop after preflight')
        result=board.run_image(args,backend)
        assert reached and 'modeled stop after preflight' in result['first_failure']['detail']
    model=Model('normal');owner=board.Live(args,root,{},access=model,process=model.process,now=model.now,sleep=model.sleep)
    read_words=[]
    def tx(*commands):
        read_words.extend(int(commands[i+1],16) for i,value in enumerate(commands) if value=='readl')
        return ' '.join('0' for _ in read_words)
    owner.tx=tx
    owner.live_state(False)
    assert tuple(read_words)==board.state.WORDS
    origin={'group':'npu','site':123,'caller':'0x41200100','value':'0x8'}
    decoded={'board_failure':origin,'last':{'name':'NPU_RETAINED'},'summary':'native failure'}
    with patch.object(board.crumbs,'decode',return_value=decoded):
        result,_,_=lifecycle(out/'native-first-failure','watchdog')
    assert result['first_failure']['site']=='npu:123:0x41200100'
    assert result['first_failure']['native_site']==123 and result['first_failure']['value']=='0x8'
    passed('generic-npu-keeps-typed-session-preflight-and-live-register-set')


def finite_npu_cases(out, image_root, state, fixture):
    live = json.loads(state.read_bytes())
    template = out/'npu-template'
    template.mkdir()
    (template/'session.bin').write_bytes((image_root/'session-template.bin').read_bytes())
    for name in ('local-restart','model-switch','sdk-access','power-cycle'):
        root = out/('npu-'+name)
        root.mkdir()
        image = root/'image.elf'
        image.write_bytes((image_root/name/'image.elf').read_bytes())
        (root/'manifest.json').write_bytes((image_root/name/'manifest.json').read_bytes())
        args = arguments(root,'--kind','npu','--template',str(template))
        args.image = image
        info = images.inspect(image,'npu')
        assert info['finite_npu'] and not info['entry_guard']
        plan = images.prepare(args,info,live,root/'bundle')
        assert plan['format']=='npu' and plan['watchdog']=={'mode':0xb1,'session_seconds':20}
        assert all(source.is_file() for _,source in plan['uploads'])
        if name == 'model-switch':
            plan.update(json.loads((fixture/'identities.json').read_bytes()))
            backup = root/'backup.bin'
            backup.write_bytes(bytes(plan['finite'].DRAM[1]))
            plan.update(dram_backup=backup,dram_backup_sha256=images.sha(backup))
            class Owner:
                evidence=root
                record={}
                restored=[]
                def read(self,address,size,path):
                    key={0x42000000:'monitor',plan['geometry']['log']:'platform',plan['geometry']['receipt']:'receipt'}[address]
                    raw=(fixture/'nominal'/(key+'.bin')).read_bytes()
                    assert len(raw)==size
                    path.write_bytes(raw)
                    return raw
                def upload(self,address,path):
                    assert address==0x41000000 and path.stat().st_size==0x1200000
                    self.restored.append(address)
                def journal(self):pass
            owner=Owner()
            captured=images.capture(owner,plan,args)
            assert captured['valid'] and owner.restored==[0x41000000] and owner.record['dram_restored']
            owner.restored.clear()
            original=owner.read
            def corrupt(address,size,path):
                raw=original(address,size,path)
                return bytes([raw[0]^1])+raw[1:] if address==plan['geometry']['receipt'] else raw
            owner.read=corrupt
            try:images.capture(owner,plan,args)
            except ValueError:pass
            else:raise AssertionError('Malformed finite retirement accepted')
            assert not owner.restored
            model=Model('normal')
            live_owner=board.Live(args,root,{},access=model,process=model.process,now=model.now,sleep=model.sleep)
            live_owner.read=corrupt
            with live_owner.locked():
                try:live_owner.capture(plan)
                except board.Mismatch:pass
                else:raise AssertionError('Malformed report accepted by live capture')
            assert live_owner.quarantined and not live_owner.record.get('dram_restored')
        if name == 'local-restart':
            calls=[]
            model=Model('normal')
            owner=board.Live(args,root,{},access=model,process=model.process,now=model.now,sleep=model.sleep)
            finite=plan['finite']
            original_check=finite.require_entry_state
            finite.require_entry_state=lambda tx:calls.append('entry')
            owner.read=lambda address,size,path:(path.write_bytes(bytes(size)),calls.append(('backup',address,size)))[0]
            owner.upload=lambda address,path:calls.append(('upload',address))
            owner.sanity=lambda:calls.append('rom')
            owner.arm=lambda mode:None
            def stop(*args):raise RuntimeError('stop before launch')
            owner.tx=stop
            try:
                try:owner.launch(plan)
                except RuntimeError:pass
            finally:finite.require_entry_state=original_check
            assert calls[0]=='entry' and calls[1]==('backup',0x41000000,0x1200000)
            assert calls[-2:]==['rom','entry'] and owner.record['npu_dirty']
            assert plan['dram_backup'].stat().st_size==0x1200000
            owner.tx=lambda *args:None
            owner.wait=lambda *args:None
            owner.returned=lambda:(0,0,{})
            normal={'session':True,'last':{'stage':board.crumbs.RETURN_ROM,'value':0}}
            with patch.object(finite,'require_entry_state'),patch.object(board.crumbs,'decode',return_value=normal) as decode:
                assert owner.launch(plan)=='returned'
                assert decode.call_args.args[2]==image
            try:
                with owner.locked():raise ValueError('modeled dirty preload refusal')
            except ValueError:pass
            assert owner.quarantined
            queued=arguments(root,'--kind','npu','--template',str(template),submit=True)
            queued.image=image
            job=board.Queue(root/'queue').submit(queued)
            frozen=Path(job['directory'])/'template'
            assert (frozen/'manifest.json').read_bytes()==(image.parent/'manifest.json').read_bytes()
            assert (frozen/'session.bin').read_bytes()==(template/'session.bin').read_bytes()
            refs=json.loads((Path(job['directory'])/'references.json').read_bytes())
            assert str(ROOT/'apps/npu-finite/development.py') in refs
            assert str(ROOT/'apps/npu-interrupt/host.py') in refs
        passed('finite-npu-frozen-preparation-'+name)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--hello',type=Path)
    parser.add_argument('--gpu-image',type=Path,action='append',help='Real generic GPU ELF for gpu-capture group; repeat for diagnostic and ordinary images.')
    parser.add_argument('--npu-images',type=Path)
    parser.add_argument('--npu-state',type=Path)
    parser.add_argument('--npu-fixture',type=Path)
    parser.add_argument('--only',default='transport,decoded-expectation,queue,policy,daemon,artifacts,reports,ddr,rails,offline',help='Comma-separated groups to resume.')
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    groups=args.only.split(',')
    if 'transport' in groups:transport_cases(args.out)
    if 'decoded-expectation' in groups:decoded_expectation_cases(args.out)
    if 'queue' in groups:
        with patch.object(board,'port_idle',return_value=True):queue_cases(args.out)
    if 'policy' in groups:policy_cases(args.out)
    if 'daemon' in groups:
        with patch.object(board,'port_idle',return_value=True):daemon_cases(args.out)
    if 'artifacts' in groups:artifacts(args.out)
    if 'reports' in groups:report_cases(args.out,args.hello)
    if 'gpu-capture' in groups:gpu_capture_cases(args.out,args.gpu_image)
    if 'ddr' in groups:owned_ddr_cases(args.out)
    if 'rails' in groups:rail_cases(args.out)
    if args.hello and 'offline' in groups:offline(args.out,args.hello)
    if 'generic-npu' in groups:generic_npu_case(args.out,args.hello)
    if 'npu' in groups:finite_npu_cases(args.out,args.npu_images,args.npu_state,args.npu_fixture)
    board.save(args.out/'results.json',rows)
    print(f'{len(rows)} checks passed; no device access')


if __name__=='__main__':main()
