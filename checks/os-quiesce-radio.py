#!/usr/bin/env python3
"""Exercise radio return coordination, retained faults and source mutations."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/radio.c', 'apps/os/quiesce.c', 'apps/os/quiesce.h',
                'apps/os/radio_scan.c', 'apps/board/main.c', 'apps/os/main.patch',
                'apps/radio-scan/main.c', 'cmake/a733-os.cmake']
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


def owner_loop(out, compiler, main_patch):
    """Execute one real generated owner pass, ending at its delay or park."""
    generated = out / 'board-os-main.c'
    subprocess.run(['patch', '--silent', '-o', str(generated),
                    str(ROOT / 'apps/board/main.c'), str(main_patch)], check=True)
    (out / 'owner-main.patch').write_bytes(main_patch.read_bytes())
    inputs = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (ROOT / 'apps/board/main.c', main_patch, generated)}
    (out / 'owner-loop-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    source = generated.read_text()
    body = source[source.index('static void radio_owner('):source.index('static void radio_client(')]
    before = '''        if(os_radio_quiesce_requested()){
            parked=os_radio_quiesce_poll(&radio,0)==2;
            goto radio_return_wait;
        }
'''
    after = '''#endif
            if(r==BOARD_RADIO_RETAINED||r==BOARD_RADIO_INVALID){'''
    assert body.count(before) == body.count(after) == 1
    mutant = body.replace(before, '').replace(after, '''#endif
            int returning=os_radio_quiesce_poll(&radio,(int)r);
            if(returning==2){parked=1;goto radio_return_wait;}
            if(r==BOARD_RADIO_RETAINED||r==BOARD_RADIO_INVALID){''')
    prelude = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#define SUNSTONE_A733_OS 1
#define SUNSTONE_A733_OS_RADIO_SCAN 1
#define OS_GRANT_HEADSET 64u
#define OS_SERVICE_RADIO 5u
#define SUNSTONE_CRUMB_HERE() ((const void *)1)
enum board_radio_result {BOARD_RADIO_PENDING,BOARD_RADIO_SCANNED,BOARD_RADIO_RETAINED,BOARD_RADIO_INVALID};
static struct {unsigned platform;} radio;
static struct {_Atomic unsigned radio_result,watchdog_state;struct {unsigned provider;} typec;} board;
static struct {struct {_Atomic unsigned pmic_ready;} shared;} providers;
static jmp_buf pass_end;
static unsigned requested,terminal,headset=1,headset_pending=1,live=1,quiesce_result=1;
static unsigned board_polls,quiesce_polls,module_polls,scan_steps,delays,parks,progress,credentials,external,revokes;
static int os_radio_quiesce_parked(void){return terminal;}
static int os_radio_quiesce_requested(void){return requested;}
static int os_radio_quiesce_poll(void *r,int result)
{assert(r==&radio);(void)result;++quiesce_polls;return requested?(int)quiesce_result:0;}
static void module_step(void){++module_polls;}
static int module_pending(void){return headset_pending;}
static void (*os_module_poll)(void)=module_step;
static int (*os_headset_quiesce_pending)(void)=module_pending;
static int os_granted(unsigned grant){assert(grant==OS_GRANT_HEADSET);return headset;}
static void os_wifi_credential_poll(void){++credentials;}
static int board_live(void *unused){assert(!unused);return live;}
static int board_radio_external_failure(void *r){assert(r==&radio);++external;return 0;}
static enum board_radio_result board_radio_poll(void *r){assert(r==&radio);++board_polls;return BOARD_RADIO_SCANNED;}
static void os_radio_scan_step(void *r,int result){assert(r==&radio&&result==BOARD_RADIO_SCANNED);++scan_steps;}
static int a733_radio_platform_clean(void *p){assert(p==&radio.platform);return 1;}
static int os_radio_quiesce_fault(uint32_t value,const void *caller){(void)value;(void)caller;return requested;}
static void a733_typec_revoke(void *p){assert(p==&board.typec.provider);++revokes;}
static void board_progress(unsigned service){assert(service==OS_SERVICE_RADIO);++progress;}
#define BOARD_PROGRESS(service) board_progress(service)
static _Noreturn void park(void){++parks;longjmp(pass_end,1);}
static void vTaskDelay(unsigned ticks){assert(ticks==1);++delays;longjmp(pass_end,1);}
'''
    cases = r'''
static void pass(unsigned request,unsigned done,unsigned pending,unsigned result,unsigned admitted)
{
    requested=request;terminal=done;headset_pending=pending;quiesce_result=result;live=admitted;
    board_polls=quiesce_polls=module_polls=scan_steps=delays=parks=progress=credentials=external=revokes=0;
    atomic_store(&board.radio_result,0);atomic_store(&providers.shared.pmic_ready,1);
    if(!setjmp(pass_end))radio_owner(NULL);
    assert(credentials==1&&!external&&!revokes);
    assert(module_polls==headset);
    unsigned stopped=done||(request&&result==2);
    assert(parks==(stopped&&!pending)&&delays==!parks&&progress==!parks);
    assert(board_polls==(!request&&!done)&&scan_steps==board_polls);
    assert(quiesce_polls==(request&&!done));
}
int main(void)
{
    pass(1,0,1,1,1); /* Pending return submits no ordinary radio work. */
    pass(1,0,1,2,1); /* Radio done, Bluetooth still needs its owner pass. */
    pass(1,1,1,2,1); /* Subsequent passes continue Bluetooth without another stop. */
    pass(1,1,0,2,1); /* Park only after the headset becomes terminal. */
    pass(1,0,0,2,1); /* Immediate terminal result still polls the shared module. */
    pass(1,0,1,1,0); /* Return bypasses the old external-failure path too. */
    pass(0,0,1,1,1); /* Normal admission still polls radio and its observer. */
    headset=0;pass(0,0,1,1,1);
    return 0;
}
'''
    results = []
    for name, variant in [('owner-loop', body), ('owner-loop-post-poll-mutation', mutant)]:
        fixture = out / (name + '.c')
        fixture.write_text(prelude + variant + cases)
        binary = out / name
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', str(fixture), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        results.append({'case': name, 'returncode': run.returncode, 'stderr': run.stderr})
        assert (run.returncode == 0) == (name == 'owner-loop'), results[-1]
    (out / 'owner-loop-results.json').write_text(json.dumps(results, indent=2) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--main-patch', type=Path, default=ROOT / 'apps/os/main.patch',
                   help='Shared owner-loop patch to validate before integration; defaults to this source tree')
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
    owner_loop(out, compiler, args.main_patch.resolve())
    print('PASS radio return, Bluetooth peer proof, CE fault, three caught mutations, scan injection and actual owner loop')


if __name__ == '__main__':
    main()
