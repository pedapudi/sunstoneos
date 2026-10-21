#!/usr/bin/env python3
"""Selected graph construction and copied runtime results in the actual linked image.

CPU translation/cache effects and NPU service/results are modeled. Saved SDK
outputs are injected at the unchanged completed-output boundary; this is not
native inference or physical interrupt evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

# Reuse the accepted image/layout and finite runtime instruction harness.
import importlib.util
ROOT = Path(__file__).resolve().parents[1]
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result
runtime = module('graph_runtime', ROOT/'checks/npu-runtime.py')
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC

class Machine(runtime.Machine):
    def __init__(self, image, oracles):
        self.oracles = oracles
        self.translations = 0
        super().__init__(image)
        self.hooks.update({image.address(n): n for n in
                          ('aarch64_device_translate', 'aarch64_device_cache_stride',
                           'xTaskGetSchedulerState', 'digest')})
    def code(self, cpu, at, size, unused):
        a = [self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(3)]
        name = self.hooks.get(at)
        if name == 'aarch64_device_translate':
            self.u.mem_write(a[2], struct.pack('<QII', a[0], 0 if a[0] == 0x03600000 else 255, 3))
            self.translations += 1
            self.ret(1)
        elif name == 'aarch64_device_cache_stride': self.ret(64)
        elif name == 'xTaskGetSchedulerState': self.ret(1)
        elif name == 'digest':
            assert 0 < a[1] <= 524288
            self.u.mem_write(a[2], hashlib.sha256(self.data(a[0], a[1])).digest())
            self.ret()
        elif name == 'npu_convolution_output':
            value = self.oracles[self.inputs[-1]]
            assert a[2] == len(value)
            self.u.mem_write(a[1], value)
            self.ret(1)
        elif self.get(at, 4) & 0xffffffe0 == 0xd53be000:
            self.u.reg_write(UC_ARM64_REG_X0+(self.get(at,4)&31), 24000000)
            self.u.reg_write(UC_ARM64_REG_PC, at+4)
        else: super().code(cpu, at, size, unused)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--input', type=Path, action='append', required=True)
    p.add_argument('--reference', type=Path, action='append', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--runtime-only', action='store_true', help='Reuse accepted constructor for the same image')
    a = p.parse_args()
    assert len(a.input) == len(a.reference) == 2
    inputs = [v.read_bytes() for v in a.input]
    outputs = [v.read_bytes() for v in a.reference]
    assert inputs[0] != inputs[1] and outputs[0] != outputs[1]
    assert all((reference.parent/'input_0.dat').read_bytes() == raw
               for reference,raw in zip(a.reference,inputs)), 'SDK consumed a different input'
    assert all(len(set(v)) > 1 and any(v) for v in outputs)
    image = runtime.base.Image(a.image)
    m = Machine(image, dict(zip(inputs, outputs)))
    a.output.mkdir(parents=True,exist_ok=False)
    if not a.runtime_only:
        config, instance, arena = 0x43010000, 0x43020000, 0x43200000
        fields = image.layouts['npu_convolution_config']
        def field(name, value, width=8): m.put(config+fields[name], value, width)
        model = image.address('a733_npu_application_model')
        field('model', model)
        field('model_bytes', m.get(image.address('a733_npu_application_model_bytes')))
        field('arena', arena); field('arena_bytes', 20480); field('arena_physical', arena)
        field('register_cpu', 0x03600000); field('register_physical', 0x03600000)
        field('fatal', 0x43fff000)
        bl = image.layouts['a733_npu_buffer']
        sizes = (4096,4096,524288,4096,4096,524288)
        cpus = (0x43300000,0x43301000,0x43400000,0x43302000,0x43303000,0x43500000)
        devices = (0xe0000000,0xe0002000,0xe0004000,0xe0085000,0xe0087000,0xe0089000)
        roles = (4,4,2,0,1,3)  # CONSTANT descriptors/weights, READ input
        stride = (fields['arena']-fields['buffers'])//6
        assert stride == 32
        for i in range(6):
            for name,value in dict(cpu=cpus[i],physical=cpus[i],bytes=sizes[i],device=devices[i],role=roles[i]).items():
                m.put(config+fields['buffers']+i*stride+bl[name],value,4 if name in ('device','role') else 8)
        assert m.call('npu_convolution_prepare',instance,config) == 1
        conv = image.layouts['npu_convolution']
        assert m.get(instance+conv['prepared'],4) == 1 and m.translations > 512
        assert m.get(instance+conv['input_bytes']) == len(inputs[0]) == 64
        assert m.get(instance+conv['output_bytes']) == len(outputs[0]) == 8
        # The generic assembler's saved SDK comparison fixes every byte in these
        # buffers; the application must use that same address layout and payload.
        commands = m.data(cpus[2],160)
        assert struct.unpack_from('<I',commands,132)[0] == 0x40
        assert commands.count(struct.pack('<II',0x08010e01,0x40)) == 1
        for i in (0,1,2): (a.output/f'constructed-{i}.bin').write_bytes(m.data(cpus[i],sizes[i]))
    # Fresh machine: actual retained USB/mailbox/client, completed output modeled
    # with two distinct outputs previously produced by the unchanged SDK graph.
    m = Machine(image, dict(zip(inputs,outputs)))
    seq = 0
    for rid,(data,expected) in enumerate(zip(inputs,outputs),1):
        seq += 1; m.exchange(runtime.host.packet(seq,runtime.host.SUBMIT,m.prepared,rid,5000000,data))
        for _ in range(100):
            seq += 1; frame=m.exchange(runtime.host.packet(seq,runtime.host.READ,m.prepared,rid))
            value=runtime.host.decode(frame,seq,m.prepared,rid)
            if value['state'] == runtime.host.DONE: break
        assert value['output'] == expected and value['input_sha256'] == hashlib.sha256(data).hexdigest()
        (a.output/f'frame-{rid}.bin').write_bytes(frame)
    seq += 1; frame=m.exchange(runtime.host.packet(seq,runtime.host.CLOSE,m.prepared,3,5000000))
    value=runtime.host.decode(frame,seq,m.prepared,3)
    if value['state'] != runtime.host.CLOSED:
        seq += 1; frame=m.exchange(runtime.host.packet(seq,runtime.host.READ,m.prepared,3))
        value=runtime.host.decode(frame,seq,m.prepared,3)
    assert value['state'] == runtime.host.CLOSED and value['jobs'] == 2 and value['irq_completions'] == 3
    assert m.operations == [1,2,6,4,2,6,4,5] and m.closed
    (a.output/'closed-frame.bin').write_bytes(frame)
    report = dict(result='pass',image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
                  constructor=('reused same image' if a.runtime_only else True),nn_operations=2,graph_completion_footers=1,
                  runtime_inputs=2,copied_outputs=2,healthy_close=True,
                  modeled='Translation/cache, NPU service, saved SDK numerical output, IRQ counts, scheduler and USB transport.',
                  physical_access=False,native_inference=False,
                  inputs=[dict(path=str(v.resolve()),sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for v in a.input+a.reference])
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS linked two-stage construction, two runtime inputs/copied outputs and healthy CLOSE')

if __name__ == '__main__': main()
