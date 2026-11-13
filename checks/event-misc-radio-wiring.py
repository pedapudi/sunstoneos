#!/usr/bin/env python3
"""Validate native radio task waits, route generation and service publication."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import host_rtos
from harness import environment
ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['apps/os/main.patch','apps/os/radio.c','apps/board/main.c','cmake/sunstone-os-core.cmake',
    'tools/sunstone-manifest.py','apps/event-waits/manifest.json','checks/event-misc-radio-wiring.py',
    'checks/event-misc-radio-wiring.c','checks/event-misc-application.py','checks/host_rtos.py','checks/harness.py',
    'runtime/deadline.c','include/sunstone_deadline.h','checks/host/host_deadline.c','checks/host/host_rtos.c',
    'checks/host/FreeRTOSConfig.h']
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path)
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    generated=a.output/'main.c'
    r=subprocess.run(['patch','--fuzz=0','--output',str(generated),str(ROOT/'apps/board/main.c'),str(ROOT/'apps/os/main.patch')],capture_output=True,text=True)
    (a.output/'patch.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
    spec=importlib.util.spec_from_file_location('extract',ROOT/'checks/event-misc-application.py')
    extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
    source=generated.read_text();production=''.join(extract.function(source,n) for n in ('radio_owner','radio_client'))
    (a.output/'radio-loops.inc').write_text(production)
    shims=(ROOT/'apps/os/radio.c').read_text();shims=shims[shims.index('static unsigned radio_wait_registered'):shims.index('static struct board_radio_profile profile;')]
    (a.output/'radio-shims.inc').write_text(shims.replace('#endif\n',''))
    sources=['checks/event-misc-radio-wiring.c','runtime/deadline.c']
    exe=host_rtos.build(a.output/'build',sources,'wiring',includes=[a.output])
    for mode in range(6):
        r=subprocess.run([str(exe),str(mode)],capture_output=True,text=True,env=environment(),timeout=30)
        (a.output/(str(mode)+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
    mutant=production.replace('int waited=until?board_radio_owner_wait(&radio,until):-1;','vTaskDelay(1);int waited=until?board_radio_owner_wait(&radio,until):-1;')
    assert mutant!=production;(a.output/'radio-loops.inc').write_text(mutant)
    exe=host_rtos.build(a.output/'mutant',sources,'wiring-mutant',includes=[a.output])
    r=subprocess.run([str(exe),'0'],capture_output=True,text=True,env=environment(),timeout=30)
    (a.output/'mutant.log').write_text(r.stdout+r.stderr);assert r.returncode
    (a.output/'radio-loops.inc').write_text(production)
    generated_app=a.output/'generated'
    r=subprocess.run([sys.executable,'-B',str(ROOT/'tools/sunstone-manifest.py'),'emit','--manifest',str(ROOT/'apps/event-waits/manifest.json'),'--output',str(generated_app)],capture_output=True,text=True)
    (a.output/'manifest.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
    app=(generated_app/'sunstone-tables.c').read_text()
    assert '#ifdef SUNSTONE_A733_RADIO_IRQ\nextern void os_radio_irq(void);\n#endif' in app
    assert '#ifdef SUNSTONE_A733_RADIO_IRQ\n    {194,0,0xd8,os_radio_irq},\n#endif' in app
    assert 'if(i==5)vTaskCoreAffinitySet(board.handles[i],1);' in source
    assert 'if(!radio_ready_notified&&board.handles[5])' in source
    assert source.index('pmic_ready,1,memory_order_release')<source.index('radio_ready_notified=1;xTaskNotifyGive(board.handles[5])')
    cmake=(ROOT/'cmake/sunstone-os-core.cmake').read_text()
    assert 'drivers/net/a733_radio_events.c' in cmake and 'SUNSTONE_A733_RADIO_IRQ=1' in cmake
    result=dict(passed=True,physical_access=False,generated_loop_cases=6,tick_mutation_rejected=True,
        limits='Real generated OS task loops and deadline runtime; device and service providers modeled. Route and PMIC publisher checked from generated source.',
        inputs_sha256={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in CHECK_INPUTS})
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS native radio task wiring, service certificates and generated IRQ route')
if __name__=='__main__':main()
