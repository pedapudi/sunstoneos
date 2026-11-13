#!/usr/bin/env python3
"""Check event-driven UART/H4 service, retained faults and idle notification waits."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import host_rtos
from harness import CLANG,environment

ROOT=Path(__file__).resolve().parents[1]
GATE_RUNS=[['--output','{output}']]
CHECK_INPUTS=['checks/event-uart-h4.c','checks/event-uart-headset-deadline.c',
    'drivers/uart/a733_uart.c','include/a733_uart.h','drivers/bluetooth/bt_h4.c','include/bt_h4.h',
    'apps/bluetooth/bluetooth.c','apps/bluetooth/bluetooth.h','apps/bluetooth/headset.c','apps/bluetooth/headset.h',
    'apps/bluetooth/security-source.py','apps/bluetooth/security-source.json',
    'checks/host_rtos.py','checks/host/host_rtos.c','checks/host/host_rtos.h','checks/host/host_deadline.c',
    'runtime/deadline.c','include/sunstone_deadline.h']
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
header=Path.home()/'.cache/sunstoneos/sources/uart/sunxi-uart-ng.h'
raw=header.read_bytes();text=raw.decode()
assert hashlib.sha256(raw).hexdigest()=='20d584972c8d32a2f48f19af15a182f371571be16dc497cbd6a12cd88747c84a'
assert re.search(r'#define SUNXI_UART_USR_RFNE\s+\(BIT\(3\)\)',text)
assert re.search(r'#define SUNXI_UART_USR_BUSY\s+\(BIT\(0\)\)',text)
assert re.search(r'#define SUNXI_UART_RFL\s+\(0x84\)',text)
sources=['checks/event-uart-h4.c','drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c','runtime/deadline.c']
exe=host_rtos.build(a.output/'host',sources,'event-uart-h4')
cases=['idle','short-rx','suffix','pool','deadlines','faults','quiet-busy'];logs=[]
for case in cases:
    result=subprocess.run([str(exe),case],capture_output=True,text=True,env=environment(),timeout=20)
    assert result.returncode==0,(case,result.stdout,result.stderr)
    logs.append(result.stdout)
security=a.output/'security'
subprocess.run([str(Path.home()/'.cache/sunstoneos/venv/bin/python3'),'-B',
    str(ROOT/'apps/bluetooth/security-source.py'),'--source',
    str(Path.home()/'.cache/sunstoneos/sources/bluetooth-security-zephyr-v4.2.1'),'--output',str(security)],
    check=True,env=environment())
network=Path.home()/'.cache/sunstoneos/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
upper_includes=[ROOT/'apps/bluetooth',ROOT/'network/include',network/'include',network/'portable/Compiler/GCC',security]
upper_defines=['SUNSTONE_BLUETOOTH_HEADSET=1','SUNSTONE_AIC8800_RSN=1']
upper_sources=['checks/event-uart-headset-deadline.c','apps/bluetooth/bluetooth.c',
    'drivers/bluetooth/bt_h4.c','drivers/uart/a733_uart.c']
upper=host_rtos.build(a.output/'upper',upper_sources,'event-uart-headset-deadline',
    defines=upper_defines,includes=upper_includes,
    flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
result=host_rtos.run(upper)
assert result.returncode==0,(result.stdout,result.stderr)
logs.append(result.stdout)
timer_mutant=host_rtos.mutated(a.output,'apps/bluetooth/headset.c',
    'EARLIER(ag->codec_deadline);EARLIER(ag->line_deadline);',
    'EARLIER(ag->codec_deadline);','headset-line-timeout-missing.c')
control=host_rtos.build(a.output/'timer-control',upper_sources,'event-uart-headset-deadline',
    defines=upper_defines+['HEADSET_SOURCE="'+str(timer_mutant)+'"'],includes=upper_includes,
    flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
result=host_rtos.run(control)
assert result.returncode!=0 and 'bluetooth_application_next_deadline(&application,UINT64_MAX)==123' in result.stderr,result.stderr
(a.output/'timer-control-failure.log').write_text(result.stdout+result.stderr)
idle_mutant=host_rtos.mutated(a.output,'apps/bluetooth/headset.c','a->uart.config.deferred_irq||',
    '0||','headset-idle-gap.c')
control=host_rtos.build(a.output/'idle-control',upper_sources,'event-uart-headset-deadline',
    defines=upper_defines+['HEADSET_SOURCE="'+str(idle_mutant)+'"'],includes=upper_includes,
    flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
result=host_rtos.run(control)
assert result.returncode!=0 and 'receive_timely(&application,1000000)' in result.stderr,result.stderr
(a.output/'idle-control-failure.log').write_text(result.stdout+result.stderr)
# Reintroducing FIFO polling must violate the no-event register-read check.
mutant=host_rtos.mutated(a.output,'drivers/bluetooth/bt_h4.c',
    'if(q->deferred_irq)return interrupt_step(q,budget,tag,deadline);',
    'if(0)return interrupt_step(q,budget,tag,deadline);','bt-h4-polled.c')
control_sources=[mutant if s=='drivers/bluetooth/bt_h4.c' else s for s in sources]
control=host_rtos.build(a.output/'control',control_sources,'event-uart-h4')
result=subprocess.run([str(control),'idle'],capture_output=True,text=True,env=environment(),timeout=20)
assert result.returncode!=0 and '!task_fixture.lsr_reads' in result.stderr,result.stderr
(a.output/'poll-control-failure.log').write_text(result.stdout+result.stderr)
# Removing RFNE gating wrongly transmits a command before an older short RX burst.
uart_mutant=host_rtos.mutated(a.output,'drivers/uart/a733_uart.c','if(v&8u)tn=0;','if(0)tn=0;','uart-rx-first-missing.c')
control_sources=[uart_mutant if s=='drivers/uart/a733_uart.c' else s for s in sources]
control=host_rtos.build(a.output/'rx-control',control_sources,'event-uart-h4')
result=subprocess.run([str(control),'short-rx'],capture_output=True,text=True,env=environment(),timeout=20)
assert result.returncode!=0 and '!f.transmitted' in result.stderr,result.stderr
(a.output/'rx-control-failure.log').write_text(result.stdout+result.stderr)
query=host_rtos.mutated(a.output,'drivers/uart/a733_uart.c',
    'if(!read_reg(u,RFL,&status))return -1;\n    return !(status&0x1ffu);',
    'if(!read_reg(u,USR,&status))return -1;\n    return !(status&8u);','uart-destructive-quiet.c')
control=host_rtos.build(a.output/'query-control',
    [query if name=='drivers/uart/a733_uart.c' else name for name in sources],'event-uart-h4')
result=subprocess.run([str(control),'quiet-busy'],capture_output=True,text=True,env=environment(),timeout=20)
assert result.returncode!=0 and 'f.busy&&!f.iir_reads' in result.stderr,result.stderr
(a.output/'query-control-failure.log').write_text(result.stdout+result.stderr)
objects=[]
for source in ['drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c']:
    obj=a.output/(Path(source).stem+'.o');objects.append(obj)
    subprocess.run([str(CLANG),'--target=aarch64-none-elf','-std=c11','-Wall','-Wextra','-Werror',
        '-ffreestanding','-fno-builtin','-O2','-I'+str(ROOT/'include'),'-c',str(ROOT/source),'-o',str(obj)],
        check=True,env=environment())
(a.output/'run.log').write_text(''.join(logs))
(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,idle_wakes_per_second=0,
    protocol_timer_cases=41,
    negative_controls=['polled H4 fails no-event register-read check','missing RFNE gate fails short-RX check',
        'missing HFP line timer fails timer query','periodic-owner gap fails event idle case',
        'destructive quiet query clears BUSY before IIR service'],
    source_sha256={s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in CHECK_INPUTS},
    reference_sha256=hashlib.sha256(raw).hexdigest(),native_objects=[str(obj) for obj in objects],
    limits='Actual UART/H4, FreeRTOS kernel and deadline timer; modeled UART/GIC effects, no native headset binding or physical qualification.'),indent=2)+'\n')
print(''.join(logs),end='');print('PASS UART/H4 event cases, five negative controls and native compilation')
