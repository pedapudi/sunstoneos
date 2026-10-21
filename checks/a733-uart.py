#!/usr/bin/env python3
"""Compare source register plans; run focused controller model; compile for AArch64."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--source', type=pathlib.Path, required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True, exist_ok=False)
inputs = ['include/a733_uart.h', 'drivers/uart/a733_uart.c', 'checks/a733-uart.c', 'checks/a733-uart.py']
manifest = {}
for name in inputs:
    data = (root / name).read_bytes()
    target = a.output / 'inputs' / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    manifest[name] = hashlib.sha256(data).hexdigest()
header = (a.source / 'sunxi-uart-ng.h').read_text()
assert hashlib.sha256(header.encode()).hexdigest() == '20d584972c8d32a2f48f19af15a182f371571be16dc497cbd6a12cd88747c84a'
source = (root / inputs[1]).read_text()
for name in ['RBR','THR','DLL','DLH','IER','IIR','FCR','LCR','MCR','LSR','MSR','USR','HALT']:
    ref = int(re.search(r'#define SUNXI_UART_' + name + r'\s+\((0x[0-9a-f]+)\)', header)[1], 16)
    actual = int(re.search(r'#define ' + name + r' (0x[0-9a-f]+)u', source)[1], 16)
    assert ref == actual, name
# The reference driver leaves FORCECFG set after CHANGE_UPDATE clears. Its
# DLAB/FIFO sequence produces these writes for 24MHz, 1.5Mbaud and RTS/CTS.
# Above 9600 baud the reference RX trigger is half FIFO (FCR bits 7:6 = 2).
core = (a.source / 'sunxi-uart-ng-core.c').read_bytes()
assert hashlib.sha256(core).hexdigest() == '29237c6c3497c6f1dfd7b850d7b8596030e1d7d99a728cff79436ec1fb084fda'
commands = []
def run(cmd):
    commands.append([str(x) for x in cmd])
    return subprocess.check_output(cmd, cwd=root, stderr=subprocess.STDOUT, text=True)
compiler = shutil.which('clang-21')
assert compiler
exe = a.output / 'uart-model'
log = run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', '-Iinclude', inputs[1], inputs[2], '-o', str(exe)])
log += run([str(exe)])
log += run([str(exe), '--drain'])
log += run([str(exe), '--capacity'])
trace = run([str(exe), '--trace'])
rxtrg = re.search(r'if \(baud <= 9600\)\s*uart_port->reg\.fcr = SUNXI_UART_FCR_RXTRG_1CH[^;]*;\s*else\s*uart_port->reg\.fcr = SUNXI_UART_FCR_RXTRG_1_2\b', core.decode())
assert rxtrg and int(re.search(r'#define SUNXI_UART_FCR_RXTRG_1_2\s+\((\d+) << 6\)', header)[1]) << 6 == 0x80
expected = [(4,0),(8,0x81),(0xa4,3),(0xc,0x83),(0,1),(4,0),(0xa4,7),
            (0xc,3),(0xa4,2),(8,0x87),(8,0x81),(0x10,0x22),(4,13)]
assert [tuple(int(v,16) for v in line.split()) for line in trace.splitlines()] == expected
obj = a.output / 'a733_uart.o'
log += run([compiler, '--target=aarch64-none-elf', '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin', '-O2', '-Iinclude', '-c', inputs[1], '-o', str(obj)])
(a.output / 'model.log').write_text(log)
(a.output / 'configuration-trace.txt').write_text(trace)
for name in ['sunxi-uart-ng.h','sunxi-uart-ng-core.c','sources.json']:
    shutil.copy2(a.source / name, a.output / name)
manifest['executables'] = {str(f.name): hashlib.sha256(f.read_bytes()).hexdigest() for f in [exe,obj]}
manifest['compiler'] = {'path': compiler, 'sha256': hashlib.sha256(pathlib.Path(compiler).read_bytes()).hexdigest()}
manifest['commands'] = commands
manifest['result'] = 'source register and full configuration write sequence comparison, ASan/UBSan controller model, native object compile passed'
(a.output / 'results.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(log, end='')
print(manifest['result'])
