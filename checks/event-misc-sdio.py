#!/usr/bin/env python3
"""Exercise the SDIO interrupt owner with real FreeRTOS and precise waits."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import host_rtos
from harness import environment
ROOT=Path(__file__).resolve().parents[1]
CACHE=Path.home()/'.cache/sunstoneos'
GATE_RUNS=[['--output','{output}'],['--retirement-only','--output','{output}'],
           ['--transport-only','--output','{output}'],['--client-only','--output','{output}'],
           ['--native-only','--output','{output}'],['--plan-only','--output','{output}'],['--composition-only','--output','{output}'],
           ['--rsn-timer-only','--output','{output}']]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--retirement-only',action='store_true');p.add_argument('--transport-only',action='store_true')
    p.add_argument('--native-only',action='store_true');p.add_argument('--client-only',action='store_true')
    p.add_argument('--plan-only',action='store_true');p.add_argument('--composition-only',action='store_true')
    p.add_argument('--rsn-timer-only',action='store_true');a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False)
    if a.rsn_timer_only:
        spec=importlib.util.spec_from_file_location('events',ROOT/'checks/event-misc-application.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        source=(ROOT/'drivers/net/aic8800_sdio_transport.c').read_text()
        enum=source[source.index('enum { BLOCK_LO_READ'):];enum=enum[:enum.index('};')+2]
        (a.output/'transport-phase.inc').write_text(enum+'\n')
        os=(ROOT/'drivers/net/aic8800_supplicant_os.c').read_text();supplicant=(ROOT/'drivers/net/aic8800_supplicant.c').read_text()
        production=''.join(module.function(os,n) for n in ('aic8800_supplicant_os_enter','aic8800_supplicant_os_leave','aic8800_supplicant_os_timer'))
        production+=''.join(module.function(supplicant,n) for n in ('aic8800_supplicant_stop','aic8800_supplicant_observe'))
        (a.output/'rsn-timer-events.inc').write_text(production)
        units=['drivers/net/aic8800_firmware.c','drivers/net/aic8800_runtime.c','drivers/net/aic8800_wifi.c',
               'drivers/net/aic8800_security.c','drivers/net/aic8800_data.c','checks/event-misc-rsn-timer.c',
               'drivers/net/aic8800_sdio_transport.c','drivers/storage/sunxi_sdio.c','drivers/storage/sdio_card.c',
               'drivers/net/aic8800_sdio_codec.c','drivers/storage/storage_digest.c']
        exe=host_rtos.build(a.output/'build',units,'rsn-timer',shim=False,includes=[ROOT/'checks',a.output],
            defines=['SUNSTONE_AIC8800_RSN=1'],flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
        r=subprocess.run([str(exe)],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/'rsn-timer.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        old='if(t->card->host->events)timer_fired=(unsigned)event_timers(t,now);'
        assert source.count(old)==1
        mutant=a.output/'transport-without-timer.c';mutant.write_text(source.replace(old,'if(0&&t->card->host->events)timer_fired=(unsigned)event_timers(t,now);'))
        altered=[mutant if n=='drivers/net/aic8800_sdio_transport.c' else n for n in units]
        exe=host_rtos.build(a.output/'mutant',altered,'rsn-timer-mutant',shim=False,includes=[ROOT/'checks',a.output],
            defines=['SUNSTONE_AIC8800_RSN=1'],flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
        r=subprocess.run([str(exe)],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/'mutant.log').write_text(r.stdout+r.stderr);assert r.returncode
        (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'actual_transport_and_timer_dispatch':True,
            'missing_timer_mutation_rejected':True,'limits':'Supplicant action step is modeled; actual observation, timer dispatch and transport execute.'},indent=2)+'\n')
        print('PASS idle transport dispatches due RSN timer');return
    if a.composition_only:
        cache=CACHE/'sources';tcp=cache/'radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
        kernel=cache/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
        exe=host_rtos.build(a.output/'build',['checks/event-misc-radio-composition.c','drivers/storage/storage_digest.c'],'composition',shim=False,
            includes=[ROOT/'checks',ROOT/'network/include',tcp/'include',tcp/'portable/Compiler/GCC'],
            defines=['SUNSTONE_A733_RADIO_IRQ=1','SUNSTONE_AIC8800_RSN=1','INCLUDE_xSemaphoreGetMutexHolder=1'],
            flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
        r=subprocess.run([str(exe)],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/'composition.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'actual_board_poll_to_wait':True},indent=2)+'\n')
        print('PASS actual board poll-to-wait transitions and shutdown timers');return
    if a.plan_only:
        spec=importlib.util.spec_from_file_location('events',ROOT/'checks/event-misc-application.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        board=(ROOT/'apps/board/radio.c').read_text()
        definitions=board[board.index('static uint64_t event_min'):]
        (a.output/'radio-plan-events.inc').write_text(module.function(definitions,'event_min')+module.function(definitions,'event_work'))
        transport=(ROOT/'drivers/net/aic8800_sdio_transport.c').read_text()
        (a.output/'radio-protocol-deadline.inc').write_text(''.join(module.function(transport,n) for n in
            ('earlier_future','earlier_timer','aic8800_transport_event_deadline')))
        tcp=CACHE/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
        exe=host_rtos.build(a.output/'build',['checks/event-misc-radio-plan.c','runtime/deadline.c'],'plan',
            includes=[a.output,ROOT/'apps/board',ROOT/'network/include',tcp/'include',tcp/'portable/Compiler/GCC'],
            defines=['SUNSTONE_A733_RADIO_IRQ=1','SUNSTONE_AIC8800_RSN=1'])
        r=subprocess.run([str(exe)],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/'plan.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'actual_board_plan':True,
            'limits':'Device/transport ownership is modeled; full controller and protocol cases have separate fixtures.'},indent=2)+'\n')
        print('PASS board ready work and shared/protocol/settling deadlines');return
    if a.client_only:
        spec=importlib.util.spec_from_file_location('events',ROOT/'checks/event-misc-application.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        board=(ROOT/'apps/board/radio.c').read_text();net=(ROOT/'drivers/net/sunstone_net.c').read_text()
        production=module.function(board,'board_radio_client_wait')+'\n'+''.join(module.function(net,n) for n in
            ('sunstone_net_event','net_now_us','independent','client','client_ready','client_gate_enter','client_gate_leave',
             'publish_reply','sunstone_net_submit','sunstone_net_resume'))
        (a.output/'radio-client-events.inc').write_text(production)
        tcp=CACHE/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
        includes=[a.output,ROOT/'apps/board',ROOT/'network/include',tcp/'include',tcp/'portable/Compiler/GCC']
        sources=['checks/event-misc-radio-client.c','runtime/deadline.c']
        exe=host_rtos.build(a.output/'build',sources,'client-events',includes=includes,
            defines=['SUNSTONE_A733_RADIO_IRQ=1','configUSE_APPLICATION_TASK_TAG=1'],link=['-Wl,--wrap=sunstone_deadline_wait_until'])
        for name,args in [('os',[]),('tagged',['tagged'])]:
            r=subprocess.run([str(exe),*args],text=True,capture_output=True,env=environment(),timeout=30)
            (a.output/(name+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        mutant=production.replace('if(sunstone_deadline_wait_until(until)==SUNSTONE_DEADLINE_ERROR)return -1;','vTaskDelay(1);')
        assert mutant!=production;(a.output/'radio-client-events.inc').write_text(mutant)
        exe=host_rtos.build(a.output/'mutant',sources,'client-tick-mutant',includes=includes,
            defines=['SUNSTONE_A733_RADIO_IRQ=1','configUSE_APPLICATION_TASK_TAG=1'],link=['-Wl,--wrap=sunstone_deadline_wait_until'])
        r=subprocess.run([str(exe)],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/'mutant.log').write_text(r.stdout+r.stderr);assert r.returncode
        (a.output/'radio-client-events.inc').write_text(production)
        (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'idle_poll_wakes_per_second_before':1000,
            'idle_poll_wakes_per_second_after':0,'tagged_and_os_clients':True,'tick_mutation_rejected':True},indent=2)+'\n')
        print('PASS radio client idle/reply/capacity and notification indices');return
    if a.native_only:
        units=['apps/board/radio.c','drivers/net/a733_radio_events.c','drivers/net/a733_radio_platform.c',
               'drivers/net/a733_radio_platform_native.c','drivers/net/aic8800_sdio_transport.c','drivers/storage/sunxi_sdio.c','drivers/net/sunstone_net.c']
        kernel=CACHE/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
        tcp=CACHE/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
        headers=CACHE/'host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include'
        flags=[str(host_rtos.CLANG),'--target=aarch64-none-elf','-mcpu=cortex-a55','-std=c11','-O2',
               '-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mgeneral-regs-only','-include','stdint.h',
               '-DSUNSTONE_A733_RADIO_IRQ=1','-DSUNSTONE_AIC8800_RSN=1','-DconfigUSE_APPLICATION_TASK_TAG=1','-DINCLUDE_xSemaphoreGetMutexHolder=1',
               '-DINCLUDE_xTaskGetSchedulerState=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1',
               '-I'+str(ROOT/'network/include'),'-I'+str(ROOT/'include'),'-I'+str(kernel/'include'),
               '-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),'-I'+str(tcp/'include'),'-I'+str(tcp/'portable/Compiler/GCC'),
               '-isystem',str(headers)]
        commands=[]
        for index,unit in enumerate(units):
            cmd=[*flags,'-MMD','-MF',str(a.output/(str(index)+'.d')),'-c',str(ROOT/unit),'-o',str(a.output/(str(index)+'.o'))]
            r=subprocess.run(cmd,text=True,capture_output=True,env=environment());commands.append(cmd)
            (a.output/(str(index)+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'commands':commands,
            'native_selected_sources':units,'limits':'Selected event branch compiles; image routing and OS task waits are root integration inputs.'},indent=2)+'\n')
        print('PASS seven native SDIO event branch compilation units');return
    if a.retirement_only:
        units=['drivers/net/a733_radio_platform.c','drivers/net/a733_radio_result.c','drivers/storage/sunxi_sdio.c',
               'drivers/storage/sdio_card.c','drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c',
               'drivers/power/a733_pmic.c','drivers/gpu/a733_gpu_resources.c','drivers/pinctrl/a733_pins.c',
               'drivers/pinctrl/a733_pin_tables.c','checks/event-misc-sdio-retire.c']
        exe=host_rtos.build(a.output/'build',units,'retirement',shim=False,includes=[ROOT/'checks'])
        r=subprocess.run([str(exe)],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/'retirement.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'full_platform_retirement':True},indent=2)+'\n')
        print('PASS full radio normal and abort retirement gates');return
    if a.transport_only:
        units=['drivers/net/aic8800_firmware.c','drivers/net/aic8800_runtime.c','drivers/net/aic8800_wifi.c',
               'drivers/net/aic8800_security.c','drivers/net/aic8800_data.c','checks/event-misc-sdio-transport.c',
               'drivers/net/aic8800_sdio_transport.c','drivers/storage/sunxi_sdio.c','drivers/storage/sdio_card.c',
               'drivers/net/aic8800_sdio_codec.c','drivers/storage/storage_digest.c']
        exe=host_rtos.build(a.output/'build',units,'transport',shim=False,includes=[ROOT/'checks'])
        r=subprocess.run([str(exe)],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/'transport.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        (a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'protocol_event_planning':True},indent=2)+'\n')
        print('PASS AIC function interrupt and precise credit planning');return
    spec=importlib.util.spec_from_file_location('events',ROOT/'checks/event-misc-application.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    platform=(ROOT/'drivers/net/a733_radio_platform.c').read_text()
    names=('host_time','host_access','host_read','host_write','host_barrier','a733_radio_platform_event_prepare')
    (a.output/'radio-host-access.inc').write_text('\n'.join(module.function(platform,n) for n in names))
    production=(ROOT/'drivers/net/a733_radio_events.c').read_text()
    (a.output/'radio-events.inc').write_text(production)
    sources=['checks/event-misc-sdio.c','drivers/storage/sunxi_sdio.c','runtime/deadline.c']
    results=[]
    def run(name,arguments=()):
        exe=host_rtos.build(a.output/name,sources,name,includes=[a.output])
        r=subprocess.run([str(exe),*arguments],text=True,capture_output=True,env=environment(),timeout=30)
        (a.output/(name+'.log')).write_text(r.stdout+r.stderr)
        results.append({'case':name,'returncode':r.returncode})
        return r
    for name,args in [('normal',()),('refusal',('refusal',)),('missing',('missing',)),('late-error',('late-error',))]:
        r=run(name,args);assert r.returncode==0,r.stderr
    mutant=production.replace('int result=sunstone_deadline_wait_until(until);','vTaskDelay(1);int result=SUNSTONE_DEADLINE_NOTIFIED;')
    assert mutant!=production;(a.output/'radio-events.inc').write_text(mutant)
    r=run('tick-mutant');assert r.returncode!=0,'idle tick wake accepted'
    (a.output/'radio-events.inc').write_text(production)
    inputs=['checks/event-misc-sdio.c','checks/event-misc-sdio.py','drivers/storage/sunxi_sdio.c',
            'drivers/net/a733_radio_events.c','drivers/net/a733_radio_platform.c','include/a733_radio_events.h','include/sunxi_sdio.h']
    record={'passed':True,'physical_access':False,'runs':results,'idle_poll_wakes_per_second_before':1000,
            'idle_poll_wakes_per_second_after':0,'source_sha256':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs},
            'references':{'manual_section':'A733 user manual1.00,11.3.8.13–17',
                'manual_sha256':hashlib.sha256((CACHE/'sources/camera-research/A733_User_Manual_V1.00.txt').read_bytes()).hexdigest(),
                'vendor_source_sha256':'52c7f92db77290437b7818f3c3a92a2956c1ab637214794e56220d0dde99ce16'},
            'limits':'Modeled SMHC/GIC; real platform host_access admission, owner and controller code. No physical qualification.'}
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n');print('PASS SDIO owner event cases and idle wake mutation')
if __name__=='__main__':main()
