#!/usr/bin/env python3
"""Check generic return preservation and helper ordering without USB or an ELF build."""
import argparse
import contextlib
import hashlib
import importlib.util
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
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.tool_tree/'tools'))
    import board
    import board_images as images
    records = images.load('preservation_real_return_host', args.host_tree/'apps/os/cpu-return-host.py')
    cpu = records.cpu
    epoch, generation = 1, 2
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
           'monitor':[0x42005000,0x42010000], 'cached_writable':[0x41200000,0x41201000],
           'application_regions':[{'address':0x45000000,'bytes':4096}],
           'npu_pool':{'address':0x47400000,'bytes':0x800000},
           'npu_stop':{'address':0x41200100,'bytes':104}, 'thermal':{}}
    has_npu = hasattr(records, 'mandatory_return')
    if not has_npu:
        geo.pop('npu_pool'); geo.pop('npu_stop')
    stop = struct.pack('<13Q', 0x315255504e, 1, 104, epoch, generation, 1, 0, 0, 0, 0, 0, 1, 0)
    # Use the actual producer's public constants/layout once NPU support is present.
    if has_npu:
        try:
            records.decode_npu_stop(stop, epoch, generation)
        except ValueError:
            raise AssertionError('Update stop fixture to the native report contract')

    class Owner(board.Live):
        def __init__(self, options, evidence, record, variant):
            super().__init__(options, evidence, record)
            self.variant, self.events, self.saved = variant, [], {}
            self.memory = [(a,bytearray(bytes([i+17])*n)) for i,(a,n) in
                           enumerate(images.preservation_regions(make_plan()))]
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
        def read(self,address,size,path):
            self.events.append(('read', address, size))
            for a,data in self.memory:
                if a<=address and address+size<=a+len(data):
                    value=bytes(data[address-a:address-a+size]); path.write_bytes(value); return value
            raise AssertionError(('read outside fixture',hex(address),size))
        def upload(self,address,source):
            self.events.append(('write',address))
            data=Path(source).read_bytes()
            if self.variant=='restore-readback' and self.record.get('native_retirement_validated'):
                raise board.Mismatch('restoration readback differs')
            for a,values in self.memory:
                if a<=address and address+len(data)<=a+len(values):
                    values[address-a:address-a+len(data)]=data; return
            raise AssertionError(('upload outside fixture',hex(address)))
        def prepare(self,info):return make_plan()
        def arm(self,mode):self.armed=True
        def tx(self,*values):
            assert values==('exe','0x52000')
            self.saved={a:bytes(data) for a,data in self.memory}
            for a,data in self.memory:data[:]=bytes([99])*len(data)
            self.put(geo['report_address'],report)
            self.put(geo['output_address'], b'bad' if self.variant=='output-malformed' else
                     struct.pack('<7Q',0x31544c5553455241,1,64,epoch,generation,1,8)+b'complete')
            if has_npu:self.put(geo['npu_stop']['address'],stop if self.variant!='npu-late-fault' else bytes(104))
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
            words={'tag':0x5c0d0001,'last':board.crumbs.RETURN_ROM<<16,'time':0,'fault':0,'esr':0,'elr':0,'far':0}
            if self.variant=='late-fault':words['fault']=0xfa030000
            return (1 if self.variant=='watchdog' else 0),0,words

    def make_plan():
        plan={'format':'generic','geometry':geo,'return_host':records,'image_ram':(0x41000000,4096),
              'input_regions':[{'address':0x46000000,'bytes':4096}], 'uploads':[],
              'launch':['exe','0x52000'],'watchdog':{'mode':0xb1,'session_seconds':1},
              'epoch':epoch,'generation':generation}
        plan['preserve_regions']=images.preservation_regions(plan)
        return plan

    source=args.out/'model.bin';source.write_bytes(b'MODEL ONLY')
    variants=['normal','output-malformed','thermal-malformed','retirement-invalid','backup-corrupt',
              'range-omitted','restore-readback','watchdog','late-fault']
    if has_npu:variants.append('npu-late-fault')
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
        good=variant in ('normal','output-malformed','thermal-malformed')
        assert result.get('dram_restored',False)==good,(variant,result)
        assert bool(result.get('rails_restored'))==good,(variant,result)
        assert result['outcome']==('returned' if variant=='normal' else
                                  'watchdog-reset' if variant=='watchdog' else
                                  'host-error' if variant=='late-fault' else 'result-mismatch'),(variant,result)
        assert result['quarantined']==(not good),(variant,result)
        owner=owners[0]
        if not good:
            assert 'restore-rails' not in owner.events
            assert not any(e[0]=='write' for e in owner.events if isinstance(e,tuple)),variant
        else:
            assert all(bytes(data)==owner.saved[a] for a,data in owner.memory)
            assert (evidence/'output-record.bin').exists() and (evidence/'return-report.bin').exists()
            if has_npu:assert (evidence/'npu-stop.bin').read_bytes()==stop
        print('PASS',variant)
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
