#!/usr/bin/env python3
"""Exercise radio return coordination, retained faults and source mutations."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/radio.c', 'apps/os/quiesce.c', 'apps/os/quiesce.h',
                'apps/os/radio_scan.c', 'apps/radio-scan/main.c', 'cmake/a733-os.cmake']
PRELUDE = r'''
#include "quiesce.h"
#include <assert.h>
#include <stdlib.h>
static unsigned requested,returning=1,failing=1,notifications,fatal,headset;
static uint64_t deadline=2500000;
static struct { void *handles[8]; } board;
#define OS_GRANT_RADIO 16u
#define OS_GRANT_HEADSET 64u
#define OS_RADIO_WORKLOAD 3u
#undef SUNSTONE_CRUMB_HERE
#define SUNSTONE_CRUMB_HERE() ((const void *)1)
int os_cpu_return_requested(void){return requested;}
int os_cpu_return_failing(void){return failing;}
static uint64_t os_cpu_return_deadline_us(void){return deadline;}
static int os_returning(void){return returning;}
static int os_granted(unsigned grant){return grant==OS_GRANT_HEADSET&&headset;}
static void xTaskNotifyGive(void *task){assert(task);++notifications;}
static void platform_exit(unsigned detail){(void)detail;abort();}
static void board_fail_at(uint32_t site,uint32_t value,const void *caller)
{(void)site;(void)value;(void)caller;++fatal;}
#include "quiesce.c"
enum a733_radio_session_result {A733_SESSION_PENDING,A733_SESSION_BUSY,A733_SESSION_DOWN,A733_SESSION_RETAINED,A733_SESSION_INVALID};
enum board_radio_result {BOARD_RADIO_DOWN,BOARD_RADIO_RETAINED,BOARD_RADIO_INVALID};
struct board_radio {
    struct {unsigned scan_only,associate,entropy_only;} config;
    struct {unsigned fail_line,fail_stage;} platform;
    struct {unsigned fail_line,fail_stage,stop_requested;} session;
};
static struct os_session {unsigned granted;struct {unsigned workload;} board;} session;
static const struct os_session *os_session_admitted(void){return &session;}
static unsigned stop_calls,release_calls,poll_calls,retain_calls,wiped,wipe_polls,stop_accept=1,release_accept=1,peer;
static unsigned down_calls,down_proved;
static int release_ready;
static uint64_t accepted_deadline;
static enum a733_radio_session_result session_result=A733_SESSION_PENDING;
static enum board_radio_result board_radio_stop_result(struct board_radio *r,enum a733_radio_session_result status)
{(void)r;return status==A733_SESSION_DOWN?BOARD_RADIO_DOWN:BOARD_RADIO_RETAINED;}
static int a733_radio_session_stop(void *s,uint64_t d)
{(void)s;++stop_calls;accepted_deadline=d;return stop_accept;}
static int a733_radio_session_stop_release(void *s){(void)s;++release_calls;return release_accept;}
static int a733_radio_session_peer_stopped(void *s){(void)s;return peer;}
static enum a733_radio_session_result a733_radio_session_poll(void *s){(void)s;++poll_calls;return session_result;}
static enum a733_radio_session_result a733_radio_session_retain(void *s){(void)s;++retain_calls;return A733_SESSION_RETAINED;}
static void os_wifi_credential_poll(void){++wipe_polls;}
static int os_wifi_credential_wiped(void){return wiped;}
'''
CASES = r'''
int os_headset_module_release_ready(void){return release_ready;}
void os_headset_module_down(int proved){++down_calls;down_proved=(unsigned)proved;}
static void reset(void)
{
    atomic_store(&radio_quiesce.result,OS_QUIESCE_PENDING);
    atomic_store(&radio_quiesce.requested,1);
    atomic_store(&radio_quiesce.fault_state,0);
    atomic_store(&armed_classes,1u<<OS_QUIESCE_DEVICES);
    shutdown_started=module_notified=stop_calls=release_calls=poll_calls=retain_calls=0;
    down_calls=down_proved=peer=wiped=wipe_polls=headset=0;
    stop_accept=release_accept=1;release_ready=0;failing=1;deadline=2500000;
    session_result=A733_SESSION_PENDING;
}
int main(void)
{
    board.handles[5]=(void *)1;board.handles[6]=(void *)2;
    assert(os_radio_start()&&!radio_quiesce.registered);
    session.granted=OS_GRANT_RADIO;session.board.workload=OS_RADIO_WORKLOAD;
    assert(os_radio_start()&&radio_quiesce.registered);
    struct board_radio r={.config.scan_only=1};
    assert(!os_radio_quiesce_poll(&r,0));requested=1;
    assert(os_quiesce_step(NULL)==OS_QUIESCE_PENDING&&notifications==2);
    reset();headset=1;
    assert(os_radio_quiesce_poll(&r,0)==1&&stop_calls==1&&poll_calls==1&&!release_calls);
    assert(accepted_deadline==deadline); /* The remaining supervisor window, not now+3s. */
    release_ready=1;
    assert(os_radio_quiesce_poll(&r,0)==1&&stop_calls==1&&release_calls==1&&poll_calls==2);
    session_result=A733_SESSION_DOWN;peer=1;
    assert(os_radio_quiesce_poll(&r,0)==1&&down_calls==1&&down_proved==1&&wipe_polls==1);
    wiped=1;assert(os_radio_quiesce_poll(&r,0)==2&&os_radio_quiesce_parked());
    assert(os_quiesce_result(&radio_quiesce)==OS_QUIESCE_DONE&&!fatal&&down_calls==1);
    reset();deadline=0;assert(os_radio_quiesce_poll(&r,0)==1&&!stop_calls&&!poll_calls);
    reset();headset=1;release_ready=-1;
    assert(os_radio_quiesce_poll(&r,0)==2&&retain_calls==1&&!release_calls&&!poll_calls);
    assert(radio_quiesce.value==0x906&&os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT&&!down_proved);
    reset();headset=1;session_result=A733_SESSION_DOWN;release_ready=1;
    wiped=1;assert(os_radio_quiesce_poll(&r,0)==2&&down_calls==1&&!down_proved);
    reset();session_result=A733_SESSION_RETAINED;r.platform.fail_line=19;r.platform.fail_stage=7;
    assert(os_radio_quiesce_poll(&r,0)==2&&radio_quiesce.value==(19u<<16|7));
    assert(os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT&&!fatal);
    os_quiesce_complete(&radio_quiesce);assert(os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT);
    reset();r.config.scan_only=0;session_result=A733_SESSION_DOWN;
    assert(os_radio_quiesce_poll(&r,0)==2&&radio_quiesce.value==0x905&&poll_calls==1);
    reset();r.config.scan_only=1;r.config.associate=1;session_result=A733_SESSION_DOWN;
    assert(os_radio_quiesce_poll(&r,0)==2&&radio_quiesce.value==0x905);
    reset();r.config.associate=0;r.config.entropy_only=1;
    assert(os_radio_quiesce_poll(&r,0)==2&&radio_quiesce.value==0x905&&!stop_calls);
    reset();r.config.entropy_only=0;failing=0;session_result=A733_SESSION_RETAINED;
    assert(os_radio_quiesce_poll(&r,0)==2&&os_quiesce_result(&radio_quiesce)==OS_QUIESCE_FAULT&&!fatal);
    atomic_store(&radio_quiesce.requested,0);radio_quiesce_refuse(1);assert(fatal==1);
    return 0;
}
'''


def application(out, compiler):
    """Run the actual application with normal and forced-success termination."""
    (out / 'radio_scan-app.h').write_text(r'''
#include <stdint.h>
#include <stddef.h>
struct ss_grant {unsigned unused;};
struct ss_radio_status {uint32_t stage,failed,done,reserved;};
int ss_live(void);void ss_progress(void);void ss_radio_status(struct ss_radio_status *);
void ss_usb_wait(uint64_t);
_Noreturn void ss_fail(uint32_t);
_Noreturn void ss_finish(const void *,size_t);
''')
    fixture = out / 'scan-failure.c'
    fixture.write_text(r'''
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include "radio_scan-app.h"
static jmp_buf terminal;
static struct ss_radio_status status;
static unsigned failure,finished,progress,waits,polls;
void radio_scan_main(const struct ss_grant *);
int ss_live(void){return 1;}
void ss_progress(void){++progress;}
void ss_radio_status(struct ss_radio_status *s){*s=status;if(!polls++)s->done=0;}
void ss_usb_wait(uint64_t us){assert(us==1000);++waits;}
_Noreturn void ss_fail(uint32_t value){failure=value;longjmp(terminal,1);}
_Noreturn void ss_finish(const void *p,size_t n)
{const uint32_t *r=p;assert(n==16&&r[0]==status.stage&&r[1]==status.failed&&r[2]==1&&!r[3]);++finished;longjmp(terminal,1);}
int main(int argc,char **argv)
{
    assert(argc==4);status.stage=(unsigned)strtoul(argv[1],0,0);status.failed=(unsigned)strtoul(argv[2],0,0);status.done=1;
    if(!setjmp(terminal))radio_scan_main(0);
    if(strtoul(argv[3],0,0)){assert(failure==0x524107&&!finished);}else assert(!failure&&finished==1);
    assert(progress==2&&waits==1&&polls==2);return 0;
}
''')
    for forced in (False, True):
        binary = out / ('scan-forced' if forced else 'scan-normal')
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(out),
                        *(['-DSUNSTONE_RADIO_SCAN_FAILURE_TEST=1'] if forced else []),
                        str(ROOT / 'apps/radio-scan/main.c'), str(fixture), '-o', str(binary)], check=True)
        for stage, failed in ((7, 0), (7, 1), (6, 0)):
            subprocess.run([str(binary), str(stage), str(failed), str(int(forced and stage == 7 and not failed))], check=True)


def observer(out, compiler):
    source = (ROOT / 'apps/os/radio_scan.c').read_text()
    body = source[source.index('void os_radio_scan_step('):source.index('void ss_radio_status(')]
    fixture = out / 'scan-observer.c'
    fixture.write_text(r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
enum {RUNNING,SCANNED,DOWN,FAILED};
enum {BOARD_RADIO_SCANNED,BOARD_RADIO_DOWN,BOARD_RADIO_RETAINED,BOARD_RADIO_INVALID};
static _Atomic unsigned state,stage_seen;
static unsigned requested,starts,steps,published,noted;
struct board_radio {struct {unsigned entropy_only;} config;struct {unsigned fail_line,fail_stage;} platform,session;};
struct a733_radio_report {struct {unsigned stage;} head;};
static struct a733_radio_report report={.head.stage=7};
static int os_radio_quiesce_requested(void){return requested;}
static void os_ce_probe_start(void){++starts;}
static void os_ce_probe_step(void){++steps;}
static int os_ce_probe_done(void){return 0;}
static int os_ce_probe_failed(void){return 0;}
static const struct a733_radio_report *board_radio_report(struct board_radio *r){(void)r;return &report;}
static void publish(struct board_radio *r){(void)r;++published;}
static int os_radio_quiesce_fault(uint32_t value,const void *caller){(void)value;(void)caller;return requested;}
#define SUNSTONE_CRUMB_FAIL_SITE(x) 0
#define SUNSTONE_CRUMB_FAIL_RETURN 0
static void board_fail_note(uint32_t site,uint32_t value,const void *caller){(void)site;(void)value;(void)caller;++noted;}
''' + body + r'''
int main(void)
{
    struct board_radio r={.config.entropy_only=1};
    os_radio_scan_step(&r,BOARD_RADIO_SCANNED);assert(starts==1&&steps==1);
    requested=1;os_radio_scan_step(&r,BOARD_RADIO_RETAINED);
    assert(starts==1&&steps==1&&atomic_load(&state)==FAILED&&!noted&&!published);
    r.config.entropy_only=0;atomic_store(&state,RUNNING);
    os_radio_scan_step(&r,BOARD_RADIO_SCANNED);assert(atomic_load(&state)==SCANNED&&atomic_load(&stage_seen)==7);
    os_radio_scan_step(&r,BOARD_RADIO_DOWN);assert(atomic_load(&state)==DOWN&&published==1);
    return 0;
}
''')
    binary = out / 'scan-observer'
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (ROOT / 'apps/os/radio.c').read_text()
    body = source[source.index('static enum os_quiesce_result radio_quiesce_step'):source.index('static struct board_radio_profile profile;')]
    body += source[source.index('int os_radio_start(void)'):]
    marker = '    if(!os_radio_quiesce_requested())return 0;'
    variants = {'radio': body,
                'removed-registration': body.replace('os_quiesce_register(&radio_quiesce)', '1'),
                'exit-in-window': body.replace(marker, marker + '\n    platform_exit(6);')}
    assert body.count('os_quiesce_register(&radio_quiesce)') == 1 and body.count(marker) == 1
    compiler = shutil.which('clang-21') or shutil.which('clang')
    results = []
    for name, variant in variants.items():
        path = out / (name + '.c')
        path.write_text(PRELUDE + variant + CASES)
        binary = out / name
        command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                   '-I'+str(ROOT / 'apps/os'), '-I'+str(ROOT / 'include'), str(path), '-o', str(binary)]
        subprocess.run(command, check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        results.append({'case': name, 'returncode': run.returncode, 'stderr': run.stderr})
        assert (run.returncode == 0) == (name == 'radio'), results[-1]
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    application(out, compiler)
    observer(out, compiler)
    print('PASS radio bounded return, Bluetooth release/peer proof, CE fault, two caught mutations and scan failure injection')


if __name__ == '__main__':
    main()
