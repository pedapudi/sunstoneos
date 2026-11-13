#!/usr/bin/env python3
"""Exercise tools/board.py with a fake FEL and board; no USB access.

The model covers ownership, transfers, WDT recovery and the durable queue.
Optional --hello prepares a real generic CPU-return ELF without a board; the generic-npu group also takes
--npu-image, a generic ELF whose application declares the NPU.
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
CARD_GATE = bytes.fromhex('00f020e3') * 6 + bytes(8)


def passed(name):
    rows.append({'name':name,'passed':True})
    print('PASS',name)


def arguments(root, *extra, submit=False, development=False):
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
        values += ['--kind','probe','--reference','docs/board-runner.md#submit-a-frozen-image-and-hypothesis']
    # Existing cases exercise the qualification behavior; development cases opt in.
    return parser.parse_args(values+([] if development else ['--preserve-dram'])+list(extra))


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
        if variant=='card-boot-once-stale':
            # The runner wrote the boot-once value but the board came back to FEL before a reset consumed it.
            self.registers.update({board.crumbs.RESET_WORDS['flag']:board.crumbs.BOOT_ONCE,board.crumbs.MARKER:board.crumbs.MARKER_KEYED})
        if variant=='card-cold-record':
            # A cold power-on ran the card gate and boot0, which found no package and entered FEL.
            # WDT0 mode reads its reset value 0 at a power-on gate entry.
            self.registers.update({board.crumbs.RESET_WORDS['flag']:0x0c028000,board.crumbs.MARKER:board.crumbs.MARKER_KEYED})
        self.calls = []
        self.programs = []
        self.quarantined = []
        self.launched = False
        self.clock = 0
        self.arm_failed = False
        self.permissions_waited = False

    def card_reset(self):
        """A reset of a board whose card holds the first stage, with the marker set: the gate takes its warm path,
        records the WDT0 mode it reads at entry (the mode armed before the reset; the board sets mode 1 only
        later, measured 2026-11-12), disarms WDT0 and enters FEL."""
        if not self.variant.startswith('card-') or self.registers.get(board.crumbs.MARKER,0)&0xffff!=board.crumbs.MARKER_SET:
            return
        flag=board.crumbs.RESET_WORDS['flag']
        if self.registers.get(flag)==board.crumbs.BOOT_ONCE:
            # The gate took its cold path once and consumed the value. The image ran its test, stopped renewing and WDT0 reset
            # the board through the warm path (card-once-other: boot0 found no package and entered FEL with the cold record).
            if self.variant=='card-once-other':
                self.registers[flag]=0x0c029041
            else:
                self.registers[flag]=0x0c024041
                self.registers[board.crumbs.RTC+0x100]=0x5c0d0001
                self.registers[board.crumbs.RTC+0x104]=board.crumbs.EL3_STOP<<16|0x5000
            self.registers[board.crumbs.WDT_MODE]=0
            return
        self.registers[flag]=0x0c024000|self.registers[board.crumbs.WDT_MODE]&0xff
        self.registers[board.crumbs.WDT_MODE]=0

    @property
    def node(self):
        return Path(f'/MODEL/003/{self.number:03}')

    def discover(self, port):
        assert port=='3-1'
        if self.pending and self.variant not in ('no-return','card-once-no-return') and not (self.variant in ('recovery-no-return','recovery-stale') and self.launched) \
                and self.clock>=getattr(self,'reset_at',0):
            self.number += 1
            self.pending = False
            if not self.launched:self.card_reset()  # the session's WDT0 reset reaches the new instance
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
        if self.variant=='recovery-precommand' and not self.launched and self.registers.get(0x07090108)==0x5aa5a55a:
            raise board.TransportFailure('injected identity failure before command start')
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
        self.programs.append(command[0])
        assert command[2]==f'3:{self.number}'
        assert self.number != getattr(self,'failed_launch_instance',None),'Request sent to the disconnected instance'
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
                if address==board.crumbs.RESET_WORDS['flag'] and value==board.crumbs.BOOT_ONCE and self.variant=='card-once-gp2-stuck':
                    i+=3;continue
                if address==board.crumbs.MARKER:
                    # A733 manual 9.1.7.20: only a keyed write takes effect.
                    if value>>16!=0x16aa or self.variant=='marker-stuck':i+=3;continue
                    if self.variant=='recovery-warm-unmarked':i+=3;continue
                    # Two outcomes the marker clear test separates: a keyed write of zero is ignored, or it clears
                    # the marker and (marker-clear-sticks) the restore then fails.
                    if self.variant=='marker-unclearable' and value&0xffff!=board.crumbs.MARKER_SET:i+=3;continue
                    if self.variant=='marker-clear-sticks' and self.registers.get(address,0)==board.crumbs.MARKER_CLEAR:i+=3;continue
                self.registers[address] = value&0xffff if address in (board.crumbs.WDT_MODE,board.crumbs.WDT_CONFIG,board.crumbs.WDT_OUTPUT) else value
                if address==board.crumbs.WDT_RESTART and self.registers[board.crumbs.WDT_MODE]==0x41:
                    self.pending=True
                    if self.registers.get(board.crumbs.RESET_WORDS['flag'])==board.crumbs.BOOT_ONCE:
                        # The card boots: the 4 s WDT0 interval, the gate and boot0 (DRAM training 5.1 s measured), the slot load
                        # (a few seconds) and the image's modeled 1 s lifetime and 4 s final interval, with no FEL instance meanwhile.
                        self.reset_at=self.clock+4+5.1+3+1+4
                        if self.variant=='card-once-late':self.reset_at=self.clock+board.boot_once_deadline(1)['total_s']+5
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
            elif op in ('exe','reset64'):
                entry_mode=self.registers.get(board.crumbs.WDT_MODE,0)
                self.launched=True
                self.pending=True
                self.registers[board.crumbs.RTC+0x100]=0x5c0d0001
                self.registers[board.crumbs.RTC+0x104]=(0x0410 if self.variant=='watchdog' else board.crumbs.RETURN_ROM)<<16
                if self.variant not in ('watchdog','stuck-disarm'):self.registers[board.crumbs.WDT_MODE]=0
                if self.variant.startswith('card-watchdog-'):
                    # The image armed WDT0 in this mode and stopped renewing it; the reset passes through the card gate.
                    self.registers[board.crumbs.WDT_MODE] = int(self.variant.rsplit('-',1)[1],16)
                    self.registers[board.crumbs.RTC+0x104] = 0x0410 << 16
                    self.card_reset()
                if self.variant == 'watchdog-before-entry':
                    self.registers[board.crumbs.RTC+0x100] = 0
                    self.registers[board.crumbs.WDT_MODE] = 0xb1
                elif self.variant == 'missing-return':
                    self.registers[board.crumbs.RTC+0x104] = 0
                elif self.variant == 'wrong-return-session':
                    self.registers[board.crumbs.RTC+0x100] ^= 1
                if self.variant.startswith('resident-'):
                    self.registers[board.crumbs.WDT_MODE] = 1
                    self.registers[board.crumbs.RTC+0x104] = board.crumbs.EL3_STOP << 16 | 0x5000
                    if self.variant == 'resident-stale':self.registers[board.crumbs.RTC+0x100] ^= 1
                    if self.variant == 'resident-wrong-status':self.registers[board.crumbs.RTC+0x104] ^= 0x1000
                    if self.variant == 'resident-wrong-stage':self.registers[board.crumbs.RTC+0x104] ^= 1 << 16
                    if self.variant == 'resident-secondary':self.registers[board.crumbs.RTC+0x10c] = 1 << 29
                    if self.variant in ('resident-fault', 'resident-failure'):
                        defines, _ = board.crumbs.header()
                        level = 3 if self.variant == 'resident-fault' else board.crumbs.FAIL_LEVEL
                        self.registers[board.crumbs.RTC+0x110] = defines['FAULT_MAGIC'] << 24 | level << 16
                    if self.variant == 'resident-malformed-fault':self.registers[board.crumbs.RTC+0x110] = 1
                    if self.variant == 'resident-normal':
                        self.registers[board.crumbs.WDT_MODE] = 0
                        self.registers[board.crumbs.RTC+0x104] = board.crumbs.RETURN_ROM << 16
                if self.variant.startswith('recovery-'):
                    recovery=images.load('model_card_recovery',ROOT/'tools/card_recovery.py')
                    assert args[i]=='exe' and kw['timeout']==recovery.EXE_SECONDS
                    if self.variant.startswith('recovery-warm'):
                        # No request: the gate reads the marker and stores its warm record with the mode it read.
                        requested=self.registers.get(recovery.RTC_FLAG)==recovery.FEL_FLAG or self.variant=='recovery-warm-as-request'
                        self.registers[board.crumbs.RTC+0x104]=0x0c020005 if requested else 0
                        self.registers[recovery.RTC_FLAG]=0 if requested else 0x0c024000|entry_mode
                        if self.variant=='recovery-warm-record':self.registers[recovery.RTC_FLAG]^=0x8000
                    else:
                        self.registers[recovery.RTC_FLAG]=0
                        self.registers[board.crumbs.RTC+0x104]=0x0c020005
                    # The gate re-arms WDT0 at 16 s instead of disarming it.
                    self.registers[board.crumbs.WDT_MODE]=recovery.GATE_WATCHDOG
                    if self.variant=='recovery-no-reenumerate':
                        # The ROM never re-enumerates; WDT0 expires and resets to FEL, leaving mode 1.
                        self.registers[board.crumbs.WDT_MODE]=1
                        self.reset_at=self.clock+recovery.INTERVAL_SECONDS
                    if self.variant=='recovery-late':self.reset_at=self.clock+recovery.INTERVAL_SECONDS
                    if self.variant=='recovery-progress':self.registers[board.crumbs.RTC+0x104]=0x0c020004
                    if self.variant=='recovery-tag':self.registers[board.crumbs.RTC+0x100]^=1
                    if self.variant=='recovery-flag':self.registers[recovery.RTC_FLAG]=recovery.FEL_FLAG
                    if self.variant=='recovery-watchdog':
                        self.registers[board.crumbs.WDT_MODE]=1
                        self.registers[board.crumbs.RTC+0x104]=0x0c020081
                        self.registers[recovery.RTC_FLAG]=recovery.FEL_FLAG
                    if self.variant=='recovery-fault':
                        defines,_=board.crumbs.header()
                        self.registers[board.crumbs.RTC+0x110]=defines['FAULT_MAGIC']<<24|3<<16
                    if self.variant=='recovery-malformed-fault':self.registers[board.crumbs.RTC+0x110]=1
                    if self.variant in ('recovery-disconnect','recovery-timeout','recovery-no-return','recovery-stale',
                                        'recovery-no-reenumerate','recovery-late'):
                        self.failed_launch_instance=self.number
                        if self.variant in ('recovery-timeout','recovery-no-reenumerate','recovery-late'):
                            self.clock+=kw['timeout']
                            raise subprocess.TimeoutExpired(command,kw['timeout'])
                        return types.SimpleNamespace(returncode=1,stdout='',stderr='injected execute disconnect')
                i+=2
            else:
                raise AssertionError(op)
        return types.SimpleNamespace(returncode=0,stdout='\n'.join(out),stderr='')


def lifecycle(root,variant,restore=False,dirty=None,grant='absent',resident=False,recovery=False,entry=1,configure=None):
    root.mkdir()
    args=arguments(root, development=resident or recovery)
    if configure:configure(args)
    if resident:args.lifetime='resident'
    if recovery:args.kind='probe';args.approved_by='coordinator'
    args.restore_rails=restore
    model=Model(variant)
    owners=[]
    class Backend(board.Live):
        def __init__(self,*values):
            super().__init__(*values,access=model,process=model.process,now=model.now,sleep=model.sleep)
            owners.append(self)
        def dram(self):
            if recovery:raise AssertionError('SRAM recovery initialized DRAM')
            if variant=='dram-failure':raise board.DramFailure('training rejected')
        def clear_stale_residue(self,*values):
            if recovery:raise AssertionError('SRAM recovery accessed credential DRAM')
            return super().clear_stale_residue(*values)
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
        def observe_return_power(self,phase):
            super().observe_return_power(phase)
            if variant=='resident-cleanup':
                self.record['cleanup_failure']='injected returned USB source cleanup failure'
        def prepare(self,info):
            if dirty:self.record[dirty]=True
            plan={'epoch':1,'generation':2,'watchdog':{'mode':0xb1,'session_seconds':1},
                  'uploads':[(0x41000000,args.image)],'launch':['exe','0x52000']}
            if resident:
                plan['lifetime']='resident'
                plan['watchdog']['mode']=0x41
            if recovery:
                plan.update(format='card-fel-recovery',nonce=1,launch=['exe','0x52000'],launch_seconds=2,uploads=[(0x52000,args.image)],
                    profile={'version':4,'gate_address':0x52000,'gate_bytes':len(CARD_GATE),'gate_watchdog':0xb1,'rtc_stage':0x0c02,
                             'final_value':5,'entry':entry,'gate_sha256':images.hashlib.sha256(CARD_GATE).hexdigest()},
                    watchdog={'mode':0x41,'gate_mode':0xb1,'session_seconds':60})
            if grant!='absent':plan['cpu_grant_raises']=grant
            return plan
        def capture(self,plan):
            if recovery:return super().capture(plan)
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
    info={'entry_guard':not recovery,'descriptor':{},'watchdog':{'mode':0xb1,'session_seconds':1}}
    if recovery:info['card_fel_recovery']=True
    with patch.object(images,'inspect',return_value=info),patch.object(images,'policy'), \
         patch.object(board.os,'access',return_value=True),contextlib.redirect_stdout(io.StringIO()), \
         (patch.object(board.crumbs,'parse_return',side_effect=ValueError('injected return decoder bug')) if variant=='return-decoder' else contextlib.nullcontext()):
        result=board.run_image(args,Backend)
    assert not model.lock
    assert json.loads(Path(result['result_path']).read_text())['outcome']==result['outcome']
    return result,model,owners[0]


def card_recovery_fixture(root, legacy=False, entry=1):
    """A structural ELF fixture; executing gate checks live separately."""
    root.mkdir()
    recovery=images.load('fixture_card_recovery',ROOT/'tools/card_recovery.py')
    digest=images.hashlib.sha256(CARD_GATE).digest()
    source=root/'fixture.S'
    source.write_text('.syntax unified\n.arm\n.section .card_gate,"ax"\n.global a733_card_fel_gate_image\n'
        '.type a733_card_fel_gate_image,%object\na733_card_fel_gate_image:\n.byte '+','.join(map(str,CARD_GATE))+
        '\n.size a733_card_fel_gate_image,.-a733_card_fel_gate_image\n'
        '.global a733_card_fel_gate_watchdog\n.type a733_card_fel_gate_watchdog,%object\n'
        'a733_card_fel_gate_watchdog: .word '+str(recovery.GATE_WATCHDOG)+'\n.size a733_card_fel_gate_watchdog,4\n'
        '.section .profile,"a"\n.balign 8\n.global a733_card_fel_profile\n.type a733_card_fel_profile,%object\n'
        'a733_card_fel_profile:\n.quad '+','.join(map(str,(recovery.PROFILE_MAGIC,4,104,recovery.GATE,len(CARD_GATE),0x0c02,5,
                                                         recovery.GATE_WATCHDOG,entry)))+
        '\n.byte '+','.join(map(str,digest))+'\n.size a733_card_fel_profile,104\n')
    script=root/'fixture.ld'
    script.write_text('ENTRY(a733_card_fel_gate_image)\nPHDRS { gate PT_LOAD FLAGS(5); profile PT_LOAD FLAGS(4); }\n'
        'SECTIONS { .card_gate 0x52000 : { *(.card_gate) } :gate '
        '.profile 0x52400 : { *(.profile) } :profile /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) } }\n')
    if legacy:
        source.write_text(source.read_text().replace('.syntax unified\n.arm\n',''))
        script.write_text(script.read_text().replace('0x52000','0x41000000').replace('0x52400','0x41001000'))
    subprocess.run([str(images.CLANG),'--target='+('aarch64-none-elf' if legacy else 'arm-none-eabi'),
                    '-c',str(source),'-o',str(root/'fixture.o')],check=True)
    subprocess.run([str(images.LLD),'-T',str(script),str(root/'fixture.o'),'-o',str(root/'fixture.elf')],check=True)
    return root/'fixture.elf'


def card_recovery_cases(out,image=None):
    recovery=images.load('check_card_recovery',ROOT/'tools/card_recovery.py')
    fixture=card_recovery_fixture(out/'card-recovery-fixture')
    info=images.inspect(fixture,'probe')
    assert info.get('card_fel_recovery') and not info.get('native_probe') and not info['entry_guard']
    args=arguments(fixture.parent,'--kind','probe','--approved-by','coordinator',development=True);args.image=fixture
    images.policy(info,args)
    unreviewed=fixture.parent/'unreviewed-fel';unreviewed.write_bytes(b'unreviewed host tool')
    saved=args.fel;args.fel=unreviewed
    try:images.policy(info,args)
    except ValueError:pass
    else:raise AssertionError('An unreviewed FEL binary was admitted')
    args.fel=saved
    passed('card-recovery-refuses-unreviewed-FEL-binary')
    for name,value in (('approved_by',None),('storage_write',True),('preserve_dram',True),('restore_rails',True),
                       ('input',[fixture]),('decoder',fixture)):
        saved=getattr(args,name);setattr(args,name,value)
        try:images.policy(info,args)
        except ValueError:pass
        else:raise AssertionError(('card recovery policy accepted',name))
        setattr(args,name,saved)
    args.marker_clear_test=True
    images.policy(info,args)
    args.approved_by=None
    try:images.policy(info,args)
    except ValueError as error:assert '--marker-clear-test' in str(error),error
    else:raise AssertionError('--marker-clear-test admitted without approval')
    args.marker_clear_test=False;args.approved_by='coordinator'
    passed('marker-clear-test-requires-coordinator-approval')
    with patch.object(images,'load',return_value=types.SimpleNamespace(classify=lambda _: 'write-capable')):
        try:images.policy(info,args)
        except ValueError:pass
        else:raise AssertionError('Card recovery SD core accepted')
    try:images.inspect(fixture,'cpu-return')
    except ValueError:pass
    else:raise AssertionError('Recovery profile accepted under another kind')
    plan=images.prepare(args,info,{},fixture.parent/'prepared')
    assert plan['epoch'] and plan['epoch']==plan['nonce']==plan['generation']
    assert plan['format']=='card-fel-recovery' and plan['launch']==['exe','0x52000']
    assert len(plan['uploads'])==2 and all(recovery.GATE<=a<recovery.SRAM_END for a,_ in plan['uploads'])
    assert not (fixture.parent/'prepared/request.bin').exists() and not (fixture.parent/'prepared/report-zero.bin').exists()
    assert 'tools/card_recovery.py' in board.session_files()
    assert recovery.FEL_ICACHE_PATCH in board.session_files()
    args.evidence=fixture.parent/'check';args.queue=fixture.parent/'offline-queue';args.state=None
    with contextlib.redirect_stdout(io.StringIO()):assert board.check_image(args)==0
    passed('card-recovery-ARM32-ELF-policy-nonce-offline-check-and-daemon-closure')
    legacy=card_recovery_fixture(out/'card-recovery-old-A64',legacy=True)
    try:images.inspect(legacy,'probe')
    except ValueError:pass
    else:raise AssertionError('The old AArch64 recovery executable was admitted')
    passed('card-recovery-refuses-ELF64-DRAM-executable')
    # Mutate real ELF marker, sections, loads and symbols rather than inspect booleans.
    from elftools.elf.elffile import ELFFile
    original=fixture.read_bytes();elf=ELFFile(io.BytesIO(original));table=elf.get_section_by_name('.symtab')
    symbol=next(s for s in table.iter_symbols() if s.name=='a733_card_fel_profile')
    section=elf.get_section(symbol['st_shndx']);offset=section['sh_offset']+symbol['st_value']-section['sh_addr']
    mutations=[('old-profile-version',offset+8,'Q',3),('marker-size',offset+16,'Q',96),
               ('gate-address',offset+24,'Q',recovery.GATE+4),('gate-size',offset+32,'Q',1024),
               ('rtc-stage',offset+40,'Q',0x0c01),('final-value',offset+48,'Q',4),
               ('profile-disarm-policy',offset+56,'Q',0),('profile-4s-policy',offset+56,'Q',0x41),
               ('unknown-entry',offset+64,'Q',3),('no-entry',offset+64,'Q',0),
               ('gate-hash',offset+72,'B',original[offset+72]^1),('wrong-ELF-entry',24,'I',recovery.ENTRY+4),
               ('A64-machine',18,'H',183)]
    gate=elf.get_section_by_name('.card_gate')
    mutations.append(('gate-bytes',gate['sh_offset'],'B',original[gate['sh_offset']]^1))
    mutations.append(('gate-disarm-policy-word',gate['sh_offset']+len(CARD_GATE),'I',0))
    for index,symbol in enumerate(table.iter_symbols()):
        if symbol.name=='a733_card_fel_gate_image':mutations.append(('gate-object-size',table['sh_offset']+index*table['sh_entsize']+8,'I',len(CARD_GATE)+4))
        if symbol.name=='a733_card_fel_profile':mutations.append(('profile-address',table['sh_offset']+index*table['sh_entsize']+4,'I',recovery.PROFILE_ADDRESS+8))
    for index,section in enumerate(elf.iter_sections()):
        if section.name=='.card_gate':mutations.append(('writable-gate',elf['e_shoff']+index*elf['e_shentsize']+8,'I',7))
        if section.name=='.profile':mutations.append(('uninitialized-profile',elf['e_shoff']+index*elf['e_shentsize']+4,'I',8))
    load=next((index,p) for index,p in enumerate(elf.iter_segments()) if p['p_type']=='PT_LOAD')
    ph=elf['e_phoff']+load[0]*elf['e_phentsize']
    mutations.extend([('writable-load',ph+24,'I',7),('NOLOAD-tail',ph+20,'I',load[1]['p_memsz']+4),
                      ('DRAM-load',ph+8,'I',0x41000000)])
    for name,position,fmt,value in mutations:
        bad=bytearray(original);struct.pack_into('<'+fmt,bad,position,value)
        target=fixture.parent/(name+'.elf');target.write_bytes(bad)
        try:images.inspect(target,'probe')
        except ValueError:pass
        else:raise AssertionError('Recovery ELF mutation admitted: '+name)
        passed('card-recovery-refuses-'+name)
    for failure in ('ordinary-nonzero','ordinary-timeout','recovery-oserror','recovery-unrelated-command','recovery-precommand-timeout'):
        root=out/failure;root.mkdir();model=Model('normal');record={};called=[]
        def failed_process(command,**kwargs):
            called.append(command)
            if failure.endswith('timeout'):raise subprocess.TimeoutExpired(command,kwargs['timeout'])
            if failure.endswith('oserror'):raise OSError('injected failure to start the FEL program')
            return types.SimpleNamespace(returncode=1,stdout='',stderr='injected nonzero FEL result')
        owner=board.Live(arguments(root,development=True),root,record,access=model,process=failed_process,now=model.now,sleep=model.sleep)
        with owner.locked():
            owner.armed=True;record['card_recovery_flag']={'armed':True}
            with (patch.object(model,'require_rom',side_effect=subprocess.TimeoutExpired(['precommand'],30))
                  if failure=='recovery-precommand-timeout' else contextlib.nullcontext()):
                try:
                    owner.tx(*(['readl','0x7090108'] if failure.endswith('command') else ['exe','0x52000']),
                             recovery_launch=failure.startswith('recovery-'))
                except (board.TransportFailure,ValueError):pass
                else:raise AssertionError('An unrelated or unstarted command was accepted provisionally')
            assert not any(t.get('provisional_disconnect') for t in record.get('transactions',[]))
            assert bool(called)==(failure not in ('recovery-unrelated-command','recovery-precommand-timeout'))
            owner.armed=False
        passed('card-recovery-transport-scope-'+failure)
    successful=('recovery-success','recovery-disconnect','recovery-timeout','recovery-warm')
    for variant in (*successful,'recovery-watchdog','recovery-progress','recovery-tag','recovery-flag','recovery-fault',
                    'recovery-malformed-fault','recovery-no-return','recovery-stale','recovery-precommand',
                    'recovery-no-reenumerate','recovery-late','recovery-warm-record','recovery-warm-unmarked',
                    'recovery-warm-as-request'):
        # A warm-entry proof sets no request; the gate stores its record instead.
        entry=2 if variant.startswith('recovery-warm') else 1
        result,model,owner=lifecycle(out/variant,variant,recovery=True,entry=entry)
        expected=variant in successful
        if variant=='recovery-warm-unmarked':
            # A marker that does not read back refuses at first contact, before any reset or upload.
            assert not board.result_passed(result) and 'cold-boot marker' in result['first_failure']['detail']
            assert not any(c[0] in ('exe','reset64') or hex(board.crumbs.WDT_MODE) in c for c in model.calls)
            passed('card-'+variant);continue
        assert bool(result.get('card_recovery_validated'))==expected,(variant,result)
        assert board.result_passed(result)==expected,(variant,result)
        assert owner.rail_phases is None,'The recovery proof changes no PMIC rail.'
        frozen=Path(result['evidence_path'])/'sunxi-fel'
        assert images.sha(frozen)==recovery.FEL_SHA256 and all(Path(p)==frozen for p in model.programs)
        assert result['recovery_fel']['sha256']==recovery.FEL_SHA256
        assert result['dram_mode']=='sram-only' and not result.get('dram_dirty') and not result.get('dram_restored')
        assert not result.get('expected_reset'),'A recovery proof cannot relabel watchdog failure.'
        assert not any(c[0] in ('read','write') and int(c[1],0)>=0x40000000 for c in model.calls)
        if variant=='recovery-precommand':
            assert not any(c[0]=='exe' for c in model.calls)
            assert not any(t.get('provisional_disconnect') for t in owner.record['transactions'])
        else:
            launch=model.calls.index(['exe','0x52000'])
            assert not any(c[0] in ('read','write') for c in model.calls[launch+1:]),'Returned ROM owns all SRAM; no RAM reads/restores'
            flag_write=['writel',hex(recovery.RTC_TAG),'0x5c0d0001','writel',hex(recovery.RTC_FLAG),
                        hex(recovery.FEL_FLAG if entry==1 else 0),'readl',hex(recovery.RTC_TAG),'readl',hex(recovery.RTC_FLAG),
                        'readl',hex(recovery.RTC_MARKER)]
            armed=[i for i,c in enumerate(model.calls[:launch]) if hex(board.crumbs.KEY|0x41) in c][-1]
            assert armed<model.calls.index(flag_write)<launch
            # The reviewed tool runs its I-cache disable helper before each MMIO
            # command, so the RTC arm command after the last upload is the
            # instruction-fetch predecessor of exe (checks/card-fel-recovery.py).
            uploads=[i for i,c in enumerate(model.calls[:launch]) if c[0]=='write']
            assert uploads and model.calls[launch-1]==flag_write and uploads[-1]<launch-1
        if expected:
            assert result['outcome']=='returned'
            assert result['return_report']['rtc_flag']==(0 if entry==1 else 0x0c024041)
            assert model.registers[recovery.RTC_FLAG]==0  # the runner cleared the gate record after reading it
            assert len(result['instances'])>=3 and not owner.failed
            # The gate left WDT0 re-armed; the runner read it, then disarmed it over FEL.
            returned=owner.record['watchdog_return']
            assert (returned['before'],returned['after'])==(recovery.GATE_WATCHDOG,0)
            assert model.registers[board.crumbs.WDT_MODE]==0
            assert result['return_report']['seconds_after_exe']<recovery.INTERVAL_SECONDS
        if variant in ('recovery-no-reenumerate','recovery-late'):
            # Without re-enumeration WDT0 resets to FEL; the crumbs still show the gate's last step.
            assert result['outcome']=='watchdog-reset' and not owner.failed
            back=result['card_recovery_return']
            assert back['rtc_flag']==0 and back['seconds_after_exe']>=recovery.INTERVAL_SECONDS
            assert owner.record['watchdog_return']['words']['last']==0x0c020005
            assert owner.record['watchdog_return']['before']==(1 if variant=='recovery-no-reenumerate' else recovery.GATE_WATCHDOG)
            assert model.registers[board.crumbs.WDT_MODE]==0
        if variant in ('recovery-disconnect','recovery-timeout'):
            assert any(t.get('provisional_disconnect') and t.get('command_started') for t in owner.record['transactions'])
        if variant=='recovery-watchdog':
            assert result['outcome']=='watchdog-reset' and result['card_recovery_return']['rtc_flag']==recovery.FEL_FLAG
        if variant in ('recovery-progress','recovery-tag','recovery-flag','recovery-fault','recovery-malformed-fault','recovery-no-return','recovery-stale',
                       'recovery-warm-record','recovery-warm-as-request'):
            assert result['quarantined'],(variant,result)
        passed('card-'+variant)
    if image is not None:
        actual=images.inspect(image,'probe');args.image=image
        images.policy(actual,args)
        prepared=images.prepare(args,actual,{},fixture.parent/'actual-image')
        assert prepared['format']=='card-fel-recovery' and prepared['launch']==['exe','0x52000']
        board.save(fixture.parent/'actual-admission.json',{'physical_access':False,'image_sha256':images.sha(image),
            'profile':prepared['profile'],'nonce':prepared['nonce'],'watchdog':prepared['watchdog']})
        passed('card-recovery-actual-frozen-image-admission')


def resident_cases(out):
    """A resident test succeeds only at its declared retained stop, never by reading DRAM."""
    for variant in ('resident-stop', 'resident-stale', 'resident-wrong-status', 'resident-wrong-stage',
                    'resident-secondary', 'resident-fault', 'resident-failure', 'resident-malformed-fault',
                    'resident-cleanup', 'resident-normal'):
        result, model, owner = lifecycle(out/variant, variant, resident=True)
        expected = variant == 'resident-stop'
        assert board.result_passed(result) == expected, (variant, result)
        if variant != 'resident-normal':
            assert result['outcome'] == 'watchdog-reset' and result['dram_lost'] and result['rom_returned']
        assert bool(result.get('expected_reset')) == expected, (variant, result)
        assert not any(c[0] == 'read' and int(c[1], 0) >= 0x41000000
                       for c in model.calls[model.calls.index(['exe', '0x52000'])+1:])
        if expected:
            assert result['first_failure'] is None and not owner.armed and not model.quarantined
        if variant=='resident-cleanup':
            assert result['breadcrumbs']['last']['stage']==board.crumbs.EL3_STOP
            assert result['breadcrumbs']['last']['value']==0x5000 and result['cleanup_failure']
        passed(variant)
    # The same retained stop under another lifetime remains a failed watchdog run.
    result, _, _ = lifecycle(out/'returning-resident-stop', 'resident-stop')
    assert not result.get('expected_reset') and not board.result_passed(result)
    passed('returning-stop-is-not-a-resident-success')
    root=out/'resident-policy';root.mkdir()
    args=arguments(root, '--lifetime', 'resident', development=True)
    info={'generic':True,'symbols':{'os_resident_test_profile':(0,24)},'descriptor':{}}
    args.kind='cpu-return'
    marker=struct.pack('<3Q',0x3154534554525353,1,0x5000)
    bounds=types.SimpleNamespace(classify=lambda _: 'read-only')
    with patch.object(images, 'symbol_bytes', return_value=marker), patch.object(images, 'load', return_value=bounds):
        images.policy(info,args)
        assert args.approved_by is None
        for name, value in (('preserve_dram',True),('kind','probe')):
            saved=getattr(args,name);setattr(args,name,value)
            try:images.policy(info,args)
            except ValueError:pass
            else:raise AssertionError(('resident policy accepted',name))
            setattr(args,name,saved)
        with patch.object(images, 'symbol_bytes', return_value=marker[:-1]+b'\x01'):
            try:images.policy(info,args)
            except ValueError:pass
            else:raise AssertionError('resident marker mismatch accepted')
    assert 'lifetime' in board.FROZEN_OPTIONS
    job={'image':str(args.image),'directory':str(root),'options':{'lifetime':'resident'}}
    command=board.Queue.runner_command(job,'3-1')
    assert command[command.index('--lifetime')+1]=='resident'
    passed('resident-policy-without-power-approval-marker-development-and-frozen-selection')


def resident_launcher_cases(out):
    """Run the real SRAM launcher instructions against the native resident contract."""
    import sram_launcher
    admission=images.load('runner_test_admission',ROOT/'tools/host_admission.py')
    producer=images.load('runner_test_request',ROOT/'apps/board/prepare.py')
    session=bytearray(904)
    struct.pack_into('<6Q',session,0,0x4153373342535332,2,592,1,2,3)
    identity=bytes(range(32));address=0x41001000
    request, contract=producer.request_bytes(address,identity,1,2,3,session)
    request=struct.pack('<Q',0x415337334f534c31)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,len(session))
    for lifetime, magic, mode in (('returning',0x415337334f544331,0xb1),('resident',0x415337334f524331,0x41)):
        root=out/('launcher-'+lifetime);root.mkdir()
        images.launcher(root,len(session),mode,60,magic=magic)
        launch=sram_launcher.execute([(address,identity)],(root/'launch.bin').read_bytes(),request,contract,session,
                                     magic,224,lifetime,watchdog=(mode,1,2),resident=lifetime=='resident')
        assert launch.stop==['handoff'] and launch.report[0]==12 and launch.session_intact and launch.canary_intact
        expected,_=admission.completed_contract(contract,60,magic,launch.ticks[0])
        assert launch.published==expected[:224],(lifetime,launch.published.hex(),expected[:224].hex())
        assert launch.writes==([(0x0205000c,0x14af)] if lifetime=='resident' else [])+[
            (sram_launcher.RVBAR,0x41000000),(sram_launcher.RVBAR+4,0)]
        if lifetime=='resident':
            assert launch.restart_ticks==[launch.ticks[0]]
            restarted=launch.events.index(('write',0x0205000c))
            assert launch.events[restarted+1:restarted+4]==[
                ('read',sram_launcher.MODE),('read',sram_launcher.CONFIG),('read',0x02050018)]
            assert struct.unpack_from('<2Q',launch.published,96)==((1<<64)-1,launch.ticks[0]+4*24000000)
            assert struct.unpack_from('<3Q',launch.published,120)==(0x41,1,2)
            assert not any(launch.published[144:176])
            for bad in ((0xb1,1,2),(0x41,0x101,2),(0x41,1,0)):
                try:sram_launcher.execute([(address,identity)],(root/'launch.bin').read_bytes(),request,contract,session,
                                          magic,224,str(bad),watchdog=bad,resident=True)
                except AssertionError as error:assert 'unexpectedly parked' in str(error),str(error)
                else:raise AssertionError(('resident launcher admitted incorrect WDT',bad))
            for flag in (sram_launcher.FLAG,0x070501c4,sram_launcher.ALT,0x07090108):
                try:sram_launcher.execute([(address,identity)],(root/'launch.bin').read_bytes(),request,contract,session,
                                          magic,224,hex(flag),watchdog=(mode,1,2),resident=True,registers={flag:1})
                except AssertionError as error:assert 'unexpectedly parked' in str(error),str(error)
                else:raise AssertionError(('resident launcher admitted active redirect or FEL flag',flag))
        passed('real-'+lifetime+'-launcher-contract-and-watchdog')
    for profile in (struct.pack('<3Q',1,0,3),struct.pack('<Q',1)):
        try:admission.contract_magic(profile,'resident')
        except ValueError:pass
        else:raise AssertionError('a linked profile without resident authority was admitted')
    passed('resident-contract-selection-refuses-unlinked-lifetime')
    nonzero=bytearray(contract);struct.pack_into('<Q',nonzero,120,0x41)
    try:admission.completed_contract(nonzero,60,0x415337334f524331)
    except ValueError:pass
    else:raise AssertionError('host completion concealed a nonzero initial watchdog field')
    passed('resident-preflight-refuses-prepopulated-watchdog-fields')


def resident_image_case(out, image, template):
    """Prepare the real bounded resident ELF and execute its own native admission."""
    root=out/'resident-image';root.mkdir()
    args=arguments(root,'--lifetime','resident','--template',str(template),development=True)
    args.image=image
    info=images.inspect(image,'cpu-return')
    words=images.offline_words()
    assert words,'The native admission needs captured board words.'
    results={}
    for lifetime in ('resident','returning'):
        args.lifetime=lifetime
        plan=images.prepare(args,info,{'words':words},root/lifetime)
        assert plan['host_admission'].get('admitted') is True,plan['host_admission']
        assert plan['lifetime']==lifetime and plan['watchdog']['mode']==(0x41 if lifetime=='resident' else 0xb1)
        results[lifetime]={'admission':plan['host_admission'],'watchdog':plan['watchdog'],
                           'launcher_sha256':images.sha(root/lifetime/'launch.bin')}
    board.save(root/'admission.json',{'physical_access':False,'coordinator_approval':False,
        'scope':'Host fixture of approved arguments; no launch authorization.',
        'image_sha256':images.sha(image),'lifetimes':results})
    passed('real-resident-image-and-returning-native-admission')


def automatic_restore_cases(out):
    """The runner restores the CPU rails after a session whose CPU clock grant admits a transition, unless the run opts
    out with --no-restore-rails; the option, frozen in a queue entry, reaches the runner as given."""
    # The grant fields the rule reads sit where the header puts them: the layout in tools/session.py, from which
    # cpu_grant_raises takes its offsets, against offsetof in include/a733_cpu_clock.h.
    layout=images.load('runner_grant_layout',ROOT/'tools/session.py')
    probe=out/'grant-offsets.c'
    probe.write_text('#include <stddef.h>\n#include "a733_cpu_clock.h"\n'+''.join(
        '_Static_assert(offsetof(struct a733_cpu_clock_grant_record,%s)==%d,"%s");\n'%(name,layout.offset('cpu_grant',name),name)
        for name in ('clusters','changes'))+
        '_Static_assert(sizeof(struct a733_cpu_clock_grant_record)==%d,"size");\n'%layout.LAYOUTS['cpu_grant'][1])
    compiled=subprocess.run(['clang','-fsyntax-only','-Wall','-Werror','-I',str(ROOT/'include'),str(probe)],capture_output=True,text=True)
    assert compiled.returncode==0,'the CPU grant layout in tools/session.py differs from include/a733_cpu_clock.h:\n'+compiled.stderr
    record=bytearray(96)
    struct.pack_into('<3Q',record,0,0x4153373343505531,1,96)
    assert images.cpu_grant_raises(None) is False and images.cpu_grant_raises(bytes(record)) is False
    assert images.cpu_grant_raises(bytes(95)) is None,'a record of another size is not read as granting nothing'
    for clusters,changes,raises in ((1,0,False),(0,4,False),(1,4,True),(2,1,True),(3,64,True)):
        struct.pack_into('<I',record,layout.offset('cpu_grant','clusters'),clusters)
        struct.pack_into('<I',record,layout.offset('cpu_grant','changes'),changes)
        assert images.cpu_grant_raises(bytes(record))==raises,(clusters,changes)
    for restore,grant,restored,choice in (
            (None,True,True,'grant admits a transition'),(None,False,False,'grant admits no transition'),
            (None,'absent',True,'grant not read'),(False,True,False,'opted out'),(True,False,True,'requested')):
        name='auto-%s-%s'%(restore,grant)
        result,model,owner=lifecycle(out/name,'normal',restore=restore,grant=grant)
        assert result['outcome']=='returned' and result['rail_restore_choice']==choice,(name,result.get('rail_restore_choice'))
        assert owner.rail_phases==(['rails','restore-rails'] if restored else ['rails']),(name,owner.rail_phases)
        assert bool(result.get('rails_restored'))==restored,name
    job={'image':'/image.elf','directory':str(out),'options':{}}
    for value,flags in ((True,['--restore-rails']),(False,['--no-restore-rails']),(None,[])):
        job['options']={'restore_rails':value}
        command=board.Queue.runner_command(job,'3-1')
        assert [c for c in command if 'restore-rails' in c]==flags,(value,command)
    for submit in (False,True):
        options=argparse.ArgumentParser();board.run_options(options,submit)
        given=lambda *extra:options.parse_args(['/image.elf','--port','3-1','--kind','cpu-return',*extra]).restore_rails
        assert (given(),given('--restore-rails'),given('--no-restore-rails'))==(None,True,False),submit
    passed('rails-restored-after-a-grant-that-admits-a-transition-unless-opted-out')


def marker_clear_cases(out):
    """--marker-clear-test: at first contact, with WDT0 read disarmed, one keyed write of zero, a read, the keyed set and a
    read. Both outcomes are recorded; a marker that does not read set again stops the run before launch."""
    marker=board.crumbs.MARKER
    for variant,expected,cleared in (('marker-clearable','returned',True),('marker-unclearable','returned',False),
                                     ('marker-clear-sticks','refused-before-launch',True)):
        root=out/variant
        def run(args):
            args.marker_clear_test=True;args.approved_by='coordinator'
        result,model,owner=lifecycle(root,variant,configure=run)
        assert result['outcome']==expected,(variant,result)
        test=owner.record['marker_clear_test']
        assert test['wdt_mode']==0 and test['clearable']==cleared,(variant,test)
        # 0x16aa0000 is also WDT0's disarm word (the same key), so the clear is found by its marker address.
        clears=[i for i,c in enumerate(model.calls) if any(c[j:j+3]==['writel',hex(marker),hex(board.crumbs.MARKER_CLEAR)]
                                                          for j in range(len(c)))]
        assert len(clears)==1,'The clear is tried once, at first contact.'
        clear=clears[0]
        assert model.calls[clear]==['writel',hex(marker),hex(board.crumbs.MARKER_CLEAR),'readl',hex(marker),
                                    'writel',hex(marker),hex(board.crumbs.MARKER_KEYED),'readl',hex(marker)]
        reset=[i for i,c in enumerate(model.calls) if '0x16aa0041' in c or c[0]=='exe']
        assert all(clear<i for i in reset),'The clear precedes every arm and launch.'
        if variant=='marker-clear-sticks':
            assert 'clear test restored' in result['first_failure']['detail'] and not reset
        else:
            assert test['after_restore']&0xffff==board.crumbs.MARKER_SET
        passed(variant)
    job={'image':'/image.elf','directory':str(out),'options':{'marker_clear_test':True,'approved_by':'coordinator'}}
    command=board.Queue.runner_command(job,'3-1')
    assert 'marker_clear_test' in board.FROZEN_OPTIONS and '--marker-clear-test' in command
    passed('marker-clear-test-is-frozen-into-the-runner-command')


def boot_once_cases(out):
    """--boot-once-slot: GP2 gets the boot-once value and reads back, WDT0 resets the board, nothing is launched or uploaded, and the
    runner waits with no FEL session for the card boot to end in the image's watchdog reset."""
    flag,slot=board.crumbs.RESET_WORDS['flag'],{'runtime_sha256':'0'*64,'slot':'A','generation':1}
    terms=board.boot_once_deadline(1)
    assert terms=={'arm_expiry_s':4,'gate_watchdog_s':16,'slot_load_s':30,'resident_session_s':1,'final_watchdog_s':4,'margin_s':10,'total_s':65},terms
    def run(variant):
        def configure(args):args.boot_once_slot=out/'slot.json';args.approved_by='coordinator'
        with patch.object(images,'boot_once_slot',return_value=slot):
            return lifecycle(out/variant,variant,resident=True,configure=configure)
    def assert_no_launch(model):
        assert not any(c[0] in ('exe','reset64') for c in model.calls),'The card boot launches nothing.'
        first=next(i for i,c in enumerate(model.calls) if c[:3]==['writel',hex(flag),hex(board.crumbs.BOOT_ONCE)])
        assert not any(c[0]=='write' for c in model.calls[first:]),'The card boot uploads nothing after the first contact.'
    result,model,owner=run('card-once-pass')
    boot=result['boot_once']
    assert result['outcome']=='watchdog-reset' and board.result_passed(result,decoded=False) and result['expected_reset'],result
    assert boot['gp2_written']==board.crumbs.BOOT_ONCE==boot['gp2_readback'] and boot['passed'],boot
    assert boot['reset_words']['flag']==0x0c024041 and boot['reset_words']['marker']==board.crumbs.MARKER_KEYED,boot
    assert boot['gate']=={'cold':False,'warm':True,'unmarked':False,'once':False,'refused_step':0,'mode':0x41},boot['gate']
    assert boot['last']==board.crumbs.EL3_STOP<<16|0x5000 and boot['stop_value']==0x5000 and boot['decoded']['last']['stage']==0x0111,boot
    assert boot['deadline']==terms and 13<boot['seconds_to_return']<terms['total_s'],boot
    assert result['boot_once_slot']==slot and result['dram_mode']=='sram-only' and not result['launch_attempted']
    assert_no_launch(model)
    write=next(i for i,c in enumerate(model.calls) if c[:3]==['writel',hex(flag),hex(board.crumbs.BOOT_ONCE)])
    arm=[i for i,c in enumerate(model.calls) if '0x16aa0041' in c]
    assert len(arm)==1 and write<arm[0],'One reset, after the value is written and read back.'
    assert model.registers[flag]==0 and not model.quarantined,'The gate record is cleared at the return.'
    assert [c for c in model.calls if c[:1]==['version']].__len__()==1,'No second FEL contact before the card boot ends.'
    passed('boot-once-pass')
    result,model,owner=run('card-once-gp2-stuck')
    assert result['outcome']=='refused-before-launch' and 'read back' in result['first_failure']['detail'],result
    assert result['boot_once']['gp2_readback']==0 and not any('0x16aa0041' in c for c in model.calls) and model.registers[flag]==0
    assert_no_launch(model)
    passed('boot-once-gp2-not-read-back-refused-before-reset')
    for variant in ('card-once-no-return','card-once-late'):
        result,model,owner=run(variant)
        assert result['outcome']=='no-return' and model.quarantined and not board.result_passed(result,decoded=False),(variant,result)
        assert sum('0x16aa0041' in c for c in model.calls)==1,'A board that has not returned is never reset again.'
        assert model.clock>=board.boot_once_deadline(1)['total_s'],(variant,model.clock)
        assert result['boot_once']['gp2_readback']==board.crumbs.BOOT_ONCE and 'watchdog_return' not in owner.record
        assert_no_launch(model)
        passed('boot-once-'+variant[len('card-once-'):])
    result,model,owner=run('card-once-other')
    boot=result['boot_once']
    assert result['outcome']=='watchdog-reset' and not board.result_passed(result,decoded=False) and not result['expected_reset'],result
    assert result['first_failure']['site']=='boot-once' and '0x0c029041'.lower() in result['first_failure']['detail'].lower(),result['first_failure']
    assert boot['gate']['cold'] and boot['gate']['once'] and boot['observed']['last']==0 and not boot['passed']
    assert_no_launch(model)
    passed('boot-once-different-record-fails-and-reports-it')
    # Admission: coordinator approval, the resident lifetime, and the frozen queue option.
    info={'generic':True,'dma':False,'symbols':{'os_resident_test_profile':0},'descriptor':{}}
    args=arguments(out,'--kind','cpu-return',development=True);args.boot_once_slot=out/'slot.json'
    profile=struct.pack('<3Q',0x3154534554525353,1,0x5000)
    with patch.object(images,'symbol_bytes',return_value=profile),patch.object(images,'boot_once_slot',return_value=slot):
        for lifetime,approved,word in (('resident',None,'coordinator'),('returning','coordinator','resident')):
            args.lifetime,args.approved_by=lifetime,approved
            try:images.policy(info,args)
            except ValueError as error:assert word in str(error) and ('--boot-once-slot' in str(error) or lifetime=='returning'),error
            else:raise AssertionError(('--boot-once-slot admitted',lifetime,approved))
        args.lifetime,args.approved_by='resident','coordinator'
        real_load=images.load
        with patch.object(images,'load',lambda name,path:types.SimpleNamespace(classify=lambda _:'no-core') if name=='storage_bounds' else real_load(name,path)):
            images.policy(info,args)
    job={'image':'/image.elf','directory':str(out),'options':{'boot_once_slot':'/slot.json','approved_by':'coordinator','lifetime':'resident'}}
    command=board.Queue.runner_command(job,'3-1')
    assert 'boot_once_slot' in board.FROZEN_OPTIONS and command[command.index('--boot-once-slot')+1]=='/slot.json',command
    passed('boot-once-requires-coordinator-approval-and-is-frozen')


