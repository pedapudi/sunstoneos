#!/usr/bin/env python3
"""Count, from an executed trace, the instructions of a masked span that stall without a cache refill.

The check runs a latency model image of the owner scheduler under QEMU with a GDB
stub (one core stepped at a time, the others stopped), starts at the entry of
uxPortSetInterruptMask or its ISR variant when the caller lies in the function
under test, and steps until vPortClearInterruptMask or its ISR variant is
entered with a level above the API mask: the interval the recorder measures
(patches/freertos-latency-c.patch). Every executed instruction is decoded from
the image and classified; the effective address of each access comes from the
registers at that step. Device addresses are below 0x40000000, non-cacheable
Normal ones in the monitor interval 0x42000000-0x42200000
(docs/cpu-coherence.md); everything else in RAM is write-back cacheable.

The trace is the model's path on emulated cores. It counts instructions by class
exactly for that path; it does not time them."""
import argparse
import bisect
import collections
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

import capstone
from capstone import arm64 as A64
from elftools.elf.elffile import ELFFile

sys.dont_write_bytecode = True
from harness import CACHE

API_MASK = 0x98  # portMAX_API_PRIORITY_MASK: configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT
STEP_LIMIT = 40000


class Ended(Exception):
    pass


class Stub:
    def __init__(self, port):
        for _ in range(100):
            try:
                self.sock = socket.create_connection(('127.0.0.1', port))
                self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit('cannot reach the QEMU GDB stub')
        self.buffer = b''
        self.command('qSupported:swbreak+')

    def read(self):
        while b'#' not in self.buffer or len(self.buffer.split(b'#', 1)[1]) < 2:
            data = self.sock.recv(65536)
            if not data:
                raise SystemExit('the GDB stub closed')
            self.buffer += data
        start = self.buffer.index(b'$')
        end = self.buffer.index(b'#', start)
        packet = self.buffer[start + 1:end].decode()
        self.buffer = self.buffer[end + 3:]
        self.sock.sendall(b'+')
        return packet

    def command(self, text):
        checksum = sum(text.encode()) & 255
        self.sock.sendall(b'$' + text.encode() + b'#' + b'%02x' % checksum)
        while True:
            while not self.buffer:
                self.buffer += self.sock.recv(65536)
            if self.buffer[:1] == b'+':
                self.buffer = self.buffer[1:]
                break
            self.buffer = self.buffer[1:]
        return self.read()

    def stop(self, text):
        reply = self.command(text)
        while not reply.startswith('T') and not reply.startswith('S'):
            if reply.startswith('W') or reply.startswith('X'):
                raise Ended(reply)
            reply = self.read()
        match = re.search(r'thread:([0-9a-f]+)', reply)
        return int(match[1], 16) if match else 1

    def registers(self, thread):
        self.command('Hg%x' % thread)
        data = bytes.fromhex(self.command('g'))
        values = [int.from_bytes(data[i * 8:i * 8 + 8], 'little') for i in range(33)]
        return values  # x0..x30, sp, pc


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.segments = []
        with open(path, 'rb') as stream:
            elf = ELFFile(stream)
            for segment in elf.iter_segments():
                if segment['p_type'] == 'PT_LOAD' and segment['p_filesz']:
                    self.segments.append((segment['p_vaddr'], segment.data()))
            symbols = elf.get_section_by_name('.symtab')
            table = []
            for symbol in symbols.iter_symbols():
                if symbol['st_info']['type'] == 'STT_FUNC' and symbol['st_size']:
                    table.append((symbol['st_value'], symbol['st_size'], symbol.name))
        table.sort()
        self.functions = table
        self.keys = [s[0] for s in table]
        self.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.cs.detail = True
        self.cache = {}

    def function(self, name):
        for start, size, symbol in self.functions:
            if symbol == name:
                return start, start + size
        raise SystemExit('symbol missing: ' + name)

    def symbol(self, address):
        i = bisect.bisect_right(self.keys, address) - 1
        return '%s+%#x' % (self.functions[i][2], address - self.functions[i][0]) if i >= 0 else hex(address)

    def decode(self, address):
        if address not in self.cache:
            for base, data in self.segments:
                if base <= address < base + len(data):
                    word = data[address - base:address - base + 4]
                    self.cache[address] = next(self.cs.disasm(word, address))
                    break
        return self.cache[address]


