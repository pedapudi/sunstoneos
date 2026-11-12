#!/usr/bin/env python3
"""Exercise the USB and radio return adapters without hardware access."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/client.c', 'apps/os/main.patch', 'apps/os/quiesce.h']

PRELUDE = r'''
#include "quiesce.h"
#include <assert.h>
#include <stdlib.h>
static unsigned returning=1, failing=1, registered, notifications, fatal;
static struct { void *handles[8]; } board;
#define SUNSTONE_CRUMB_FAIL_BOARD 1u
#define SUNSTONE_CRUMB_HERE() ((const void *)1)
#define SUNSTONE_CRUMB_FAIL_SITE(group) (1u<<12)
static int os_returning(void) { return returning; }
static void xTaskNotifyGive(void *task) { assert(task); ++notifications; }
static void platform_exit(unsigned detail) { (void)detail; abort(); }
static void board_fail_at(uint32_t site,uint32_t value,const void *caller)
{ (void)site; (void)value; (void)caller; ++fatal; }
int os_quiesce_register(struct os_quiesce *q)
{ assert(q->order==OS_QUIESCE_DEVICES); ++registered; q->registered=1; return 1; }
int os_quiesce_requested(const struct os_quiesce *q) { return atomic_load(&q->requested); }
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *q) { return atomic_load(&q->result); }
int os_quiesce_fault(struct os_quiesce *q,uint32_t site,uint32_t value,const void *caller)
{
    if(!failing)return 0;
    if(!atomic_exchange(&q->fault_state,1)){q->site=site;q->value=value;q->caller=caller;}
    atomic_store(&q->result,OS_QUIESCE_FAULT);return 1;
}
void os_quiesce_complete(struct os_quiesce *q) { atomic_store(&q->result,OS_QUIESCE_DONE); }
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
    failing=0;usb_quiesce_unproven(0);assert(fatal==1);
    return 0;
}
'''


def build(out, name, body, cases):
    folder = out / name
    folder.mkdir(parents=True, exist_ok=True)
    source, binary = folder / 'check.c', folder / 'check'
    source.write_text(PRELUDE + body + cases)
    subprocess.run(['clang-21', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-I' + str(ROOT / 'apps/os'),
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    usb(args.output)


if __name__ == '__main__':
    main()