def boot_once_slot_cases(out):
    """The submitted ELF must flatten to the runtime the slot manifest names."""
    runtime=b'flat runtime'
    manifest=out/'plan.json';args=types.SimpleNamespace(image=out/'image.elf',boot_once_slot=manifest)
    def write(digest,**extra):
        manifest.write_text(json.dumps({'kind':'a733-boot-slot-plan','slot':'A','generation':3,'entry':0x41000000,
            'extents':[{'name':'runtime.bin','kind':1,'load':0x41000000,'bytes':len(runtime),'sha256':digest},
                       {'name':'session.bin','kind':2,'load':0x41f00000,'bytes':4,'sha256':'1'*64}],**extra}))
    card=types.SimpleNamespace(flat_runtime=lambda image:runtime)
    with patch.object(images,'load',return_value=card):
        write(images.hashlib.sha256(runtime).hexdigest())
        record=images.boot_once_slot(args)
        assert record['slot']=='A' and record['generation']==3 and record['runtime_sha256']==images.hashlib.sha256(runtime).hexdigest()
        write('2'*64)
        try:images.boot_once_slot(args)
        except ValueError as error:assert 'another image' in str(error),error
        else:raise AssertionError('A slot manifest for another image was accepted')
        manifest.write_text('{}')
        try:images.boot_once_slot(args)
        except ValueError:pass
        else:raise AssertionError('A manifest without extents was accepted')
    passed('boot-once-slot-manifest-names-the-submitted-image')


