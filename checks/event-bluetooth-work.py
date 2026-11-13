#!/usr/bin/env python3
"""Check exact Bluetooth progress snapshots, RX age and owner publications."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import host_rtos
from harness import environment
ROOT = Path(__file__).resolve().parents[1]

CHECK_INPUTS = ['apps/bluetooth/bluetooth.c', 'apps/bluetooth/bluetooth.h',
    'apps/bluetooth/headset.c', 'apps/bluetooth/headset.h', 'apps/bluetooth/work-state.h',
    'include/bt_h4.h', 'include/bt_hci.h', 'include/bt_acl.h', 'include/bt_l2cap.h',
    'include/bt_rfcomm.h', 'include/bt_sco.h', 'include/bt_sdp.h', 'include/bt_sdp_server.h',
    'include/bt_hfp_ag.h', 'include/bt_security.h', 'include/bt_security_port.h',
    'drivers/bluetooth/bt_acl.c', 'drivers/bluetooth/bt_l2cap.c', 'drivers/bluetooth/bt_sdp_server.c',
    'checks/event-bluetooth-work.c']
GATE_RUNS = [['--output', '{output}']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    security = args.output/'security'
    subprocess.run([sys.executable, '-B', str(ROOT/'apps/bluetooth/security-source.py'), '--source',
                    str(Path.home()/'.cache/sunstoneos/sources/bluetooth-security-zephyr-v4.2.1'),
                    '--output', str(security)], check=True, env=environment())
    network = Path.home()/'.cache/sunstoneos/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
    includes = [ROOT/'apps/bluetooth', ROOT/'network/include', network/'include',
                network/'portable/Compiler/GCC', security]
    wrappers = []
    for name in ('bt_hci', 'bt_l2cap'):
        wrapper = args.output/(name+'-lower.c')
        wrapper.write_text('#define '+name+'_step unused_'+name+'_step\n#include '+
                           json.dumps(str(ROOT/'drivers/bluetooth'/(name+'.c')))+'\n')
        wrappers.append(wrapper)
    sources = ['checks/event-bluetooth-work.c', 'apps/bluetooth/bluetooth.c',
               'drivers/bluetooth/bt_h4.c', 'drivers/uart/a733_uart.c',
               'drivers/bluetooth/bt_acl.c', 'drivers/bluetooth/bt_sdp_server.c', *wrappers]
    defines = ['SUNSTONE_BLUETOOTH_HEADSET=1', 'SUNSTONE_AIC8800_RSN=1']
    def build(where, selected=sources, extra=()):
        return host_rtos.build(where, selected, 'event-bluetooth-work', defines=defines+list(extra),
            includes=includes, flags=['-ffunction-sections', '-fdata-sections'], link=['-Wl,--gc-sections'])
    executable = build(args.output/'host')
    result = host_rtos.run(executable)
    (args.output/'run.log').write_text(result.stdout+result.stderr)
    assert not result.returncode, result.stdout+result.stderr
    mutations = [
        ('missing-owner-hint', 'xTaskNotifyGive(a->owner);', '(void)0;'),
        ('fairness-is-progress', 'out->stop_phase=context.stop_phase;',
         'out->stop_phase=context.stop_phase+voice.server.rotation;'),
        ('owned-age-forgotten', 'r->owner==BT_H4_OWNED&&r->serial>seen', 'r->owner==BT_H4_OWNED&&(r->serial>seen&&0)'),
    ]
    for name, old, new in mutations:
        mutant = host_rtos.mutated(args.output, 'apps/bluetooth/headset.c', old, new, name+'.c')
        control = build(args.output/name, extra=['HEADSET_SOURCE='+json.dumps(str(mutant))])
        failed = host_rtos.run(control)
        (args.output/(name+'.log')).write_text(failed.stdout+failed.stderr)
        assert failed.returncode, ('mutation passed', name)
    (args.output/'results.json').write_text(json.dumps(dict(passed=True, physical_access=False,
        snapshot_bytes=7232, snapshot_pair_bytes=14464, negative_controls=[m[0] for m in mutations],
        scope='Actual component queries, publications, ACL credit block, L2CAP enqueue and SDP server; lower passes modeled.'),
        indent=2)+'\n')
    print(result.stdout, end='')
    print('PASS three targeted negative controls')


if __name__ == '__main__':
    main()