ATOMICS = re.compile(r'^(ld(add|clr|eor|set|smax|smin|umax|umin)|swp|cas|st(add|clr|eor|set|smax|smin|umax|umin))(a|l|al)?(b|h)?$|^casp')
EXCLUSIVE = re.compile(r'^(ld|st)a?l?x[rp](b|h)?$|^(ldaxr|ldxr|stxr|stlxr|ldaxp|stlxp|ldxp|stxp)')
ACQREL = re.compile(r'^(ldar|ldapr|stlr|ldlar|stllr)(b|h)?$')


def region(address):
    if address < 0x40000000:
        return 'device'
    if 0x42000000 <= address < 0x42200000:
        return 'noncacheable'
    return 'cacheable'


def effective(insn, regs):
    for operand in insn.operands:
        if operand.type == A64.ARM64_OP_MEM:
            def value(register):
                if register == 0:
                    return 0
                name = insn.reg_name(register)
                if name in ('sp', 'wsp'):
                    return regs[31]
                if name in ('xzr', 'wzr'):
                    return 0
                return regs[int(name[1:])] if name[0] in 'xw' else 0
            base = value(operand.mem.base)
            index = value(operand.mem.index)
            if operand.mem.index:
                shift = operand.shift.value if operand.shift.type else 0
                index = (index << shift) & (2 ** 64 - 1)
            post = re.search(r'\],\s*[#xw]', insn.op_str) is not None
            return (base + index + (0 if post else operand.mem.disp)) & (2 ** 64 - 1)
    return None