def transport_cases(out):
    for variant in ('normal','watchdog','no-return','host-error','dram-failure','rail-refusal','readback','partial-arm','journal-error','invalid-report','stuck-disarm','return-decoder',
                    'card-watchdog-41','card-watchdog-b1','card-cold-record','card-boot-once-stale','marker-stuck'):
        result,model,owner=lifecycle(out/variant,variant)
        expected={'normal':'returned','watchdog':'watchdog-reset','no-return':'no-return','card-watchdog-41':'watchdog-reset','card-watchdog-b1':'watchdog-reset',
                  'card-cold-record':'returned','card-boot-once-stale':'returned','marker-stuck':'refused-before-launch',
                  'host-error':'host-error','dram-failure':'refused-before-launch','rail-refusal':'refused-before-launch',
                  'readback':'refused-before-launch','partial-arm':'host-error','journal-error':'host-error','invalid-report':'host-error','stuck-disarm':'host-error','return-decoder':'host-error'}[variant]
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
        flag,marker=board.crumbs.RESET_WORDS['flag'],board.crumbs.MARKER
        if variant=='normal':
            # The first contact sets the cold-boot marker; the instance after the session's reset finds it retained.
            epochs=result['power_epoch']
            assert epochs[0]['words']['marker']==0 and epochs[0]['marker_after']==board.crumbs.MARKER_KEYED
            assert all(e['words']['marker']==board.crumbs.MARKER_KEYED and 'marker_after' not in e for e in epochs[1:])
            first=next(i for i,c in enumerate(model.calls) if hex(marker) in c and 'writel' in c)
            reset=next(i for i,c in enumerate(model.calls) if '0x16aa0041' in c)
            assert first<reset,'The marker is set before the session reset can reach a card.'
        if variant.startswith('card-'):
            # The session's own WDT0 reset (mode 0x41) passed through the card gate; the runner cleared its record.
            session=result['power_epoch'][1]
            assert session['words']['flag']==0x0c024041 and session['flag_after']==0,(variant,session)
            assert session['gate']=={'cold':False,'warm':True,'unmarked':False,'once':False,'refused_step':0,'mode':0x41}
        if variant.startswith('card-watchdog-'):
            # The gate disarmed WDT0; the record's low byte is the mode the image armed, so the return is a watchdog reset.
            armed=int(variant.rsplit('-',1)[1],16)
            back=owner.record['watchdog_return']
            assert back['reset_words']['flag']==0x0c024000|armed,(variant,back)
            assert back['before']==0 and back['mode_at_reset']==armed and back['gate']['mode']==armed and back['gate']['warm']
            assert result['dram_lost'] and model.registers[flag]==0
        if variant=='card-boot-once-stale':
            # First contact clears it before the session's reset, so that reset takes the warm path.
            first=result['power_epoch'][0]
            assert first['words']['flag']==board.crumbs.BOOT_ONCE and first['gate'] is None and first['flag_after']==0,first
            clear=next(i for i,c in enumerate(model.calls) if c[:3]==['writel',hex(flag),'0x0'])
            reset=next(i for i,c in enumerate(model.calls) if '0x16aa0041' in c)
            assert clear<reset
        if variant=='card-cold-record':
            assert result['power_epoch'][0]['gate']=={'cold':True,'warm':False,'unmarked':False,'once':False,'refused_step':0,'mode':0}
            assert result['power_epoch'][0]['flag_after']==0 and 'marker_after' in result['power_epoch'][0]
        if variant=='marker-stuck':
            assert 'cold-boot marker' in result['first_failure']['detail']
            assert not any('0x16aa0041' in c or c[0]=='exe' for c in model.calls)
        if variant=='no-return':
            # One fresh-instance wait, never an automatic second reset.
            assert sum('0x16aa0041' in c for c in model.calls)==1
        passed(variant)
    marker_clear_cases(out)
    boot_once_cases(out)
    boot_once_slot_cases(out)
    for variant,expected in (
            ('watchdog-before-entry','watchdog-reset'),('missing-return','host-error'),
            ('wrong-return-session','host-error'),('fel-after-return','host-error'),
            ('decoder-failure','returned'),('decoder-invalid','host-error'),
            ('restore-failure','host-error'),('restore-clock','host-error'),
            ('normal','returned'),('watchdog','watchdog-reset')):
        result,model,owner=lifecycle(out/('restore-'+variant),variant,restore=True)
        assert result['outcome']==expected,(variant,result)
        assert bool(model.quarantined)==(variant in ('fel-after-return','missing-return','wrong-return-session')),(variant,model.quarantined)
        assert bool(result.get('rails_restored'))==(variant in ('normal','decoder-failure','decoder-invalid'))
        assert owner.rail_phases==(['rails','restore-rails'] if variant in ('normal','decoder-failure','decoder-invalid','restore-failure') else ['rails'])
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


