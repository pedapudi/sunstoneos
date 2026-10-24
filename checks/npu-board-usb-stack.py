#!/usr/bin/env python3
"""Run the linked NPU USB task, TinyUSB and MUSB FIFO PIO over modeled registers.

The NPU client's ready state and tensor windows are fixtures. No inference,
provider startup, physical USB, DMA, cache or multicore behavior is qualified.
Control endpoint and endpoint-open effects are modeled; bulk FIFO transfers,
packet handlers, FIFO pointers and completion callbacks execute native code.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj
base = module('npu_usb_stack_base', ROOT / 'checks/g2d-usb-stack.py')
USB = 0x04100000

def fifo_layout(path):
    # Resolve the anonymous FIFO structure through its linked typedef.
    with path.open('rb') as stream:
        for cu in base.ELFFile(stream).get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_typedef' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value == b'tu_fifo_t':
                    typ = die.get_DIE_from_attribute('DW_AT_type')
                    return {d.attributes['DW_AT_name'].value.decode(): int(d.attributes['DW_AT_data_member_location'].value)
                            for d in typ.iter_children() if d.tag == 'DW_TAG_member'}
    raise AssertionError('linked tu_fifo_t missing')

class Machine(base.Machine):
    def __init__(self, image, types, sizes, enums, speed):
        super().__init__(image, types, sizes, enums, speed)
        self.cpu.mem_map(USB, 0x10000)
        self.regs = {}; self.bank = 0; self.rx = bytearray(); self.tx = bytearray()
        self.bulk = {}; self.completions = []; self.pio = Counter(); self.word_alignment = Counter()
        self.fifo_calls = []; self.sequence = 0
        self.fifo_layout = fifo_layout(image.path)
        self.fifo_spans = {}; self.wraps = Counter()
        for name in ('xTaskGenericNotify', 'dcd_edpt_xfer_fifo', 'tu_fifo_get_read_info', 'tu_fifo_get_write_info', 'tu_fifo_advance_read_pointer', 'tu_fifo_advance_write_pointer'):
            at = image.address(name)
            self.names[at] = name
            self.cpu.hook_add(UC_HOOK_CODE, self.hook, begin=at, end=at)
        self.cpu.hook_add(UC_HOOK_MEM_READ, self.mmio_read, begin=USB, end=USB+0xffff)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.mmio_write, begin=USB, end=USB+0xffff)
        self.client = image.address('npu_board') + image.layouts['npu_board_application']['client']
        self.cl = image.layouts['npu_board_client']; self.tl = image.layouts['npu_board_tensor']
        board = image.address('board') + image.layouts['board_application']['session']
        session = image.layouts['a733_board_session']
        self.write(board + session['epoch'], 17, 8); self.write(board + session['generation'], 3, 8)
        self.write(self.client + self.cl['task'], 0x43000100, 8)
    def key(self, off):
        return (self.bank if 0x80 <= off <= 0x87 or 0x90 <= off <= 0x99 else 0, off)
    def reg(self, bank, off):
        return self.regs.get((bank, off), 0)
    def mmio_read(self, cpu, access, at, size, value, unused):
        off = at - USB
        if off == 8:
            assert len(self.rx) >= size, (size, len(self.rx))
            data = bytes(self.rx[:size]); del self.rx[:size]
            self.cpu.mem_write(at, data); self.pio['out'] += size
        else:
            value = self.bank if off == 0x42 else self.regs.get(self.key(off), 0)
            self.write(at, value, size)
    def mmio_write(self, cpu, access, at, size, value, unused):
        off = at - USB
        if off == 4:
            self.tx.extend(value.to_bytes(size, 'little')); self.pio['in'] += size
        elif off == 0x42:
            self.bank = value
        elif off in (0x44, 0x46, 0x4c):
            self.regs[self.key(off)] = self.regs.get(self.key(off), 0) & ~value
        else:
            self.regs[self.key(off)] = value
    def hook(self, cpu, at, size, unused):
        name = self.names.get(at)
        args = [cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(5)]
        if name == 'xTaskGenericNotify':
            self.seen[name] += 1; self.ret(1); return
        if name == 'dcd_edpt_open':
            desc = bytes(cpu.mem_read(args[1], 7)); ep = desc[2]
            self.regs[(ep & 15, 0x80 if ep & 128 else 0x84)] = self.mps
            self.regs[(0, 0x48 if ep & 128 else 0x4a)] = 1 << (ep & 15)
        if name == 'dcd_edpt_xfer_fifo':
            ep, fifo, count = args[1] & 255, args[2], args[3] & 65535
            assert ep in (2, 0x81) and ep not in self.bulk
            self.bulk[ep] = (fifo, count); self.fifo_calls.append([ep, count])
            self.seen[name] += 1
            return
        if name in ('tu_fifo_get_read_info', 'tu_fifo_get_write_info'):
            direction = 'in' if name == 'tu_fifo_get_read_info' else 'out'
            f = self.fifo_layout; depth = self.word(args[0]+f['depth'], 2)
            index = self.word(args[0]+f['rd_idx' if direction=='in' else 'wr_idx'], 2) % depth
            pointer = self.word(args[0]+f['buffer'], 8)+index
            self.word_alignment[(direction, pointer % 4)] += 1
            self.fifo_spans[direction] = depth-index
            self.seen[name] += 1
            return
        if name in ('tu_fifo_advance_read_pointer', 'tu_fifo_advance_write_pointer'):
            direction = 'in' if name == 'tu_fifo_advance_read_pointer' else 'out'
            self.wraps[direction] += args[1] > self.fifo_spans[direction]
            self.seen[name] += 1
            return
        if name == 'dcd_event_handler':
            t = self.t['dcd_event_t']; ptr = args[0]
            if self.word(ptr+t['event_id'], 1) == self.enums['DCD_EVENT_XFER_COMPLETE']:
                ep = self.word(ptr+t['xfer_complete.ep_addr'], 1)
                if ep in (2, 0x81):
                    n = self.word(ptr+t['xfer_complete.len'])
                    assert ep in self.bulk
                    self.bulk.pop(ep); self.completions.append([ep, n])
        super().hook(cpu, at, size, unused)
    def enumerate(self):
        self.tick(); self.event('BUS_RESET_END', **{'bus_reset.speed': self.speed}); self.tick()
        self.event('SETUP_RECEIVED', setup_received=struct.pack('<BBHHH', 0, 9, 1, 0, 0)); self.tick()
        assert self.opened == {2: self.mps, 0x81: self.mps} and 2 in self.bulk
        self.complete(0x80); self.tick(); assert not self.failure
    def out(self, data):
        for offset in range(0, len(data), self.mps):
            packet = data[offset:offset+self.mps]
            assert 2 in self.bulk and not self.rx
            self.rx.extend(packet)
            self.regs[(0, 0x88)] = len(packet); self.regs[(2, 0x86)] = 1
            self.regs[(0, 0x46)] = 4
            self.call('dcd_int_handler', 0); assert not self.rx
            self.tick(); assert not self.failure
    def take(self):
        assert self.reg(1, 0x82) & 1
        packet = bytes(self.tx); self.tx.clear(); assert len(packet) <= self.mps
        self.regs[(1, 0x82)] &= ~1; self.regs[(0, 0x44)] = 2
        self.call('dcd_int_handler', 0); self.tick(); assert not self.failure
        self.packets.append(len(packet)); return packet
    def drain(self):
        result = bytearray()
        for _ in range(256):
            if not (self.reg(1, 0x82) & 1):
                assert 0x81 not in self.bulk
                return bytes(result)
            result.extend(self.take())
        raise AssertionError('IN packet bound')
    def packet(self, op, size=0, offset=0):
        self.sequence += 1
        return struct.pack('<4I2Q2I24x', 0x3151424e, op, self.sequence, size, 17, 3, 0, offset)
    def field(self, name, value):
        self.write(self.client+self.cl[name], value)
    def tensor(self, direction, count):
        at = self.client+self.cl[direction]
        self.write(at+self.tl['cpu'], 0x48000001, 8)
        self.write(at+self.tl['bytes'], count)


def run(image, speed):
    t, sizes, enums = base.metadata(image.path)
    m = Machine(image, t, sizes, enums, speed)
    m.enumerate()
    # Before OPEN, STATUS leaves all client fields zero.
    m.out(m.packet(1)); raw = m.drain()
    assert len(raw) == 64 and int.from_bytes(raw[12:16], 'little') == 4
    m.field('ready', 1); m.field('inputs', 1); m.field('outputs', 1)
    # Odd uploads force unaligned FIFO pointers; repeated chunks wrap RX.
    m.field('window', 1); payload = bytes(i % 251 for i in range(4133)); m.tensor('input', len(payload))
    offset = 0
    for count in (1, 3, 509, 1023, 2049, 548):
        m.out(m.packet(3, count, offset)+payload[offset:offset+count]); response = m.drain()
        assert len(response) == 64 and int.from_bytes(response[12:16], 'little') == 1
        offset += count
    assert offset == len(payload) and bytes(m.cpu.mem_read(0x48000001, len(payload))) == payload
    m.field('window', 2)
    reads = []
    # Includes catalogue limits, odd tails and exact FS/HS response multiples.
    # The sequence wraps TX and places successive RAM word starts off alignment.
    for count in (2, 2816, 1, 3, 448, 2816):
        m.tensor('output', count); data = bytes((i*17+count) % 256 for i in range(count))
        m.cpu.mem_write(0x48000001, data)
        before = len(m.completions); m.out(m.packet(5))
        # Queue RETURN before terminal completion: the window stays lent.
        old_request = m.word(m.client+m.cl['request'])
        m.out(m.packet(6))
        first = bytearray()
        while len(first) < count+64:
            first.extend(m.take())
            assert len(first) <= count+64
            if len(first) < count+64 or (count+64) % m.mps == 0:
                assert m.word(m.client+m.cl['window']) == 2
                assert m.word(m.client+m.cl['request']) == old_request
        if (count+64) % m.mps == 0:
            assert m.reg(1, 0x82) & 1 and not m.tx, 'terminal ZLP missing'
            assert m.take() == b''
        assert bytes(first[64:]) == data and int.from_bytes(first[28:32], 'little') == count
        assert m.word(m.client+m.cl['window']) == 0
        assert m.word(m.client+m.cl['request']) == (m.sequence << 8 | 3)
        # Model only the client reply after the native window handback/post.
        m.field('answer', m.sequence << 8 | 1); m.tick(); m.tick()
        second = m.drain(); assert len(second) == 64 and int.from_bytes(second[4:8], 'little') == 6
        m.field('window', 2)
        reads.append(dict(bytes=count, completions=m.completions[before:]))
    assert all(m.seen[name] for name in ('dcd_edpt_xfer_fifo','tu_fifo_get_read_info','tu_fifo_get_write_info','tu_fifo_advance_read_pointer','tu_fifo_advance_write_pointer'))
    assert any(align for (name, align), n in m.word_alignment.items() if name=='out')
    assert any(align for (name, align), n in m.word_alignment.items() if name=='in')
    assert m.wraps['in'] and m.wraps['out'], m.wraps
    return dict(wrapped_packets=dict(m.wraps), speed='high' if speed==2 else 'full', passed=True, reads=reads,
                fifo_calls=m.fifo_calls, pio_bytes=dict(m.pio), packet_lengths=m.packets,
                ram_alignments={f'{name}:{align}':n for (name,align),n in m.word_alignment.items()},
                executed=dict(m.seen))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--speed', choices=('full','high'), required=True)
    a=p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    e=base.base.Image(a.image, layout_names=('npu_board_application','npu_board_client','npu_board_tensor','board_application','a733_board_session'))
    e.path=a.image
    result=run(e, 2 if a.speed=='high' else 1)
    result.update(image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(), scope=__doc__, physical_access=False)
    (a.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NPU',a.speed,'USB FIFO PIO, odd/wrapped buffers, completion and queued request ordering')
if __name__=='__main__':main()
