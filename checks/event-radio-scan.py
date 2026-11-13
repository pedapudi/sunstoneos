#!/usr/bin/env python3
"""Check radio scan publications and strict public work waits with real RTOS delivery."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/radio-scan/main.c', 'apps/radio-scan/manifest.json', 'apps/os/radio_scan.c',
                'apps/os/application.c', 'include/sunstone_app.h', 'runtime/deadline.c',
                'apps/board/radio.h', 'checks/event-radio-scan.c', 'checks/event-radio-scan.py']
GATE_RUNS = [['--output', '{output}']]


def function(text, name):
    import re
    match = re.search(r'^(?:static )?(?:void|int|unsigned) '+name+r'\([^;{}]*\)\s*\{', text, re.M)
    assert match, name
    end, depth = text.index('{', match.start())+1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    app = (ROOT/'apps/radio-scan/main.c').read_text()
    producer = (ROOT/'apps/os/radio_scan.c').read_text()
    runtime = (ROOT/'apps/os/application.c').read_text()
    board = (ROOT/'apps/board/radio.h').read_text()
    enum = board[board.index('enum board_radio_result {'):]
    enum = enum[:enum.index('};')+2]
    declarations = producer[producer.index('enum { RUNNING'):producer.index('static unsigned published;')]
    assert 'atomic_store_explicit(&state,' not in producer
    producer_unit = enum+'\n'+declarations+'\n'+'\n'.join(function(producer, name) for name in
        ('publish_state', 'os_radio_scan_step', 'ss_radio_status'))
    runtime_unit = '\n'.join(function(runtime, name) for name in
        ('os_wake', 'os_application_wake_main', 'os_wait_begin', 'os_wait_end',
         'application_wait_until_unless', 'application_wait_until', 'ss_wait_until', 'ss_usb_wait'))
    generated = args.output/'generated'
    subprocess.run([sys.executable, '-B', str(ROOT/'tools/sunstone-manifest.py'), 'emit', '--manifest',
                    str(ROOT/'apps/radio-scan/manifest.json'), '--output', str(generated)],
                   check=True, env=environment())
    def build(where, radio_app=app, radio_producer=producer_unit, os_wait=runtime_unit):
        where.mkdir(parents=True, exist_ok=True)
        (where/'event-radio-producer.h').write_text(radio_producer)
        (where/'event-radio-runtime.h').write_text(os_wait)
        (where/'radio-app.c').write_text(radio_app)
        return host_rtos.build(where, ['checks/event-radio-scan.c', where/'radio-app.c', 'runtime/deadline.c'],
            'event-radio-scan', includes=[generated, where])
    executable = build(args.output/'host')
    rows = []
    for case in range(20):
        result = subprocess.run([str(executable), str(case)], capture_output=True, text=True, env=environment(), timeout=20)
        (args.output/f'case-{case}.log').write_text(result.stdout+result.stderr)
        assert not result.returncode, (case, result.stdout, result.stderr)
        rows.append(case)
    mutations = [
        ('missing-publication', app, producer_unit.replace('os_application_wake_main();', '(void)0;'), runtime_unit, 1),
        ('tick-polling', app.replace('ss_wait_until(until);', 'ss_usb_wait(1000);'), producer_unit, runtime_unit, 0),
        ('strict-cap-missing', app, producer_unit, runtime_unit.replace('if(limit&&until>limit)until=limit;', '(void)0;'), 14),
        ('legacy-cap-added', app, producer_unit, runtime_unit.replace(
            'if(application_wait_until(now+(wait_us?wait_us:1))==SUNSTONE_DEADLINE_ERROR)ss_fail(0x757);',
            'ss_wait_until(now+(wait_us?wait_us:1));'), 10),
        ('private-cap-added', app, producer_unit, runtime_unit.replace(
            'static int application_wait_until_unless(uint64_t until,int (*unless)(void))\n{',
            'static int application_wait_until_unless(uint64_t until,int (*unless)(void))\n{\n'
            '    if(atomic_load(&finish_by)&&until>atomic_load(&finish_by))until=atomic_load(&finish_by);'), 7),
        ('past-deadline-fails', app, producer_unit, runtime_unit.replace(
            'if(limit&&until>limit)until=limit;', 'if(until<=a733_smp_counter()/24)ss_fail(0x757);\n    if(limit&&until>limit)until=limit;'), 16),
        ('now-deadline-armed', app, producer_unit, runtime_unit.replace(
            'if(application_wait_until(until)==', 'if(application_wait_until(until+1)=='), 17),
        ('tick-rounded-deadline', app, producer_unit, runtime_unit.replace(
            'if(application_wait_until(until)==', 'if(application_wait_until((until/1000+1)*1000)=='), 18),
    ]
    for name, radio_app, radio_producer, os_wait, case in mutations:
        assert (radio_app, radio_producer, os_wait) != (app, producer_unit, runtime_unit), name
        control = build(args.output/name, radio_app, radio_producer, os_wait)
        failed = subprocess.run([str(control), str(case)], capture_output=True, text=True, env=environment(), timeout=20)
        (args.output/(name+'.log')).write_text(failed.stdout+failed.stderr)
        assert failed.returncode, ('mutation passed', name)
    (args.output/'results.json').write_text(json.dumps(dict(passed=True, physical_access=False,
        cases=rows, negative_controls=[m[0] for m in mutations],
        scope='Actual scan app/producer/status and public/private wait code with real FreeRTOS/deadline delivery; device effects modeled.'),
        indent=2)+'\n')
    print('PASS radio scan:', len(rows), 'cases;', len(mutations), 'mutations rejected')


if __name__ == '__main__':
    main()