def development_dram_cases(out):
    """A development run transfers no DRAM; --preserve-dram keeps the save, restore and DRAM quarantine."""
    normal={'session':True,'last':{'stage':board.crumbs.RETURN_ROM,'value':0}}
    for preserve in (False,True):
        root=out/('dram-mode-'+('preserve' if preserve else 'development'));root.mkdir()
        args=arguments(root,development=not preserve)
        assert images.preserving(args)==preserve
        model=Model('normal')
        owner=board.Live(args,root,{},access=model,process=model.process,now=model.now,sleep=model.sleep)
        owner.node,owner.identity=Path('/MODEL/003/009'),[1]
        owner.tx=lambda *values:'';owner.wait=lambda *values:None;owner.arm=lambda *values,**keywords:None
        owner.returned=lambda:(0,0,{});owner.usb_source_off=lambda *values,**keywords:None
        plan={'format':'generic','uploads':[],'preserve_regions':[(0x41000000,4096)],'epoch':1,
              'watchdog':{'mode':0xb1,'session_seconds':1},'launch':['exe','0x52000']}
        saved=[{'address':0x41000000,'bytes':4096,'file':'x','sha256':'0'}]
        with patch.object(images,'validate_uploads'),patch.object(images,'backup_dram',return_value=saved) as backup,\
             patch.object(board.crumbs,'decode',return_value=normal):
            assert owner.launch(plan)=='returned'
        assert backup.called==preserve and ('dram_backups' in owner.record)==preserve
        assert bool(owner.record.get('dram_dirty'))==preserve and bool(owner.record.get('dram_unsaved'))==(not preserve)
        # A failure after validated retirement: development keeps the instance, preserve quarantines it.
        for validated in (True,False):
            owner.quarantined=False;owner.record['quarantined']=False
            owner.record['native_retirement_validated']=validated
            with patch.object(images,'capture',side_effect=ValueError('modeled decode failure')),\
                 patch.object(owner.access,'quarantine_instance') as quarantine:
                try:owner.capture(plan)
                except board.Mismatch:pass
                else:raise AssertionError('capture failure accepted')
            expected=True if preserve else not validated
            assert bool(quarantine.called)==expected,(preserve,validated)
            if quarantine.called:
                reason=quarantine.call_args.args[1]
                assert ('DRAM restoration' in reason)==preserve and 'retirement' in reason
        # Settling ownership: only a run that saved DRAM can leave it unresolved.
        owner.quarantined=False;owner.record.pop('quarantined',None)
        with patch.object(owner.access,'quarantine_instance') as quarantine:
            owner.settle_dram_ownership()
        assert bool(quarantine.called)==preserve
        passed('dram-mode-'+('preserve-keeps-save-restore-and-quarantine' if preserve else 'development-skips-transfers-and-never-quarantines-for-dram'))
    # The mode is recorded in the result and travels through the queue as a frozen option.
    root=out/'dram-mode-queue';root.mkdir()
    for flag in (False,True):
        queue=board.Queue(root/('q-'+str(flag)));args=arguments(root,submit=True,development=not flag)
        job=queue.submit(args)
        assert job['options']['preserve_dram']==flag
    passed('dram-mode-frozen-in-queue-entry')


