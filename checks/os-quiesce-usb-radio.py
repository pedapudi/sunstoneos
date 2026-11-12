#!/usr/bin/env python3
"""Exercise the USB and radio return adapters without hardware access."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/client.c', 'apps/os/main.patch', 'apps/os/quiesce.h',
                'apps/os/quiesce.c', 'apps/os/radio.c', 'apps/os/radio_scan.c', 'apps/board/radio.c']

PRELUDE = r'''
#include "quiesce.h"
#include <assert.h>
#include <stdlib.h>
static unsigned returning=1, failing=1, registered, notifications, fatal;
static struct { void *handles[8]; struct { uint64_t deadline_us; } session; } board;
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

USB_CASES = r'''
int main(void)
{
    returning=0;assert(os_usb_quiesce_start()&&registered==0);
    returning=1;assert(os_usb_quiesce_start()&&registered==1);
    board.handles[4]=(void *)1;
    assert(usb_quiesce.step(&usb_quiesce)==OS_QUIESCE_PENDING&&notifications==1);
    /* A running controller and an empty port both lack a stop proof. */
    usb_quiesce_unproven(1);
    assert(usb_quiesce.step(&usb_quiesce)==OS_QUIESCE_FAULT);
    assert(usb_quiesce.site==0x1801&&usb_quiesce.value==2&&!fatal);
    usb_quiesce_unproven(0);assert(usb_quiesce.value==2&&!fatal);
    atomic_store(&usb_quiesce.fault_state,0);
    usb_quiesce_unproven(0);assert(usb_quiesce.value==1&&!fatal);
    /* A requested normal return uses the same fault latch. */
    failing=0;atomic_store(&usb_quiesce.requested,1);
    atomic_store(&usb_quiesce.result,OS_QUIESCE_PENDING);
    atomic_store(&usb_quiesce.fault_state,0);
    usb_quiesce_unproven(1);
    assert(os_quiesce_result(&usb_quiesce)==OS_QUIESCE_FAULT&&usb_quiesce.value==2&&!fatal);
    os_quiesce_complete(&usb_quiesce);
    assert(os_quiesce_result(&usb_quiesce)==OS_QUIESCE_FAULT);
    atomic_store(&usb_quiesce.requested,0);
    failing=0;usb_quiesce_unproven(0);assert(fatal==1);
    return 0;
}
'''

RADIO_SUPPORT = r'''
#define OS_FAILURE_RETURN_US 3000000u
#define OS_GRANT_RADIO 16u
#define OS_RADIO_WORKLOAD 3u
enum { BOARD_RADIO_PENDING, BOARD_RADIO_RETAINED, BOARD_RADIO_INVALID, BOARD_RADIO_SCANNED, BOARD_RADIO_DOWN };
struct board_radio {
    struct { unsigned scan_only,entropy_only; } config;
    struct { unsigned fail_line,fail_stage,fault,held,retained; struct { unsigned id; } pmic_request; } platform;
    struct { unsigned fail_line,fail_stage; struct { unsigned pending,held; } transport; } session;
};
struct os_session { unsigned granted; struct { unsigned workload; } board; } session;
static unsigned shutdown_calls,shutdown_accept,wiped,wipe_polls;
static uint64_t shutdown_deadline;
static const struct os_session *os_session_admitted(void) { return &session; }
static uint64_t a733_smp_counter(void) { return 1000000u*24; }
static int board_radio_shutdown(struct board_radio *r,uint64_t deadline)
{ (void)r; ++shutdown_calls;shutdown_deadline=deadline;return shutdown_accept; }
static void os_wifi_credential_poll(void) { ++wipe_polls; }
static int os_wifi_credential_wiped(void) { return wiped; }
'''

RADIO_CASES = r'''
static void reset_radio(void)
{
    atomic_store(&radio_quiesce.result,OS_QUIESCE_PENDING);
    atomic_store(&radio_quiesce.requested,1);
    atomic_store(&radio_quiesce.fault_state,0);
    shutdown_started=shutdown_calls=shutdown_accept=wiped=wipe_polls=0;
    failing=1;
}
int main(void)
{
    struct board_radio r={.config.scan_only=1};
    board.handles[5]=(void *)1;board.handles[6]=(void *)2;
    board.session.deadline_us=60000000;
    session.granted=0;assert(os_radio_start()&&registered==0);
    session.granted=OS_GRANT_RADIO;session.board.workload=OS_RADIO_WORKLOAD;
    assert(os_radio_start()&&registered==1);
    assert(!os_radio_quiesce_poll(&r,BOARD_RADIO_PENDING));
    reset_radio();
    assert(radio_quiesce.step(&radio_quiesce)==OS_QUIESCE_PENDING&&notifications==2);
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_SCANNED)==1&&shutdown_calls==1&&!shutdown_started);
    shutdown_accept=1;
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_SCANNED)==1&&shutdown_calls==2&&shutdown_started);
    assert(shutdown_deadline==4000000);
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_PENDING)==1&&shutdown_calls==2);
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_DOWN)==1&&wipe_polls==1);
    wiped=1;assert(os_radio_quiesce_poll(&r,BOARD_RADIO_DOWN)==2);
    assert(os_radio_quiesce_parked()&&os_quiesce_result(&radio_quiesce)==OS_QUIESCE_DONE&&!fatal);
    /* A retained transport cannot be reported as a completed shutdown. */
    reset_radio();r.session.transport.pending=1;
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_DOWN)==2);
    assert(os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT&&radio_quiesce.value==0x903&&!fatal);
    r.session.transport.pending=0;
    reset_radio();r.platform.fail_line=14;r.platform.fail_stage=5;
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_RETAINED)==2);
    assert(radio_quiesce.site==0x1901&&radio_quiesce.value==(14u<<16|5)&&!fatal);
    /* No scan stop exists for bootstrap or the full network station. */
    reset_radio();assert(os_radio_quiesce_poll(&r,BOARD_RADIO_PENDING)==2&&radio_quiesce.value==0x904&&!fatal);
    reset_radio();r.config.scan_only=0;
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_SCANNED)==2&&radio_quiesce.value==0x902&&!fatal);
    /* A normal-return failure is captured, and completion cannot erase it. */
    reset_radio();failing=0;r.config.scan_only=1;
    assert(os_radio_quiesce_poll(&r,BOARD_RADIO_RETAINED)==2);
    assert(os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT&&!fatal);
    os_quiesce_complete(&radio_quiesce);
    assert(os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT);
    atomic_store(&radio_quiesce.requested,0);
    radio_quiesce_refuse(1);assert(fatal==1);
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
    result = build(out, 'usb', body, USB_CASES)
    assert result.returncode == 0, result.stderr
    registration = 'os_quiesce_register(&usb_quiesce)'
    assert body.count(registration) == 1
    assert build(out, 'usb-no-registration', body.replace(registration, '1'), USB_CASES).returncode != 0
    step = '    if(board.handles[4])xTaskNotifyGive(board.handles[4]);'
    assert body.count(step) == 1
    assert build(out, 'usb-exit-in-return', body.replace(step, '    platform_exit(6);\n' + step), USB_CASES).returncode != 0
    assert 'if(os_quiesce_requested(&usb_quiesce))goto stopped;' in source
    assert 'if(os_quiesce_requested(&usb_quiesce))break;' in source
    patch = (ROOT / 'apps/os/main.patch').read_text()
    assert '(!os_usb_quiesce_start||!os_usb_quiesce_start())' in patch
    print('PASS USB return: requests wake the owner; active and empty hosts report unproved drain; admission stays open; two mutants fail')


def radio(out):
    source = (ROOT / 'apps/os/radio.c').read_text()
    body = RADIO_SUPPORT + source[source.index('static enum os_quiesce_result radio_quiesce_step'):source.index('static struct board_radio_profile profile;')]
    body += source[source.index('int os_radio_start(void)'):]
    result = build(out, 'radio', body, RADIO_CASES)
    assert result.returncode == 0, result.stderr
    registration = 'os_quiesce_register(&radio_quiesce)'
    assert body.count(registration) == 1
    assert build(out, 'radio-no-registration', body.replace(registration, '1'), RADIO_CASES).returncode != 0
    step = '    if(!os_radio_quiesce_requested())return 0;'
    assert body.count(step) == 1
    assert build(out, 'radio-exit-in-return', body.replace(step, step + '\n    platform_exit(6);'), RADIO_CASES).returncode != 0
    patch = (ROOT / 'apps/os/main.patch').read_text()
    assert 'os_radio_quiesce_poll(&radio,(int)r)' in patch
    assert 'os_headset_quiesce_pending' in patch
    assert 'if(board_live(NULL)&&!os_radio_quiesce_requested())' in patch
    print('PASS radio return: idle scan shutdown, busy retry, credential wipe, retained and unsupported paths; two mutants fail')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    usb(args.output)
    radio(args.output)


if __name__ == '__main__':
    main()