def classify(insn, regs, counts, detail):
    name = insn.mnemonic
    if name in ('dsb', 'dmb', 'isb'):
        counts[name + ' ' + insn.op_str.replace(' ', '')] += 1
    elif name in ('msr', 'mrs'):
        counts[name + ' ' + insn.op_str.split(',')[0 if name == 'msr' else 1].strip().lower()] += 1
    elif name in ('svc', 'eret', 'wfe', 'wfi', 'sev', 'sevl', 'tlbi', 'dc', 'ic'):
        counts[name] += 1
    elif ATOMICS.match(name):
        address = effective(insn, regs)
        counts['lse atomic %s' % (region(address) if address is not None else '?')] += 1
    elif EXCLUSIVE.match(name):
        address = effective(insn, regs)
        counts['exclusive %s' % (region(address) if address is not None else '?')] += 1
    elif name.startswith('ld') or name.startswith('st'):
        address = effective(insn, regs)
        kind = 'acquire/release' if ACQREL.match(name) else ('load' if name.startswith('ld') else 'store')
        if address is not None:
            where = region(address)
            if where != 'cacheable' or kind == 'acquire/release':
                counts['%s %s' % (kind, where)] += 1
                if where != 'cacheable':
                    detail.append((hex(insn.address), name, hex(address)))
    elif name in ('bl', 'blr'):
        counts['calls'] += 1
    counts['instructions'] += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    parser.add_argument('--port', type=int, default=17834)
    parser.add_argument('--qemu-args', type=Path, help='JSON list of further QEMU arguments (loader devices of the OS board)')
    parser.add_argument('--memory', default='2G')
    parser.add_argument('--core', type=int, action='append', help='1-based GDB thread numbers to accept (default: all)')
    parser.add_argument('--function', action='append', required=True,
                        help='NAME[:SKIP[:COUNT[:THREAD]]]: spans opened from NAME, after SKIP earlier ones, COUNT of them')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = Image(args.image)
    set_entries = [image.function(n)[0] for n in ('uxPortSetInterruptMask', 'uxPortSetInterruptMaskFromISR')]
    clear_entries = [image.function(n)[0] for n in ('vPortClearInterruptMask', 'vPortClearInterruptMaskFromISR')]
    targets = []
    for text in args.function:
        parts = text.split(':')
        name, skip, count = parts[0], int(parts[1]) if len(parts) > 1 else 0, int(parts[2]) if len(parts) > 2 else 3
        thread = int(parts[3]) if len(parts) > 3 else 0
        begin, end = image.function(name)
        sites = [a for a in range(begin, end, 4) if image.decode(a).mnemonic == 'bl'
                 and int(image.decode(a).op_str.lstrip('#'), 0) in set_entries]
        assert sites, 'no mask call in ' + name
        targets.append(dict(sites=sites, name=name, range=image.function(name), skip=skip, count=count, thread=thread, seen=0, spans=[]))
    command = [str(args.qemu), '-M', 'virt,gic-version=3,virtualization=off', '-cpu', 'cortex-a76', '-smp', '8',
               '-m', args.memory, '-nographic', '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
               '-accel', 'tcg,thread=single', '-icount', 'shift=0,sleep=off', '-kernel', str(args.image),
               '-gdb', 'tcp:127.0.0.1:%d' % args.port, '-S', *(json.loads(args.qemu_args.read_text()) if args.qemu_args else [])]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        stub = Stub(args.port)
        # Secondary cores give the primary one second of guest time to finish its set-up; stopping
        # it for every set-up span would exhaust that wait. Trace only from the scheduler start.
        start = image.function('xPortStartScheduler')[0]
        stub.command('Z0,%x,4' % start)
        stub.stop('vCont;c')
        stub.command('z0,%x,4' % start)
        for t in targets:
            for address in t['sites']:
                stub.command('Z0,%x,4' % address)
        reply = stub.stop('vCont;c')
        while any(len(t['spans']) < t['count'] for t in targets):
            thread = reply
            regs = stub.registers(thread)
            pc = regs[32]
            link = pc + 4
            wanted = [t for t in targets if pc in t['sites'] and len(t['spans']) < t['count']
                      and (not t['thread'] or t['thread'] == thread)]
            if not wanted:
                reply = stub.stop('vCont;c')
                continue
            # Step out of the breakpoint, recording the call from its entry.
            stub.command('z0,%x,4' % pc)
            counts, detail = collections.Counter(), []
            steps, outermost, returned = 0, None, False
            while steps < STEP_LIMIT:
                insn = image.decode(regs[32])
                classify(insn, regs, counts, detail)
                stub.stop('vCont;s:%x' % thread)
                regs = stub.registers(thread)
                steps += 1
                if not returned and regs[32] == link:
                    returned = True
                    outermost = regs[0] > API_MASK
                    if not outermost:
                        break
                if returned and regs[32] in clear_entries and regs[0] > API_MASK:
                    break
            stub.command('Z0,%x,4' % pc)
            if outermost and steps < STEP_LIMIT:
                for t in wanted[:1]:
                    t['seen'] += 1
                    if t['seen'] > t['skip']:
                        t['spans'].append(dict(thread=thread, caller=image.symbol(link), instructions=steps,
                                               counts=dict(counts), noncacheable_accesses=detail))
            reply = stub.stop('vCont;c')
    except Ended:
        print('the model ended')
    finally:
        process.kill()
    report = {t['name']: t['spans'] for t in targets}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    for name, spans in report.items():
        for span in spans:
            print('%s thread %d  %d instructions' % (name, span['thread'], span['instructions']))
            for key, value in sorted(span['counts'].items()):
                if key != 'instructions':
                    print('    %-28s %d' % (key, value))


if __name__ == '__main__':
    main()