def reference_cases(out):
    """A submission cites a tracked docs heading before anything is queued; only the coordinator exempts one."""
    tree = out/'reference-tree';(tree/'docs').mkdir(parents=True)
    (tree/'docs/known.md').write_text('# Vendor sequence\n\n```\n# not a heading\n```\n\n## Step `one`, with: punctuation?\n## Repeat\n## Repeat\n')
    (tree/'docs/untracked.md').write_text('# Untracked\n')
    (tree/'outside.md').write_text('# Outside\n')
    for command in (['init','-q'],['add','docs/known.md','outside.md'],
                    ['-c','user.name=t','-c','user.email=t@example.invalid','commit','-q','-m','fixture']):
        subprocess.run(['git','-C',str(tree),*command],check=True,capture_output=True)
    assert board.heading_anchors((tree/'docs/known.md').read_text())==[
        'vendor-sequence','step-one-with-punctuation','repeat','repeat-1']
    def attempt(name,*extra,reference=True):
        root=out/('reference-'+name);root.mkdir()
        args=arguments(root,submit=True,development=True)
        args.host_tree=tree
        args.reference=None
        queue=board.Queue(root/'queue')
        before=list((queue.root/'entries').glob('*')) if (queue.root/'entries').exists() else []
        for key,value in zip(extra[::2],extra[1::2]):setattr(args,key,value)
        try:job=queue.submit(args)
        except ValueError as error:
            entries=list((queue.root/'entries').glob('*')) if (queue.root/'entries').exists() else []
            assert entries==before and not (queue.root/'state.json').exists(),(name,entries)
            return str(error)
        return job
    for name,reference,message in (
            ('missing',None,'requires --reference PATH#ANCHOR'),
            ('no-anchor','docs/known.md','must read PATH#ANCHOR'),
            ('absent-file','docs/absent.md#vendor-sequence','does not exist'),
            ('outside-docs','outside.md#outside','not under docs/'),
            ('untracked','docs/untracked.md#untracked','not tracked'),
            ('unknown-anchor','docs/known.md#vendor','matches no heading'),
            ('fenced-heading','docs/known.md#not-a-heading','matches no heading')):
        error=attempt(name,'reference',reference)
        assert isinstance(error,str) and message in error,(name,error)
        passed('reference-refused-before-queue-'+name)
    job=attempt('valid','reference','docs/known.md#repeat-1')
    assert job['reference']['path']=='docs/known.md' and job['reference']['anchor']=='repeat-1'
    assert job['options']['reference']=='docs/known.md#repeat-1' and len(job['reference']['sha256'])==64
    assert json.loads((Path(job['directory'])/'entry.json').read_text())['reference']==job['reference']
    # The same entry reaches the runner as a flag and lands in the result.
    parsed=argparse.ArgumentParser();board.run_options(parsed)
    run=parsed.parse_args([job['image'],'--port','3-1','--reference',job['options']['reference']])
    assert board.run_reference(run)=={'path':'docs/known.md','anchor':'repeat-1'}
    passed('reference-stored-in-entry-and-passed-to-the-run')
    absolute=attempt('absolute','reference',str(tree/'docs/known.md')+'#step-one-with-punctuation')
    assert absolute['reference']['path']=='docs/known.md'
    passed('reference-accepts-absolute-path-and-github-anchor-rules')
    for name,fields,message in (
            ('none-alone',('reference','none'),'requires --approved-by coordinator'),
            ('none-no-reason',('reference','none','approved_by','coordinator'),'nonempty --reference-reason'),
            ('none-blank-reason',('reference','none','approved_by','coordinator','reference_reason','  '),'nonempty --reference-reason'),
            ('none-reason-only',('reference','none','reference_reason','reads hardware facts'),'requires --approved-by coordinator')):
        error=attempt(name,*fields)
        assert isinstance(error,str) and message in error,(name,error)
        passed('reference-'+name+'-refused')
    exempt=attempt('exempt','reference','none','approved_by','coordinator','reference_reason','reads hardware facts')
    assert exempt['reference']=={'none':True,'approved_by':'coordinator','reason':'reads hardware facts'}
    assert exempt['options']['reference']=='none' and exempt['options']['reference_reason']=='reads hardware facts'
    run=parsed.parse_args([exempt['image'],'--port','3-1','--reference','none','--approved-by','coordinator',
                           '--reference-reason','reads hardware facts'])
    assert board.run_reference(run)['reason']=='reads hardware facts'
    passed('reference-none-accepted-only-with-coordinator-and-reason')
    # A queue-level refusal keeps the reference in its result; evidence records copy it.
    queue=board.Queue(out/'reference-valid'/'queue')
    with queue.locked() as state:
        entry=next(j for j in state['jobs'] if j['id']==job['id'])
        queue.refused(state,entry,'modeled refusal')
    assert entry['result']['reference']==job['reference']
    evidence=images.load('runner_evidence',ROOT/'tools/evidence.py')
    record=evidence.queue_record(queue.root,job['id'],'cpu')
    assert record['reference']=='docs/known.md#repeat-1'
    problems=[];evidence.check_value(record,evidence.load_schema(),'record',problems);assert not problems,problems
    record=evidence.queue_record(board.Queue(out/"reference-exempt"/"queue").root,exempt['id'],'cpu')
    assert record['reference']=='none: reads hardware facts'
    problems=[];evidence.check_value(record,evidence.load_schema(),'record',problems);assert not problems,problems
    passed('reference-copied-into-evidence-record-and-schema')


