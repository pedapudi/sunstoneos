#!/usr/bin/env python3
"""Check generic return preservation and helper ordering without USB or an ELF build."""
import argparse
import contextlib
import hashlib
import io
from pathlib import Path
import struct
import sys
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tool-tree', type=Path, default=ROOT)
    parser.add_argument('--host-tree', type=Path, default=ROOT)
    parser.add_argument('--rail-elf', type=Path)
    parser.add_argument('--post-rail-elf', type=Path)
    parser.add_argument('--npu-parser-tree', type=Path, help='Actual report parser checkout before source integration.')
    parser.add_argument('--sdk-report', type=Path, help='Nominal report captured from the real SDK owner host fixture.')
    parser.add_argument('--legacy-host', action='store_true', help='Explicit historical CPU-only comparison without NPU cases.')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.tool_tree/'tools'))
    import board
    import board_images as images
    records = images.load('preservation_real_return_host', args.host_tree/'apps/os/cpu-return-host.py')
    if args.npu_parser_tree:
        records.npu_capture=images.load('preservation_real_npu_parser',args.npu_parser_tree/'apps/os/npu-report.py')
    cpu = records.cpu
    epoch, generation = struct.unpack_from('<2Q',args.sdk_report.read_bytes(),16) if args.sdk_report else (1,2)
    report = bytearray(cpu.REPORT_BYTES)
    struct.pack_into('<8Q', report, 0, cpu.MAGIC, 3, len(report), epoch, 1000, 4, 0, 0)
    for core in range(8):
        offset = cpu.CPU_OFFSET + core*cpu.CPU_BYTES
        struct.pack_into('<7Q', report, offset, *([epoch]*4), epoch if core else 0, epoch if core else 0, 0)
        struct.pack_into('<Q', report, offset+104, epoch)
        struct.pack_into('<Q', report, offset+768, epoch if core else 0)
    # Small independent ranges exercise the generic mechanism without allocating
    # an image's 18 MiB reservation in the host fixture.
    geo = {'report_address':0x42006000, 'report_bytes':len(report),
           'output_address':0x41200000, 'output_bytes':64,
           'monitor':[0x42000000,0x42010000], 'cached_writable':[0x41200000,0x41201000],
           'application_regions':[{'address':0x45000000,'bytes':4096}],
           'npu_pool':{'address':0x47400000,'bytes':0x800000},
           'npu_stop':{'address':0x41200100,'bytes':104}, 'thermal':{}}
    has_npu = not args.legacy_host
    if has_npu:
        assert callable(getattr(records,'mandatory_return',None)), 'Mandatory native return validator is missing.'
        assert callable(getattr(records,'decode_npu_stop',None)), 'Mandatory native NPU stop validator is missing.'
    if not has_npu:
        geo.pop('npu_pool'); geo.pop('npu_stop')
    stop = struct.pack('<4I2Q10I', 0x5255504e, 1, 104, 1, epoch, generation,
                       0, 0, 0, 0, 0, 0, 1, 0, 0, 0xc35a3ca5) + bytes(32)
    sdk_stop=args.sdk_report.read_bytes() if args.sdk_report else struct.pack(
        '<4I2Q10I',0x5255504e,1,104,1,epoch,generation,3,2,2,0,0,0,1,8,0,0)+bytes(32)
    # Use the actual producer's public constants/layout once NPU support is present.
    if has_npu:
        try:
            records.decode_npu_stop(stop, epoch, generation)
            sdk=records.npu_capture.decode(sdk_stop,epoch,generation)
            assert sdk['mode']=='sdk' and sdk['jobs']==sdk['jobs_selected']==2 and sdk['output_bytes']==8
            assert sdk['mismatches']==sdk['pattern_bytes']==sdk['pattern']==0 and sdk['last_digest']=='00'*32
            assert records.npu_capture.classify(sdk)=='completed'
            if args.sdk_report:print('SDK producer report',str(args.sdk_report),images.sha(args.sdk_report))
        except ValueError:
            raise AssertionError('Update stop fixture to the native report contract')

    class Owner(board.Live):
        def __init__(self, options, evidence, record, variant):
            super().__init__(options, evidence, record)
            self.variant, self.events, self.saved = variant, [], {}
            self.restore_count=0
            self.helper_running=False
            self.memory = [(a,bytearray(bytes([i+17])*n)) for i,(a,n) in
                           enumerate(images.preservation_regions(make_plan()))]
            self.saved={a:bytes(data) for a,data in self.memory}
            self.quarantined = False
        @contextlib.contextmanager
        def locked(self):
            try: yield self
            finally:
                if self.record.get('dram_dirty') and not self.record.get('dram_restored'):
                    self.quarantine('unresolved DRAM')
        def quarantine(self, reason):
            self.quarantined = True; self.record['quarantined'] = True
        def journal(self):pass
        def sanity(self):pass
        def reset(self):pass
        def dram(self):pass
        def live_state(self):
            return {'words':{hex(0x8871000+0x1000*d+o):v for d in range(3)
                              for o,v in ((0,board.state.BOOT_PLL[d]),(0x1c,board.state.BOOT_MUX[d]))}}
        def rails(self, phase='rails'):
            self.events.append(phase)
            if phase=='restore-rails':
                assert self.record['dram_restored'] and self.record['native_retirement_validated']
                assert all(bytes(data)==self.saved[a] for a,data in self.memory)
                if self.variant=='npu-sdk':
                    board.Live.rails(self,phase)
                    assert self.record['rail_runs'][phase]['dram_restored']
                    assert all(bytes(data)==self.saved[a] for a,data in self.memory)
        def read(self,address,size,path):
            self.events.append(('read', address, size))
            for a,data in self.memory:
                if a<=address and address+size<=a+len(data):
                    value=bytes(data[address-a:address-a+size]); path.write_bytes(value); return value
            raise AssertionError(('read outside fixture',hex(address),size))
        def upload(self,address,source,*,expected_sha256=None):
            self.events.append(('write-restore' if self.record.get('native_retirement_validated') else 'write-upload',address))
            data=Path(source).read_bytes()
            assert expected_sha256 is None or hashlib.sha256(data).hexdigest()==expected_sha256
            if self.record.get('native_retirement_validated'):
                self.restore_count+=1
            if self.variant=='restore-readback' and self.restore_count==2:
                raise board.Mismatch('restoration readback differs')
            for a,values in self.memory:
                if a<=address and address+len(data)<=a+len(values):
                    values[address-a:address-a+len(data)]=data; return
            raise AssertionError(('upload outside fixture',hex(address)))
        def prepare(self,info):return make_plan()
        def arm(self,mode,**kwargs):self.armed=True
        def tx(self,*values):
            if values==('readl','0x07090160'):return '0x10008002'
            if values==('reset64','0x41000000'):
                self.helper_running=True
                self.put(board.POST_RAIL_HELPER_DRAM[0],bytes([109])*board.POST_RAIL_HELPER_DRAM[1])
                followup=images.load('sdk_real_post_helper_parser',args.tool_tree/'apps/os/cpu-return-followup.py')
                lines=[f'{name} {phase} pll={pll:#x} mux={mux:#x} mhz={mhz} pat0=0 pat1=0 lfm=0'
                       for name,pll,mux,mhz in board.state.BOOT_ROWS for phase in ('before','after')]
                lines += ['SUPPLY dcdc5_uv=1000000 dcdc3_uv=800000 bin=4 dcxo_hz=26000000',
                          'VOLT dcdc5_selector=30 dcdc5_uv=800000',followup.COMPLETE]
                payload=('\n'.join(lines)+'\n').encode();raw=bytearray(16384)
                struct.pack_into('<4Q',raw,0,0x534b4552,5,0,len(payload));raw[256:256+len(payload)]=payload
                self.put(0x42000000,raw);return ''
            assert values==('exe','0x52000')
            for a,data in self.memory:data[:]=bytes([99])*len(data)
            self.put(geo['report_address'],report)
            self.put(geo['output_address'], b'bad' if self.variant=='output-malformed' else
                     struct.pack('<7Q',0x31544c5553455241,1,64,epoch,generation,1,8)+b'complete')
            if has_npu:
                native_stop=bytearray(sdk_stop if self.variant=='npu-sdk' else stop)
                if self.variant=='npu-mismatch':
                    struct.pack_into('<3I',native_stop,36,1,1,1)
                    struct.pack_into('<I',native_stop,60,2)
                if self.variant=='npu-telemetry-malformed':struct.pack_into('<I',native_stop,68,1)
                if self.variant=='npu-refused':struct.pack_into('<I',native_stop,48,1)
                self.put(geo['npu_stop']['address'],native_stop if self.variant!='npu-late-fault' else bytes(104))
            if self.variant=='retirement-invalid':self.put(geo['report_address'],bytes(len(report)))
            if self.variant=='backup-corrupt':Path(self.record['dram_backups'][-1]['file']).write_bytes(b'corrupt')
            if self.variant=='range-omitted':self.record['dram_backups'].pop()
        def put(self,address,value):
            for a,data in self.memory:
                if a<=address and address+len(value)<=a+len(data):data[address-a:address-a+len(value)]=value;return
            raise AssertionError('capture outside fixture')
        def wait(self,seconds):pass
        def returned(self):
            self.armed=False
            words={'tag':0x5c0d0000|(epoch&0xffff),'last':board.crumbs.RETURN_ROM<<16,'time':0,'fault':0,'esr':0,'elr':0,'far':0}
            if self.helper_running:
                self.helper_running=False;return 0xb1,0,words
            if self.variant=='late-fault':words['fault']=0xfa030000
            return (1 if self.variant=='watchdog' else 0),0,words

    def make_plan():
        plan={'format':'generic','geometry':geo,'return_host':records,'image_ram':(0x41000000,0x50000),
              'input_regions':[{'address':0x46000000,'bytes':4096}], 'uploads':[(0x41000000,source)],
              'launch':['exe','0x52000'],'watchdog':{'mode':0xb1,'session_seconds':1},
              'epoch':epoch,'generation':generation}
        plan['preserve_regions']=images.preservation_regions(plan)
        return plan

    source=args.out/'model.bin';source.write_bytes(b'MODEL ONLY')
    variants=['normal','output-malformed','thermal-malformed','retirement-invalid','backup-corrupt',
              'range-omitted','restore-readback','watchdog','late-fault']
    if has_npu:variants.extend(('npu-sdk','npu-late-fault','npu-mismatch','npu-refused','npu-telemetry-malformed'))
    for variant in variants:
        evidence=args.out/variant
        parser=argparse.ArgumentParser();board.run_options(parser)
        options=parser.parse_args([str(source),'--port','3-1','--restore-rails'])
        options.evidence=evidence
        owners=[]
        def backend(*values):
            owner=Owner(*values,variant);owners.append(owner);return owner
        thermal=types.SimpleNamespace(captures=lambda g:[],decode=lambda *v: {'classification':'complete'})
        if variant=='thermal-malformed':thermal.decode=lambda *v:(_ for _ in ()).throw(ValueError('bad optional thermal'))
        info={'entry_guard':True,'descriptor':{},'generic':True}
        with patch.object(images,'inspect',return_value=info),patch.object(images,'policy'),\
             patch.object(board,'application_inputs'),patch.object(records,'thermal_capture',thermal),\
             patch.object(records.host,'image_manifest',return_value={'manifest':{'output_limit':8}}),\
             contextlib.redirect_stdout(io.StringIO()):
            result=board.run_image(options,backend)
        good=variant in ('normal','output-malformed','thermal-malformed','npu-mismatch','npu-refused','npu-telemetry-malformed','npu-sdk')
        assert result.get('dram_restored',False)==good,(variant,result)
        assert bool(result.get('rails_restored'))==good,(variant,result)
        assert result['outcome']==('returned' if variant in ('normal','npu-sdk') else
                                  'watchdog-reset' if variant=='watchdog' else
                                  'host-error' if variant=='late-fault' else 'result-mismatch'),(variant,result)
        assert result['quarantined']==(not good),(variant,result)
        owner=owners[0]
        if not good:
            assert 'restore-rails' not in owner.events
            if variant=='restore-readback':
                assert any(bytes(data)==owner.saved[a] for a,data in owner.memory)
                assert any(bytes(data)!=owner.saved[a] for a,data in owner.memory)
                assert all(Path(item['file']).is_file() for item in owner.record['dram_backups'])
            assert not any(e[0]=='write-restore' for e in owner.events if isinstance(e,tuple)) or variant=='restore-readback',variant
        else:
            first_write=next(i for i,e in enumerate(owner.events) if isinstance(e,tuple) and e[0]=='write-upload')
            assert all(e[0]=='read' and e[2]<=4*1024*1024 for e in owner.events[1:first_write])
            assert all(bytes(data)==owner.saved[a] for a,data in owner.memory)
            assert (evidence/'output-record.bin').exists() and (evidence/'return-report.bin').exists()
            if has_npu:assert (evidence/'npu-stop.bin').stat().st_size==104
            if variant=='npu-sdk':
                assert (evidence/'npu-stop.bin').read_bytes()==sdk_stop
                assert result['return_report']['npu_capture_status']=='decoded'
                assert result['return_report']['npu_report']['mode']=='sdk'
                assert result['rail_runs']['restore-rails']['image_sha256']==board.POST_RAIL_HELPER_SHA256
        print('PASS',variant)
    # Exercise the production pinned helper method, including its own uploads.
    if args.rail_elf:
        assert images.sha(args.rail_elf)=='ef63663020cf827f89811ef9cb60733a4965f70e518fe96ccd8821c59babfb5f'
        linked=images.symbols(args.rail_elf)
        assert linked['__heap_end'][0]==sum(board.RAIL_HELPER_DRAM)
        assert linked['__bss_end'][0]<=linked['__exception_stack_top'][0]<=linked['__monitor_stack_top'][0]<=sum(board.RAIL_HELPER_DRAM)
        assert images.sha(board.state.RAIL_HELPER)==board.state.RAIL_HELPER_SHA256
        print('PASS pinned helper ELF bounds include BSS, stacks and heap')
    if args.post_rail_elf:
        assert images.sha(args.post_rail_elf)=='ae00ce1d4a1f82b77ab1e016dc1a08f795e56c9c6f4c92d166f6434d75590098'
        linked=images.symbols(args.post_rail_elf)
        assert linked['__heap_end'][0]==sum(board.POST_RAIL_HELPER_DRAM)
        assert linked['__bss_end'][0]<=linked['__exception_stack_top'][0]<=linked['__monitor_stack_top'][0]<=sum(board.POST_RAIL_HELPER_DRAM)
        assert images.sha(board.POST_RAIL_HELPER)==board.POST_RAIL_HELPER_SHA256
        print('PASS pinned post helper ELF bounds include BSS, stacks and heap')
    for variant in ('normal','uncertain','corrupt','report-failed'):
        evidence=args.out/('helper-'+variant);evidence.mkdir()
        helper=evidence/'model-helper.bin';helper.write_bytes(b'MODEL HELPER')
        class RailOwner:
            def __init__(self):
                self.evidence=evidence;self.args=types.SimpleNamespace(rail_helper=helper)
                self.record={};self.events=[];self.clock=0
                self.memory={0x41000000:bytes([71])*board.RAIL_HELPER_DRAM[1],0x42000000:bytes([72])*16384}
                self.before=self.memory.copy()
            def journal(self):pass
            def elapsed_now(self):return self.clock
            def read(self,address,size,path):
                data=self.memory[address];assert len(data)==size;path.write_bytes(data);return data
            def upload(self,address,path,*,expected_sha256=None):
                self.events.append(address)
                data=Path(path).read_bytes()
                assert expected_sha256 is None or hashlib.sha256(data).hexdigest()==expected_sha256
                self.memory[address]=data+self.memory[address][len(data):]
            def arm(self,*values,**kwargs):pass
            def wait(self,*values):self.clock=1
            def returned(self):return (0 if variant=='uncertain' else 0xb1),0,{}
            def tx(self,*values):
                if values==('readl','0x07090160'):return '0x10008002'
                assert values==('reset64','0x41000000')
                self.memory[0x41000000]=bytes([109])*len(self.memory[0x41000000])
                raw=bytearray(16384);struct.pack_into('<4Q',raw,0,0x534b4552,5,0,0)
                self.memory[0x42000000]=bytes(raw)
                if variant=='corrupt':
                    Path(self.record['rail_dram_backups']['rails'][-1]['file']).write_bytes(b'changed')
        owner=RailOwner()
        with patch.object(board.state,'RAIL_HELPER_SHA256',images.sha(helper)),\
             patch.object(images,'rail_result',side_effect=ValueError('invalid helper receipt') if variant=='report-failed' else None,
                          return_value={'dcdc3_uv':800000,'dcdc5_uv':800000}):
            try:board.Live.rails(owner)
            except ValueError:
                assert variant!='normal'
            else:assert variant=='normal'
        assert bool(owner.record.get('dram_restored'))==(variant=='normal')
        assert len(owner.events)==(4 if variant=='normal' else 2)
        if variant=='normal':assert owner.memory==owner.before
        print('PASS helper DRAM '+variant)
    # The post-ROM profile uses the distinct reviewed DCDC5-only binary and
    # the real parser; its continuation gate is independent of CLI input paths.
    followup=images.load('preservation_helper_followup', args.tool_tree/'apps/os/cpu-return-followup.py')
    for variant in ('normal','dcdc3-raised','unretired','unrestored'):
        evidence=args.out/('post-helper-'+variant);evidence.mkdir()
        helper=evidence/'post-model.bin';helper.write_bytes(b'MODEL POST HELPER')
        lines=[f'{name} {phase} pll={pll:#x} mux={mux:#x} mhz={mhz} pat0=0 pat1=0 lfm=0'
               for name,pll,mux,mhz in board.state.BOOT_ROWS for phase in ('before','after')]
        lines += [f'SUPPLY dcdc5_uv=1000000 dcdc3_uv={1050000 if variant=="dcdc3-raised" else 800000} bin=4 dcxo_hz=26000000',
                  'VOLT dcdc5_selector=30 dcdc5_uv=800000', followup.COMPLETE]
        payload=('\n'.join(lines)+'\n').encode()
        class PostOwner(RailOwner):
            def __init__(self):
                super().__init__()
                self.record={'normal_return':True,'native_retirement_validated':variant!='unretired',
                             'dram_restored':variant!='unrestored'}
                self.memory[0x41000000]=bytes([71])*board.POST_RAIL_HELPER_DRAM[1]
                self.before=self.memory.copy()
            def tx(self,*values):
                if values==('readl','0x07090160'):return '0x10008002'
                assert values==('reset64','0x41000000')
                self.memory[0x41000000]=bytes([109])*len(self.memory[0x41000000])
                raw=bytearray(16384);struct.pack_into('<4Q',raw,0,0x534b4552,5,0,len(payload))
                raw[256:256+len(payload)]=payload;self.memory[0x42000000]=bytes(raw)
            def returned(self):return 0xb1,0,{}
        owner=PostOwner()
        with patch.object(board,'POST_RAIL_HELPER',helper),patch.object(board,'POST_RAIL_HELPER_SHA256',images.sha(helper)):
            try:board.Live.rails(owner,'restore-rails')
            except ValueError:assert variant!='normal'
            else:assert variant=='normal'
        assert len(owner.events)==(4 if variant=='normal' else 2 if variant=='dcdc3-raised' else 0)
        if variant=='normal':assert owner.memory==owner.before
        print('PASS post helper '+variant)
    evidence=args.out/'missing-post-helper'
    parser=argparse.ArgumentParser();board.run_options(parser)
    options=parser.parse_args([str(source),'--port','3-1','--restore-rails']);options.evidence=evidence
    with patch.object(images,'inspect',return_value={'entry_guard':True,'descriptor':{}}),\
         patch.object(images,'policy'),patch.object(board,'POST_RAIL_HELPER',args.out/'absent-helper'),\
         contextlib.redirect_stdout(io.StringIO()):
        result=board.run_image(options,lambda *v: (_ for _ in ()).throw(AssertionError('opened board before helper admission')))
    assert result['outcome']=='refused-before-launch' and 'post-ROM rail helper is absent' in result['first_failure']['detail']
    print('PASS missing post helper refuses before device access')
    owner=board.Live(options,args.out,{})
    with patch.object(owner,'tx',side_effect=AssertionError('uploaded a changed backup')):
        try:owner.upload(0x41000000,source,expected_sha256='0'*64)
        except ValueError as error:assert 'sealed restoration' in str(error)
        else:raise AssertionError('changed saved source reached the transport')
    print('PASS saved source hash checked before sealed upload')
    plan=make_plan();plan['preserve_regions'].pop()
    try:images.validate_uploads(plan)
    except ValueError:pass
    else:raise AssertionError('omitted protected region accepted')
    plan=make_plan();plan['uploads']=[(0x48000000,source)]
    try:images.validate_uploads(plan)
    except ValueError:pass
    else:raise AssertionError('upload outside preserved DRAM accepted')
    print('PASS omitted extent and unprotected upload refused before uploads')


if __name__=='__main__':main()
