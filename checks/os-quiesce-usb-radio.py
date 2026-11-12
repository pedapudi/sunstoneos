#!/usr/bin/env python3
"""Exercise the USB and radio return adapters without hardware access."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/client.c', 'apps/os/main.patch', 'apps/os/quiesce.h',
                'checks/os-quiesce-radio.py', 'apps/os/quiesce.c', 'apps/os/radio.c', 'apps/os/radio_scan.c', 'apps/board/radio.c']

PRELUDE = r'''
#define SUNSTONE_A733_USB_FAILURE_TEST 1
#include "quiesce.h"
#include <assert.h>
#include <stdlib.h>
static unsigned returning=1, failing=1, registered, notifications, fatal, closing;
static uint64_t published_deadline;
static int os_cpu_return_requested(void) {return closing;}
static uint64_t os_cpu_return_deadline_us(void) {return published_deadline;}
static struct {
 void *handles[8]; struct { uint64_t deadline_us; } session;
 struct {unsigned core_started,stop_phase; struct {unsigned marker;} core; struct {unsigned stop_phase;} platform;} host;
 struct {unsigned marker;} typec;
} board;
#ifndef SUNSTONE_CRUMB_FAIL_BOARD
#define SUNSTONE_CRUMB_FAIL_BOARD 1u
#endif
#undef SUNSTONE_CRUMB_HERE
#undef SUNSTONE_CRUMB_FAIL_SITE
#define SUNSTONE_CRUMB_HERE() ((const void *)1)
#define SUNSTONE_CRUMB_FAIL_SITE(group) (1u<<12)
static int os_returning(void) { return returning; }
static int os_cpu_return_failing(void) { return failing; }
static void xTaskNotifyGive(void *task) { assert(task); ++notifications; }
static void platform_exit(unsigned detail) { (void)detail; abort(); }
static void board_fail_at(uint32_t site,uint32_t value,const void *caller)
{ (void)site; (void)value; (void)caller; ++fatal; }
int os_quiesce_register(struct os_quiesce *q)
{ assert(q->order==OS_QUIESCE_DEVICES); ++registered; q->registered=1; return 1; }
int os_quiesce_requested(const struct os_quiesce *q) { return atomic_load(&q->requested); }
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *q) { return atomic_load(&q->result); }
'''

USB_SUPPORT = r'''
#define SUNSTONE_BOARD_USB2_IRQ 1
#define SUNSTONE_A733_OS_USB_CLASSES 1
static int halt_result,irq_result,broker_idle,platform_result,typec_result,request_result=1;
static unsigned halt_calls,irq_calls,platform_calls,broker_stops,typec_requests;
static uint64_t usb_now;
static uint64_t a733_smp_counter(void) {return usb_now*24;}
static void os_usb_service_retire(void) {}
static void os_usb_service_dispatch(void) {assert(halt_result==1&&irq_result==1);}
static int os_usb_service_idle(void) {return broker_idle;}
static void os_usb_service_stopped(void) {assert(halt_result==1&&irq_result==1);++broker_stops;}
static void os_usb_serial_native_stopped(void) {assert(halt_result==1&&irq_result==1);}
static int a733_xhci_stop_draining(const void *core) {(void)core;return halt_result==0;}
static int board_host_irq_before(void) {return 1;}
static int board_host_irq_after(void) {return 1;}
static int a733_usb2_native_halt(void *host,uint64_t deadline)
{(void)host;assert(deadline==3000000);++halt_calls;return halt_result;}
static int board_host_irq_retire(uint64_t deadline) {assert(deadline==3000000&&halt_result==1);++irq_calls;return irq_result;}
static int a733_usb2_native_stop(void *host,uint64_t deadline)
{(void)host;assert(deadline==3000000&&halt_result==1&&irq_result==1&&broker_idle);++platform_calls;return platform_result;}
static int a733_typec_native_stop_request(void *typec,uint64_t deadline)
{(void)typec;assert(deadline==3000000&&platform_result==1);++typec_requests;return request_result;}
static int a733_typec_native_stop_result(void *typec) {(void)typec;return typec_result;}
'''

USB_CASES = r'''
static void reset_usb(void)
{
 atomic_store(&usb_quiesce.result,OS_QUIESCE_PENDING);
 atomic_store(&usb_quiesce.fault_state,0);atomic_store(&usb_quiesce.requested,1);
 halt_result=irq_result=platform_result=typec_result=0;broker_idle=request_result=1;
 halt_calls=irq_calls=platform_calls=broker_stops=typec_requests=0;
 usb_now=0;
 board.host.core_started=1;
}
int main(void)
{
    assert(!os_usb_failure_ready());os_usb_failure_checkpoint(1);assert(!os_usb_failure_ready());
    os_usb_failure_checkpoint(2);assert(os_usb_failure_ready());
    os_usb_failure_checkpoint(1);assert(os_usb_failure_ready());
    returning=0;assert(os_usb_quiesce_start()&&registered==0);
    returning=1;assert(os_usb_quiesce_start()&&registered==1);
    board.handles[4]=(void *)1;
    assert(usb_quiesce.step(&usb_quiesce)==OS_QUIESCE_PENDING&&notifications==1);
    usb_stop_fault(0x801,2);
    assert(usb_quiesce.step(&usb_quiesce)==OS_QUIESCE_FAULT);
    assert(usb_quiesce.site==0x1801&&usb_quiesce.value==2&&!fatal);
    usb_stop_fault(0x801,1);assert(usb_quiesce.value==2&&!fatal);
    atomic_store(&usb_quiesce.fault_state,0);
    usb_stop_fault(0x801,1);assert(usb_quiesce.value==1&&!fatal);
    /* A requested normal return uses the same fault latch. */
    failing=0;atomic_store(&usb_quiesce.requested,1);
    atomic_store(&usb_quiesce.result,OS_QUIESCE_PENDING);
    atomic_store(&usb_quiesce.fault_state,0);
    usb_stop_fault(0x801,2);
    assert(os_quiesce_result(&usb_quiesce)==OS_QUIESCE_FAULT&&usb_quiesce.value==2&&!fatal);
    os_quiesce_complete(&usb_quiesce);
    assert(os_quiesce_result(&usb_quiesce)==OS_QUIESCE_FAULT);
    atomic_store(&usb_quiesce.requested,0);
    failing=0;usb_stop_fault(0x801,1);assert(fatal==1);fatal=0;
    reset_usb();
    closing=1;published_deadline=0;assert(usb_return_unpublished());
    published_deadline=3000000;assert(!usb_return_unpublished());
    closing=0;published_deadline=0;assert(!usb_return_unpublished());
    assert(!usb_return_poll(0)&&!halt_calls); /* Deadline not published yet. */
    assert(!usb_return_poll(3000000)&&halt_calls==1&&!irq_calls);
    halt_result=1;assert(!usb_return_poll(3000000)&&irq_calls==1&&!broker_stops);
    irq_result=1;broker_idle=0;assert(!usb_return_poll(3000000)&&broker_stops==1&&!platform_calls);
    broker_idle=1;assert(!usb_return_poll(3000000)&&platform_calls==1&&!typec_requests);
    platform_result=1;assert(!usb_return_poll(3000000)&&typec_requests==1);
    assert(os_quiesce_result(&usb_quiesce)==OS_QUIESCE_PENDING);
    typec_result=1;assert(usb_return_poll(3000000)==1&&os_quiesce_result(&usb_quiesce)==OS_QUIESCE_DONE);
    reset_usb();halt_result=1;assert(!usb_return_poll(3000000)&&irq_calls==1);
    usb_now=3000000;assert(usb_return_poll(3000000)==-1&&irq_calls==1&&halt_calls==1);
    /* Untouched constructors use the same published terminal proofs. */
    reset_usb();board.host.core_started=0;
    halt_result=irq_result=platform_result=typec_result=1;
    assert(usb_return_poll(3000000)==1&&os_quiesce_result(&usb_quiesce)==OS_QUIESCE_DONE);
    for(unsigned fault=0;fault<5;++fault){
      reset_usb();halt_result=irq_result=platform_result=typec_result=1;
      if(fault==0)halt_result=-1;if(fault==1)irq_result=-1;
      if(fault==2)platform_result=-1;if(fault==3)request_result=-1;if(fault==4)typec_result=-1;
      assert(usb_return_poll(3000000)==-1&&os_quiesce_result(&usb_quiesce)==OS_QUIESCE_FAULT&&!fatal);
      os_quiesce_complete(&usb_quiesce);assert(os_quiesce_result(&usb_quiesce)==OS_QUIESCE_FAULT);
    }
    return 0;
}
'''




def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for offset in range(opening, len(source)):
        depth += source[offset] == '{'
        depth -= source[offset] == '}'
        if not depth:
            return source[start:offset + 1] + '\n'
    raise AssertionError('unclosed function: ' + signature)


def build(out, name, body, cases):
    folder = out / name
    folder.mkdir(parents=True, exist_ok=True)
    source, binary = folder / 'check.c', folder / 'check'
    # Run the production fault/completion transitions. The request bit here
    # represents a class whose supervisor publication is already complete.
    registry = (ROOT / 'apps/os/quiesce.c').read_text()
    transitions = function(registry, 'void os_quiesce_complete(') + function(registry, 'int os_quiesce_fault(')
    source.write_text(PRELUDE + transitions + body + cases)
    subprocess.run(['clang-21', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-I' + str(ROOT / 'apps/os'), '-I' + str(ROOT / 'include'),
                    str(source), '-o', str(binary)], check=True, capture_output=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def usb(out):
    source = (ROOT / 'apps/os/client.c').read_text()
    body = source[source.index('static enum os_quiesce_result usb_quiesce_step'):source.index('void os_application_usb_changed(void);')]
    body += USB_SUPPORT + function(source, 'static int usb_return_poll(')
    result = build(out, 'usb', body, USB_CASES)
    assert result.returncode == 0, result.stderr
    registration = 'os_quiesce_register(&usb_quiesce)'
    assert body.count(registration) == 1
    assert build(out, 'usb-no-registration', body.replace(registration, '1'), USB_CASES).returncode != 0
    step = '    if(os_quiesce_fault(&usb_quiesce,site,value,caller))return;'
    assert body.count(step) == 1
    assert build(out, 'usb-exit-in-return', body.replace(step, step.replace('return;', '{platform_exit(6);return;}')), USB_CASES).returncode != 0
    halt = 'int result=a733_usb2_native_halt(&board.host,deadline);'
    assert build(out, 'usb-skip-halt-proof', body.replace(halt, 'int result=1;'), USB_CASES).returncode != 0
    typec = 'result=a733_typec_native_stop_result(&board.typec);'
    assert build(out, 'usb-skip-typec-proof', body.replace(typec, 'result=1;'), USB_CASES).returncode != 0
    assert 'if(usb_return_unpublished())' in source
    assert 'if(board.host.platform.phase==6u&&!board.host.core_started)os_usb_failure_checkpoint(1);' in source
    assert 'if(os_quiesce_requested(&usb_quiesce))goto stopped;' in source
    assert 'if(os_quiesce_requested(&usb_quiesce))break;' in source
    patch = (ROOT / 'apps/os/main.patch').read_text()
    assert 'if(board.typec.provider.state==A733_TYPEC_LOOK)os_usb_failure_checkpoint(2);' in patch
    assert patch.count('os_returning()&&os_cpu_return_requested()&&!os_cpu_return_deadline_us()') == 2
    assert '(!os_usb_quiesce_start||!os_usb_quiesce_start())' in patch
    print('PASS USB return: original owner orders halt, IRQ, broker, platform and Type-C proofs; faults stay latched; four mutants fail')


def radio(out):
    subprocess.run([sys.executable, str(ROOT / 'checks/os-quiesce-radio.py'),
                    '--output', str(out / 'radio')], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    usb(args.output)
    radio(args.output)


if __name__ == '__main__':
    main()