def reset_release_cases(out):
    """A verified fresh instance after a watchdog reset is released; every other unresolved state stays quarantined."""
    for dirty in ('dram_dirty','npu_dirty'):
        for variant,quarantined in (('watchdog',False),('no-return',True),('missing-return',True),('fel-after-return',True)):
            result,model,owner=lifecycle(out/f'reset-{dirty}-{variant}',variant,dirty=dirty)
            assert bool(model.quarantined)==quarantined and result['quarantined']==quarantined,(dirty,variant,result)
            if variant=='watchdog':
                assert result['outcome']=='watchdog-reset' and result['dram_lost'] and result['rom_returned']
                assert result['dram_released_by_reset'] and owner.identity!=[1,1,3,4,5]
            q=board.Queue(out/f'reset-queue-{dirty}-{variant}');args=arguments(out/f'reset-{dirty}-{variant}',submit=True)
            job=q.submit(args);assert q.claim('3-1')
            q.complete(job['id'],result)
            assert bool(q.status()['boards']['3-1']['pause'])==quarantined,(dirty,variant)
            passed(f'reset-{dirty}-{variant}-'+('quarantined' if quarantined else 'released-and-unpaused'))
    # A transport failure before any return leaves the instance unknown.
    root=out/'reset-transport';root.mkdir();args=arguments(root);model=Model('normal')
    owner=board.Live(args,root,{'dram_dirty':True,'dram_lost':True},access=model,
                     process=lambda *a,**k:types.SimpleNamespace(returncode=1,stdout='',stderr='failed'),now=model.now,sleep=model.sleep)
    try:
        with owner.locked():owner.tx('version')
    except board.TransportFailure:pass
    assert model.quarantined
    passed('reset-transport-failure-quarantines-even-with-dram-lost')

    class Access:
        def __init__(self,root,identity):
            self.RECOVERY_REQUIRED=root/'recovery-required.json';self.current=identity
        def discover(self,port):return Path('/MODEL/003/009')
        def identity(self,node):return self.current
        def target(self,node):return Path('/MODEL/3-1')
    def finish(root,**fields):
        root.mkdir()
        q=board.Queue(root/'state');args=arguments(root,submit=True)
        job=q.submit(args);assert q.claim('3-1')
        result={'first_failure':None,'instances':[{'identity':[7,1,1,0,0]},{'identity':[7,2,1,0,0]}],
                'outcome':'watchdog-reset','rom_returned':True,'dram_lost':True,'quarantined':True,**fields}
        q.complete(job['id'],result)
        return q
    def attempt(name,reason=board.DRAM_UNRESOLVED,current=(7,2,1,0,0),**fields):
        root=out/('release-'+name);q=finish(root,**fields);access=Access(root,list(current))
        record=access.RECOVERY_REQUIRED.with_name('recovery-required-3-1-7-2-1-0-0.json')
        record.write_text(json.dumps({'identity':[7,2,1,0,0],'reason':reason}))
        with patch.object(board,'CACHE',root/'cache'):
            try:released=q.release('3-1','fresh instance after watchdog reset',access)
            except ValueError as error:return record,q,str(error),root
        return record,q,None,root
    record,q,error,root=attempt('accepted')
    assert error is None and not record.exists()
    assert len(list((root/'cache/trash/recovery-required-released').glob('*recovery-required-3-1-7-2-1-0-0.json')))==1
    events=[json.loads(l) for l in (q.root/'ledger.jsonl').read_text().splitlines()]
    assert [e['reason'] for e in events if e['event']=='released']==['fresh instance after watchdog reset']
    passed('release-accepts-fresh-instance-after-watchdog-reset')
    for name,kwargs in (('no-return',dict(outcome='no-return',rom_returned=False,dram_lost=False)),
                        ('transport-failure',dict(reason='FEL transaction failed: boom')),
                        ('unresolved-return',dict(outcome='host-error',dram_lost=False)),
                        ('cleanup-failure',dict(cleanup_failure='vbus')),
                        ('other-instance',dict(current=(7,3,1,0,0)))):
        record,q,error,root=attempt(name,**kwargs)
        assert error and record.exists(),(name,error)
        assert not (root/'cache/trash').exists()
        passed('release-refuses-'+name)
    try:board.Queue(out/'release-accepted/state').release('3-1','  ',Access(out,[7,2,1,0,0]))
    except ValueError:pass
    else:raise AssertionError('empty reason accepted')
    passed('release-requires-reason')


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
    ledger=[json.loads(line) for line in (queue.root/'ledger.jsonl').read_text().splitlines()]
    assert len([e for e in ledger if e['event']=='entry'])==3
    assert len([e for e in ledger if e['event']=='result'])==3
    passed('queue-order-exclusion-no-return-pause-resume-ledger')
    for rule in ('dram','quarantine'):
        q=board.Queue(root/rule)
        for n in range(2):
            args.image.write_bytes(f'MODEL {rule} {n}'.encode())
            args.hypothesis='same hypothesis'
            job=q.submit(args);assert q.claim('3-1')
            result={'outcome':'host-error','stage':'dram' if rule=='dram' else 'capture',
                    'first_failure':{'site':'0x41012340'},'quarantined':rule=='quarantine'}
            q.complete(job['id'],result)
            if rule=='quarantine':break
        assert q.status()['boards']['3-1']['pause'],rule
        passed('queue-pause-'+rule)
    q=board.Queue(root/'freeze');args.hypothesis='freeze input';job=q.submit(args)
    Path(job['image']).write_bytes(b'mutated')
    assert q.claim('3-1') is None
    assert json.loads(Path(job['result_path']).read_text())['first_failure']['detail'].startswith('Queued input bytes changed')
    passed('queued-image-changes-refused')
    q=board.Queue(root/'any');args.port='any';args.hypothesis='any board';job=q.submit(args)
    assert q.claim('3-3')['id']==job['id'] and q.claim('3-1') is None
    passed('any-port-is-assigned-once')


def queue_integrity_cases(out):
    """A queue claim checks the frozen image and input bytes before launch; the runner uses the host code the daemon started with."""
    for target in ('image', 'host-helper'):
        for mutation in ('unchanged', 'changed', 'missing'):
            root=out/f'integrity-{target}-{mutation}';root.mkdir()
            args=arguments(root,submit=True)
            args.fel=root/'model-fel';args.fel.write_bytes(b'MODEL ONLY - never execute')
            queue=board.Queue(root/'state')
            job=queue.submit(args)
            path=Path(job['image']) if target=='image' else args.fel
            if mutation=='changed':
                path.write_bytes(b'changed after submission')
            elif mutation=='missing':
                path.rename(root/'saved-input')
            claimed=queue.claim('3-1')
            if mutation=='unchanged' or target=='host-helper':
                # A host helper that changed after submission is not frozen: the run uses the code the daemon started with.
                assert claimed and claimed['id']==job['id']
                continue
            assert claimed is None
            result=json.loads(Path(job['result_path']).read_text())
            assert result['outcome']=='refused-before-launch' and not result['launch_attempted']
            assert result['first_failure']['site']=='queue'
            assert result['first_failure']['detail'].startswith('Queued input bytes changed')
            assert not queue.status()['boards']['3-1']['running']
        passed('queued-'+target+'-integrity-before-launch')


def policy_cases(out):
    root=out/'policy';root.mkdir();args=arguments(root)
    info={'generic':False,'descriptor':{}}
    args.storage_write=True
    try:images.policy(info,args)
    except ValueError as e:assert 'Storage write requires descriptor storage_bounds' in str(e) or 'port 3-1' in str(e),str(e)
    else:raise AssertionError('storage write accepted')
    passed('unapproved-storage-write-refused')
    args.storage_write=False
    # Classification is default-deny (tools/storage_bounds.py): the raw stand-in counts as write-capable,
    # so the operating-point case uses an ELF with no SD core.
    elf=root/'no-core.o';src=root/'no-core.c';src.write_text('int stand_in(void) { return 1; }\n')
    subprocess.run(['/usr/bin/clang-21','--target=aarch64-none-elf','-c','-O1',str(src),'-o',str(elf)],check=True)
    args.image=elf
    info['descriptor']={'operating_points':[{'block':'gpu','mhz':1200,'mv':960}]}
    try:images.policy(info,args)
    except ValueError as e:assert 'requires --approved-by coordinator' in str(e)
    else:raise AssertionError('unreviewed point accepted')
    args.approved_by='coordinator';images.policy(info,args)
    passed('unreviewed-operating-point-requires-coordinator')


