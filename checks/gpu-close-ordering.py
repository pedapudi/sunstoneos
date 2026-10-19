#!/usr/bin/env python3
"""Actual linked GPU client: copied results precede CLOSE and STOPPED precedes success."""
import argparse, hashlib, importlib.util, json
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_transport',ROOT/'checks/gpu-usb-ordering.py')
transport=importlib.util.module_from_spec(spec);spec.loader.exec_module(transport)
class Machine(transport.Machine):
    def __init__(self,image,outcome):
        super().__init__(image);self.outcome=outcome;self.sequence=0;self.closed=False;self.statuses=0;self.operations=[]
        self.write(self.app+self.fields['usb_observed'],1)
        self.hooks[image.address('pvr_gpu_service_collect')]='collect'
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address)
        args=[cpu.reg_read(UC_ARM64_REG_X0+n) for n in range(4)]
        if name=='pvr_gpu_service_send':
            self.operation=int.from_bytes(cpu.mem_read(args[1],4),'little');self.operations.append(self.operation)
            if self.operation==2:
                assert self.sequence==2 and not self.field('result')
                for n in range(2):
                    p=self.app+self.fields['compute_sequence']+n*4
                    assert int.from_bytes(cpu.mem_read(p,4),'little')==n+1
                self.closed=True
            if self.operation==3:self.sequence+=1
            self.write(args[3],len(self.operations),8);self.ret(1);return
        if name=='collect':
            p=args[2];fields=self.image.layouts['pvr_gpu_service_reply'];cpu.mem_write(p,bytes(fields['render_pixels']+256))
            self.write(p+fields['result'],0)
            self.write(p+fields['phase'],3)
            self.write(p+fields['compute_phase'],7)
            self.write(p+fields['compute_word_count'],32)
            self.write(p+fields['compute_sequence'],self.sequence)
            if self.closed:
                self.statuses+=1
                if self.outcome=='stopped' and self.statuses>=2:self.write(p+fields['phase'],5)
                elif self.outcome=='failed':self.write(p+fields['result'],4);self.write(p+fields['phase'],6)
                else:self.write(p+fields['phase'],4)
            self.ret(1);return
        if name=='vTaskDelay':
            if self.field('result') or (self.closed and self.outcome=='pending' and self.statuses>=2):
                self.stop='observed';cpu.emu_stop();return
            self.ret();return
        super().hook(cpu,address,size,unused)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    image=transport.layout.Image(a.image);cases=[]
    for outcome in ['stopped','pending','failed']:
        m=Machine(image,outcome);m.client();assert m.closed
        assert m.field('result')=={'stopped':1,'pending':0,'failed':2}[outcome]
        cases.append({'close_outcome':outcome,'result':m.field('result'),'operations':m.operations})
    result={'passed':True,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':cases,'physical_access':False,'scope':'Actual linked compute client with modeled service replies and scheduler; no GPU execution or arithmetic claim.'}
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS actual GPU client: copied outputs before CLOSE; STOPPED required before success; pending/failure retained')
if __name__=='__main__':main()