def withdraw_cases(out):
    root=out/'withdraw';root.mkdir();queue=board.Queue(root/'state')
    args=arguments(root,submit=True)
    jobs=[]
    for n in range(3):
        args.hypothesis=f'withdraw input {n}'
        args.image.write_bytes(f'MODEL withdraw {n}'.encode())
        jobs.append(queue.submit(args))
    running=queue.claim('3-1');assert running['id']==jobs[0]['id']
    def cli(*extra):
        err=io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(err):
            try:
                return board.main(['--queue',str(queue.root),'withdraw',*extra]),err.getvalue()
            except ValueError as error:
                return 2,str(error)
    # Only a queued entry can be withdrawn, and the reason is required.
    code,message=cli(running['id'],'--reason','not allowed');assert code==2 and 'only a queued entry' in message
    code,message=cli(jobs[1]['id'],'--reason','   ');assert code==2 and 'reason' in message
    code,message=cli('not-an-id','--reason','x');assert code==2 and 'Invalid entry ID' in message
    code,message=cli('1-deadbeef','--reason','x');assert code==2 and 'No queue entry' in message
    with queue.locked() as state:
        states={j['id']:j['state'] for j in state['jobs']}
    assert states=={jobs[0]['id']:'running',jobs[1]['id']:'queued',jobs[2]['id']:'queued'}
    code,message=cli(jobs[1]['id'],'--reason','superseded by a newer image');assert code==0
    result=json.loads(Path(jobs[1]['result_path']).read_text())
    assert result['outcome']=='refused-before-launch' and result['launch_attempted'] is False
    assert 'superseded by a newer image' in result['first_failure']['detail']
    # The daemon skips the withdrawn entry, a second withdrawal is refused, and no pause results.
    queue.complete(running['id'],{'outcome':'returned','first_failure':None})
    third=queue.claim('3-1');assert third['id']==jobs[2]['id']
    code,message=cli(jobs[1]['id'],'--reason','again');assert code==2 and 'is done' in message
    assert queue.status()['boards']['3-1']['pause'] is None
    ledger=[json.loads(line) for line in (queue.root/'ledger.jsonl').read_text().splitlines()]
    withdrawn=[e for e in ledger if e['event']=='withdrawn']
    assert len(withdrawn)==1 and withdrawn[0]['job']==jobs[1]['id'] and withdrawn[0]['reason']=='superseded by a newer image'
    assert any(e['event']=='result' and e['job']==jobs[1]['id'] for e in ledger)
    passed('withdraw-refuses-only-a-queued-entry-with-reason-and-ledger')


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
    assert not state['jobs'] and state['history'],state
    finished=[json.loads((Path(j['directory'])/'entry.json').read_text()) for j in state['history']]
    assert all(j['state']=='done' and j['result']['outcome']=='refused-before-launch' for j in finished)
    assert all(not j['result'].get('launch_attempted') for j in finished)
    assert all(j['outcome']=='refused-before-launch' for j in state['history'])
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
    assert plan['host_admission'].get('admitted') is True, plan['host_admission']      # the image's own EL3 admission ran on these bytes
    assert not (root/'hello/assets').exists()
    assert (root/'hello/launch.bin').stat().st_size<=0xc00
    passed('real-hello-typed-session-inputs-and-armed-launcher')
    # The legacy GPU ladder case is retired: it prepared an image built outside this tree and needed that build's host
    # tree (the removed gpu-bisect-1031-crumbs worktree), so it failed wherever the tree was absent. The generic GPU
    # return path is covered by the gpu-capture group, which takes real generic GPU ELFs with --gpu-image.



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
    return_host=images.load('generic_capture_return_host',ROOT/'apps/os/cpu-return-host.py')
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
            args=arguments(root);args.image=image;args.kind='gpu-return'
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
                    plan={'epoch':epoch,'generation':generation,'watchdog':{'mode':0xb1,'session_seconds':1},
                            'uploads':[],'launch':['exe','0x52000'],'format':'generic','geometry':geo,'return_host':return_host,
                            'image_ram':return_host.IMAGE_RAM,'input_regions':[]}
                    plan['preserve_regions']=images.preservation_regions(plan)
                    return plan
                def read(self,address,size,path):
                    data=super().read(address,size,path)
                    if variant=='short-gpu' and address==gpu['address']:
                        data=data[:-1];path.write_bytes(data)
                    return data
            with patch.object(board.os,'access',return_value=True),contextlib.redirect_stdout(io.StringIO()):
                result=board.run_image(args,Backend)
            invalid=variant != 'normal'
            assert result['outcome']==('failure-return' if invalid else 'returned'),(variant,result)
            report=result['return_report']
            assert report['secondaries_off']==7 and 'qualification_passed' not in report
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
                assert 'qualification_passed' not in report['thermal']
            if variant=='normal':
                assert report['gpu_capture_status']=='decoded' and 'qualification_passed' not in report['gpu']
                if diagnostic:assert report['gpu']['jobs']==0 and not report['gpu']['completed_gpu_work']
                else:assert report['gpu']['jobs']==7 and report['gpu']['completed_gpu_work']
            passed(f'generic-gpu-{index}-{variant}-preserves-native-return-and-raw-captures')


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
        return_host=host
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
        except ValueError as error:assert 'NPU kind requires an application that declares the NPU' in str(error),str(error)
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
            args=arguments(control);args.image=hello;args.kind='cpu-return'
            model=Model('normal')
            output=struct.pack('<7Q',0x31544c5553455241,1,56+limit,2 if foreign else 1,2,1,8)+b'complete'+bytes(limit-8)
            for address,data in [(geo['report_address'],copied_return_report(1)),(geo['output_address'],output)]:
                model.memory.update({address+n:b for n,b in enumerate(data)})
            class Backend(board.Live):
                rail_phases=()
                def __init__(self,*values):
                    super().__init__(*values,access=model,process=model.process,now=model.now,sleep=model.sleep)
                def dram(self):pass
                def rails(self,phase='rails'):self.rail_phases+=(phase,)
                def live_state(self):
                    # After hello-compute's return the board reads the boot PLL and mux words: its CPU clock owner lowers
                    # CPU-B to 1014 MHz and the ROM return resets CPU-L and the DSU (apps/os/NORMAL-RETURN.md). The board
                    # run 1793763471773768349-5554bd1a, with the control template's grant at the reviewed points, read
                    # them before its rail restoration.
                    return {'words':{hex(0x8871000+0x1000*d+o):v for d in range(3)
                                     for o,v in ((0,board.state.BOOT_PLL[d]),(0x1c,board.state.BOOT_MUX[d]))}}
                def prepare(self,info):
                    # The control template's CPU grant (both clusters, four changes) admits a transition.
                    plan={'epoch':1,'generation':2,'watchdog':{'mode':0xb1,'session_seconds':1},
                            'uploads':[],'launch':['exe','0x52000'],'format':'generic','geometry':geo,'return_host':return_host,
                            'image_ram':return_host.IMAGE_RAM,'input_regions':[],'cpu_grant_raises':True}
                    plan['preserve_regions']=images.preservation_regions(plan)
                    return plan
            owners=[]
            class Recorded(Backend):
                def __init__(self,*values):
                    super().__init__(*values);owners.append(self)
            with patch.object(board.os,'access',return_value=True),contextlib.redirect_stdout(io.StringIO()):
                result=board.run_image(args,Recorded)
            assert result['outcome']==('failure-return' if foreign else 'returned'),result
            # The rails are restored after the return whether or not the application's result is a success.
            assert result['rail_restore_choice']=='grant admits a transition',result
            assert owners[0].rail_phases==('rails','restore-rails') and result['rails_restored'] is True,owners[0].rail_phases
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
    # (seconds from the arm to the board's return and enumeration, seconds more before the host notices and talks to it).
    # A loaded host delays the second figure only; the countdown runs on the board, so only the first decides.
    timing={'normal':(6,0),'expired':(20,0),'boundary':(14,0),'delayed-disarm':(6,0),'suspend':(6,0),'loaded-host':(2,15)}
    for variant in timing:
        expired=variant in ('expired','boundary','suspend')
        root=out/('rail-'+variant);root.mkdir();args=arguments(root)
        args.rail_helper=root/'pinned-model.bin';args.rail_helper.write_bytes(b'MODEL PINNED HELPER')
        raw=bytearray(16384)
        encoded=log.encode();struct.pack_into('<4Q',raw,0,0x534b4552,5,0,len(encoded));raw[256:256+len(encoded)]=encoded
        class RailModel(Model):
            suspended=0
            def discover(self,port):
                if self.pending:
                    returned,noticed=timing[variant]
                    self.created=self.clock+returned+(20 if variant=='suspend' else 0)       # the kernel's node creation, in the wall clock
                    self.clock+=returned+noticed
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
        model.created=None
        model.registers[0x07090160]=0x10008002
        owner=board.Live(args,root,{},access=model,process=model.process,now=model.now,sleep=model.sleep,
                         elapsed_now=lambda:model.clock+model.suspended,wall_now=lambda:model.clock+model.suspended,
                         instance_created=lambda node:model.created)
        with patch.object(board.state,'RAIL_HELPER_SHA256',images.sha(args.rail_helper)),patch.object(board.os,'access',return_value=True),owner.locked():
            try:owner.rails()
            except ValueError as error:assert expired and 'watchdog' in str(error)
            else:assert not expired
        assert not owner.armed and bool(model.quarantined)==expired
        if expired:assert not (root/'rails/rail-result.bin').exists(),'Never read DRAM after possible WDT expiry.'
        else:assert owner.record['rails']['dcdc5_uv']==800000
        passed('pinned-rail-helper-'+variant)


def generic_npu_case(out, hello, image):
    """A generic image whose application declares the NPU (ss_application_npu), such as the gate image ret-npu-load."""
    import gate_session
    root=out/'generic-npu';root.mkdir()
    source=json.loads((board.state.EVIDENCE/'sol-resume-20261031/port-3-3/cpu-thermal-state/session.json').read_text())
    words={}
    for transaction in source['transactions']:
        command=transaction['command']
        addresses=[int(command[i+1],16) for i,value in enumerate(command) if value=='readl']
        words.update({hex(address):int(value,16) for address,value in zip(addresses,transaction['stdout'].split())})
    for address,mask in images.NPU_ENTRY_GATES.items():words[hex(address)]=words.get(hex(address),0)|mask
    for address in images.NPU_PCK4:words[hex(address)]=8
    template=gate_session.npu_template(image,root)
    # The session grants the NPU exactly when the application declares it. A typed NPU record for an application that
    # declares no NPU is refused, and so is the control template, which has no NPU record, for one that declares it.
    args=arguments(root,'--kind','cpu-return','--template',str(template));args.image=hello
    try:images.prepare(args,images.inspect(hello,'cpu-return'),{'words':words},root/'undeclared-bundle')
    except ValueError as error:assert 'the record is present and the application does not' in str(error),str(error)
    else:raise AssertionError('an NPU record was prepared for an application that declares no NPU')
    args=arguments(root,'--kind','npu');args.image=image
    info=images.inspect(image,'npu')
    assert info['generic'] and not info.get('finite_npu') and 'npu_stop' in info['report_geometry']
    try:images.prepare(args,info,{'words':words},root/'control-bundle')
    except ValueError as error:assert 'the record is absent and the application declares it' in str(error),str(error)
    else:raise AssertionError('a session without an NPU record was prepared for an application that declares the NPU')
    args=arguments(root,'--kind','npu','--template',str(template));args.image=image
    plan=images.prepare(args,info,{'words':words},root/'bundle')
    assert plan['format']=='generic'
    queued=arguments(root,'--kind','npu','--template',str(root/'bundle'),submit=True);queued.image=image
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


def finite_npu_cases(out, image_root, state, fixture, host_tree=None, cases=()):
    live = json.loads(state.read_bytes())
    template = out/'npu-template'
    template.mkdir()
    (template/'session.bin').write_bytes((image_root/'session-template.bin').read_bytes())
    for name in ('local-restart','model-switch','sdk-access','power-cycle'):
        if cases and name not in cases:
            continue
        root = out/('npu-'+name)
        root.mkdir()
        image = root/'image.elf'
        image.write_bytes((image_root/name/'image.elf').read_bytes())
        (root/'manifest.json').write_bytes((image_root/name/'manifest.json').read_bytes())
        args = arguments(root,'--kind','npu','--template',str(template))
        args.image = image
        if name=='power-cycle':
            # The current runner must also refuse a frozen parser that still
            # recognizes and accepts the old POWER geometry. No backend may
            # be constructed, including when --host-tree selects that parser.
            def no_backend(*values,**keywords):raise AssertionError('board backend created')
            assert host_tree is not None, 'Supply --npu-host-tree with the prior POWER-capable parser'
            frozen_finite=images.load('power_capable_irq',host_tree/'apps/npu-interrupt/host.py').finite
            records=images.load('power_capable_records',ROOT/'apps/os/host.py')
            old_facts=images.finite_npu_info(image,records,frozen_finite,
                json.loads((root/'manifest.json').read_bytes()))
            assert old_facts['execution_profile']==frozen_finite.POWER
            for number,tree in enumerate((None,host_tree)):
                args.host_tree=tree
                args.evidence=root/('refused-'+str(number))
                result=board.run_image(args,backend=no_backend)
                assert result['outcome']=='refused-before-launch' and not result['launch_attempted']
                assert 'physical NPU OFF' in result['first_failure']['detail'] or \
                    'supported finite NPU profile' in result['first_failure']['detail']
            passed('finite-npu-physical-off-refused-before-backend-including-frozen-host')
            continue
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
            assert not (Path(job['directory'])/'references.json').exists()
        passed('finite-npu-frozen-preparation-'+name)


def preflight_cases(out):
    """An unavailable host pre-flight refuses; only a recorded --no-preflight reason lets it through."""
    root=out/'preflight';root.mkdir()
    typed={'generic':True,'symbols':{'ss_application_npu':(0,32)}}
    cache=root/'cache';cache.mkdir()           # no capture and no recorded words here, as on a machine without the evidence cache
    on={'status':'ON','words':{hex(a):m for a,m in images.NPU_ENTRY_GATES.items()}|{hex(a):8 for a in images.NPU_PCK4}}
    def prepare(args,info,live,out_dir,**kw):
        images.validate_npu_on(live['words'])
        return {'host_admission':{'admitted':True}}
    def count(queue):
        with queue.locked() as state:return len(state['jobs'])+len(state['history'])
    def submit(name,*extra):
        (root/name).mkdir(exist_ok=True);queue=board.Queue(root/name/'state');args=arguments(root/name,*extra,submit=True);args.hypothesis=name
        return queue,args
    def refuses(queue,args):
        before=count(queue)
        try:queue.submit(args)
        except board.PreflightUnavailable as error:
            assert 'Pre-flight cannot run' in str(error) and '--no-preflight' in str(error),str(error)
        else:raise AssertionError('an unavailable pre-flight queued the image')
        assert count(queue)==before,'a refused submission was queued'
        assert not [d for d in (queue.root/'entries').glob('*') if not (d/'entry.json').exists()],'a refusal left a half-built entry'
    with patch.object(images,'CACHE',cache),patch.object(board,'CACHE',cache),patch.object(images,'inspect',return_value=typed),\
         patch.object(images,'prepare',side_effect=prepare),patch.object(board,'port_idle',return_value=True):
        # An image with a typed session and no recorded board words: the pre-flight cannot run, and nothing is queued.
        queue,args=submit('unavailable');assert images.offline_words() is None
        refuses(queue,args)
        passed('preflight-unavailable-refuses-submit')
        # The old path (warn and continue) must fail this check.
        with patch.object(board,'apply_override',lambda a,record:record):
            try:refuses(queue,args)
            except AssertionError:pass
            else:raise AssertionError('restoring warn-and-continue did not fail the check')
        passed('preflight-warn-and-continue-mutation-fails-the-check')
        # An explicit override queues, records its reason in the entry, and carries it into the result.
        queue,args=submit('override','--no-preflight','the recorded words are being rebuilt');job=queue.submit(args)
        assert job['preflight']['override']=='the recorded words are being rebuilt' and job['preflight']['unavailable'],job['preflight']
        assert json.loads((Path(job['directory'])/'entry.json').read_text())['preflight']['override']
        assert queue.claim('3-1');queue.complete(job['id'],{'outcome':'returned','first_failure':None})
        result=json.loads(Path(job['result_path']).read_text())
        assert result['preflight']['override']=='the recorded words are being rebuilt',result
        queue,args=submit('override-empty','--no-preflight','  ')
        try:queue.submit(args)
        except board.PreflightUnavailable as error:assert 'needs a reason' in str(error)
        else:raise AssertionError('an empty override reason was accepted')
        passed('preflight-override-needs-and-records-a-reason')
        # check: a typed session with no host admission refuses; an override records.
        plan={'host_admission':{'admitted':None,'unavailable':'no cached harness'}}
        (root/'check').mkdir(exist_ok=True);probe_args=arguments(root/'check')
        try:board.check_preflight(probe_args,typed,plan)
        except board.PreflightUnavailable:pass
        else:raise AssertionError('check accepted an unavailable pre-flight')
        probe_args.no_preflight='harness is built by the gate';assert board.check_preflight(probe_args,typed,plan)['override']
        passed('preflight-unavailable-refuses-check')
        # A run that read the NPU entry state refreshes the recorded words; the next submission pre-flights.
        before=images.offline_words();assert before is None
        queue,args=submit('refresh');refuses(queue,args)
        donor,donor_args=submit('donor','--no-preflight','seed');donor_job=donor.submit(donor_args);assert donor.claim('3-1')
        donor.complete(donor_job['id'],{'outcome':'returned','npu_on_admission':dict(on,status='refused'),'sid':'a'})
        assert images.offline_words() is None,'a refused NPU entry state refreshed the words'
        queue,args=submit('refresh');refuses(queue,args)
        donor2,donor2_args=submit('donor2','--no-preflight','seed');donor2_job=donor2.submit(donor2_args);assert donor2.claim('3-1')
        donor2.complete(donor2_job['id'],{'outcome':'watchdog-reset','npu_on_admission':on,'sid':'b'})
        assert images.offline_words()==on['words']
        queue,args=submit('refreshed');job=queue.submit(args)
        assert job['preflight']=={'admitted':True},job['preflight']
        passed('preflight-passes-with-words-refreshed-from-a-run')
    # A probe image is not a typed-session image: its entry says so, and it queues.
    root2=root/'probe';root2.mkdir()
    queue=board.Queue(root2/'state');args=arguments(root2,submit=True)
    job=queue.submit(args)
    assert job['preflight'].get('preflight')=='not applicable' and not job['preflight'].get('unavailable'),job['preflight']
    with patch.object(images,'inspect',return_value={'generic':False}):
        args.hypothesis='probe not typed';job=queue.submit(args)
    assert job['preflight']['preflight']=='not applicable'
    passed('preflight-probe-records-not-applicable')


def removed_cases(out):
    """The hypothesis is the prediction: no expectation option, file or outcome, and no host-code refusal."""
    for option in ('--expect','--expect-result'):
        parser=argparse.ArgumentParser(exit_on_error=False)
        board.run_options(parser,True)
        try:
            with contextlib.redirect_stderr(io.StringIO()):parser.parse_args(['image.bin','--port','3-1',option,'x'])
        except (SystemExit,argparse.ArgumentError):pass
        else:raise AssertionError(option+' is still accepted')
    assert 'result-mismatch' not in board.OUTCOMES
    assert not any(hasattr(board,name) for name in ('expectation','matches_expectation','validate_expectation','references','verify_references'))
    result,model,owner=lifecycle(out/'recorded-decoder-failure','decoder-failure',restore=True)
    assert result['outcome']=='returned' and result['decoded']=={'failed':True} and result['decoder_failures']==['native job failed'],result
    assert not (Path(result['evidence_path'])/'expect.json').exists()
    assert result['git_revision']
    passed('no-expectation-option-file-or-outcome')


def daemon_code_cases(out):
    """A check or submission whose session files differ from the daemon's checkout is refused, naming the files."""
    import shutil, subprocess
    root=out/'daemon-code';root.mkdir()
    daemon=root/'daemon'
    for name in board.session_files():
        (daemon/name).parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(board.ROOT/name,daemon/name)
    env={**os.environ,'GIT_AUTHOR_NAME':'c','GIT_AUTHOR_EMAIL':'c@e.invalid','GIT_COMMITTER_NAME':'c','GIT_COMMITTER_EMAIL':'c@e.invalid',
         'GIT_CONFIG_GLOBAL':os.devnull,'GIT_CONFIG_SYSTEM':os.devnull}
    def git(*a):return subprocess.run(['git','-C',str(daemon),*a],check=True,capture_output=True,text=True,env=env).stdout.strip()
    # The daemon started from a revision whose session files are older; the checkout has advanced since, and its HEAD now holds
    # exactly the files of the tree under test. A comparison with HEAD would pass; the daemon still runs the older revision.
    older=['tools/board_images.py','apps/os/host.py']
    saved={name:(daemon/name).read_bytes() for name in older}
    for name in older:(daemon/name).write_text('# an older copy\n')
    git('init','-q');git('add','-A');git('commit','-qm','older session files');started=git('rev-parse','HEAD')
    for name in older:(daemon/name).write_bytes(saved[name])
    git('commit','-qam','the checkout advanced');advanced=git('rev-parse','HEAD');assert advanced!=started
    queue=board.Queue(root/'state');record=queue.root/'daemon.json'
    args=arguments(root,submit=True);args.queue=queue.root
    with patch.object(images,'inspect',return_value={'generic':False}),patch.object(board,'port_idle',return_value=True):
        record.write_text(json.dumps({'root':str(daemon),'started_revision':advanced}))
        queue.submit(args);board.require_daemon_code(queue.root)
        passed('daemon-code-identical-files-pass-submit-and-check')
        record.write_text(json.dumps({'root':str(daemon),'started_revision':started}))
        for action in (lambda:queue.submit(args),lambda:board.check_image(args)):
            try:action()
            except ValueError as error:
                message=str(error)
                assert started[:12] in message and 'apps/os/host.py, tools/board_images.py;' in message,message
                assert message.endswith('land them and restart the daemon first'),message
            else:raise AssertionError('a pre-flight on files the daemon does not run was accepted')
        assert len(list((queue.root/'entries').glob('*/entry.json')))==1,'a refused submission was queued'
        passed('daemon-code-started-revision-not-head-decides-submit-and-check')
        record.write_text(json.dumps({'root':str(daemon)}))
        try:board.require_daemon_code(queue.root)
        except ValueError as error:assert 'no started_revision' in str(error),str(error)
        else:raise AssertionError('a daemon record without its started revision was accepted')
        passed('daemon-code-record-without-started-revision-refused')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--hello',type=Path)
    parser.add_argument('--resident-image',type=Path,help='Bounded stop-on-finish OS ELF for resident native admission.')
    parser.add_argument('--resident-template',type=Path,help='Typed session template directory for --resident-image.')
    parser.add_argument('--card-recovery-image',type=Path,help='Actual RAM-only first-stage RTC recovery ELF.')
    parser.add_argument('--gpu-image',type=Path,action='append',help='Real generic GPU ELF for gpu-capture group; repeat for diagnostic and ordinary images.')
    parser.add_argument('--npu-image',type=Path,help='Generic ELF whose application declares the NPU, for the generic-npu group (gate image ret-npu-load)')
    parser.add_argument('--npu-images',type=Path)
    parser.add_argument('--npu-state',type=Path)
    parser.add_argument('--npu-fixture',type=Path)
    parser.add_argument('--npu-host-tree',type=Path,help='Frozen host tree whose NPU parser still admits POWER')
    parser.add_argument('--npu-case',action='append',default=[],
                        choices=('local-restart','model-switch','sdk-access','power-cycle'),
                        help='Run only selected finite NPU cases; repeat to resume unfinished cases.')
    parser.add_argument('--only',default='transport,resident,card-recovery,queue,queue-integrity,policy,preflight,daemon,artifacts,reports,ddr,rails,offline',help='Comma-separated groups to resume.')
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    groups=args.only.split(',')
    if 'transport' in groups:transport_cases(args.out);removed_cases(args.out);automatic_restore_cases(args.out)
    if 'resident' in groups:resident_cases(args.out);resident_launcher_cases(args.out)
    if 'card-recovery' in groups:card_recovery_cases(args.out,args.card_recovery_image)
    if 'resident' in groups and args.resident_image:
        assert args.resident_template,'--resident-image needs --resident-template'
        resident_image_case(args.out,args.resident_image,args.resident_template)
    if 'queue' in groups:
        with patch.object(board,'port_idle',return_value=True):queue_cases(args.out)
        with patch.object(board,'port_idle',return_value=True):withdraw_cases(args.out)
        with patch.object(board,'port_idle',return_value=True):reset_release_cases(args.out)
    if 'transport' in groups:development_dram_cases(args.out);reference_cases(args.out)
    if 'queue-integrity' in groups:
        with patch.object(board,'port_idle',return_value=True):queue_integrity_cases(args.out)
    if 'policy' in groups:policy_cases(args.out)
    if 'preflight' in groups:preflight_cases(args.out)
    if 'preflight' in groups:daemon_code_cases(args.out)
    if 'daemon' in groups:
        with patch.object(board,'port_idle',return_value=True):daemon_cases(args.out)
    if 'artifacts' in groups:artifacts(args.out)
    if 'reports' in groups:report_cases(args.out,args.hello)
    if 'gpu-capture' in groups:gpu_capture_cases(args.out,args.gpu_image)
    if 'ddr' in groups:owned_ddr_cases(args.out)
    if 'rails' in groups:rail_cases(args.out)
    if args.hello and 'offline' in groups:offline(args.out,args.hello)
    if 'generic-npu' in groups:generic_npu_case(args.out,args.hello,args.npu_image)
    if 'npu' in groups:finite_npu_cases(args.out,args.npu_images,args.npu_state,args.npu_fixture,args.npu_host_tree,args.npu_case)
    board.save(args.out/'results.json',rows)
    print(f'{len(rows)} checks passed; no device access')


if __name__=='__main__':main()
