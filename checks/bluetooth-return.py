#!/usr/bin/env python3
"""Check application return composition and UART native lock error propagation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from harness import CACHE, CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = CACHE / 'sources/bluetooth-hci/include/net/bluetooth/hci.h'
    raw = source.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == '9da36dad169c8c02ba3a676a3c884527e4efb7d152b975b131f33aaa59347bb7'
    text = raw.decode()
    reference = [text[:text.index('#ifndef __HCI_H')], '#include <stdint.h>',
                 'typedef uint8_t __u8; typedef uint16_t __le16;',
                 'typedef struct { uint8_t b[6]; } bdaddr_t;',
                 '#define __packed __attribute__((packed))']
    macros = ('HCI_COMMAND_PKT HCI_ACLDATA_PKT HCI_SCODATA_PKT HCI_EVENT_PKT HCI_ISODATA_PKT '
              'HCI_OP_RESET HCI_OP_READ_LOCAL_VERSION HCI_OP_READ_LOCAL_COMMANDS HCI_OP_READ_LOCAL_FEATURES '
              'HCI_OP_READ_BUFFER_SIZE HCI_OP_READ_BD_ADDR HCI_EV_CMD_COMPLETE HCI_EV_CMD_STATUS HCI_EV_DISCONN_COMPLETE')
    for name in macros.split():
        reference.append(re.search(r'^#define\s+' + name + r'\s+0x[0-9a-fA-F]+', text, re.M)[0])
    structs = ('hci_command_hdr hci_event_hdr hci_acl_hdr hci_sco_hdr hci_iso_hdr hci_ev_cmd_complete '
               'hci_ev_cmd_status hci_rp_read_local_version hci_rp_read_local_commands '
               'hci_rp_read_local_features hci_rp_read_buffer_size hci_rp_read_bd_addr')
    for name in structs.split():
        reference.append(re.search(r'struct ' + name + r' \{.*?\} __packed;', text, re.S)[0])
    (out / 'bt-ref.h').write_text('\n'.join(reference) + '\n')
    kernel = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    tcp = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
    flags = [CLANG, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
             '-ffunction-sections', '-fdata-sections', '-DSUNSTONE_AIC8800_RSN=1',
             '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-Inetwork/include', '-Iinclude',
             '-I' + str(kernel / 'include'), '-I' + str(kernel / 'portable/GCC/ARM_AARCH64_SRE'),
             '-I' + str(tcp / 'include'), '-I' + str(tcp / 'portable/Compiler/GCC'), '-I' + str(out)]
    commands = []

    def run(label, command):
        command = list(map(str, command))
        commands.append(command)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        result = subprocess.run(command, cwd=ROOT, env=environment(), capture_output=True, text=True)
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, label + ': see ' + str(out / (label + '.log'))
        return result.stdout

    sources = ['checks/bluetooth-return.c', 'drivers/uart/a733_uart.c', 'drivers/bluetooth/bt_h4.c',
               'drivers/bluetooth/bt_hci.c', 'drivers/storage/storage_digest.c']
    binary = out / 'return'
    run('compile', flags + sources + ['-Wl,--gc-sections', '-o', binary])
    results = [json.loads(run('case-' + str(case), [binary, case])) for case in range(11)]
    # Execute the production adapter body. The shared-lock fixture separately
    # exercises semaphore contention, dynamic deadline publication and expiry.
    native = (ROOT / 'drivers/uart/a733_uart_platform_native.c').read_text()
    enter = re.search(r'static int enter\(void \*cookie,uint64_t deadline\)\n\{.*?\n\}', native, re.S)[0]
    access = re.search(r'static int access_allowed\(.*?\n\}', native, re.S)[0]
    read = re.search(r'static int read32\(.*?\n\}', native, re.S)[0]
    fixture = '''#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
enum { A733_PLATFORM_REGISTER_LOCK };
typedef void *TaskHandle_t;
struct locks {atomic_uint retained;void *mutex[1];};
struct a733_uart_platform_native { struct locks *locks;unsigned port;void *owner_task; };
static struct locks shared;
static int active=1,answer,calls,live=1,mmio;
static int current(void *p){return p&&active;}
static int a733_platform_lock_step(void *p,unsigned lock,uint64_t d)
{assert(p==&shared&&lock==A733_PLATFORM_REGISTER_LOCK&&d==3000000);++calls;return answer;}
static int a733_platform_lock_live(struct locks *p){assert(p==&shared);return live;}
static int a733_uart_platform_native_address(unsigned p,uintptr_t a,unsigned w)
{assert(p==1);(void)a;(void)w;return 1;}
static TaskHandle_t xSemaphoreGetMutexHolder(void *m){(void)m;return (void *)1;}
static uint32_t a733_mmio_read32_dsb_sy_both(uintptr_t a){(void)a;++mmio;return 0;}
'''
    fixture += enter + access + read + '''
int main(void){struct a733_uart_platform_native n={&shared,1,(void *)1};
 for(answer=-1;answer<=1;++answer)assert(enter(&n,3000000)==answer);
 active=0;assert(enter(&n,3000000)==-1&&calls==3);active=1;
 uint32_t value;assert(read32(&n,0x02501000,&value)==1&&mmio==1);
 assert(read32(&n,0x02002000,&value)==1&&mmio==2);
 live=0;assert(read32(&n,0x02501000,&value)==0&&mmio==2);
 assert(read32(&n,0x02002000,&value)==0&&mmio==2);return 0;}
'''
    (out / 'native-enter.c').write_text(fixture)
    run('native-enter-compile', flags + [out / 'native-enter.c', '-o', out / 'native-enter'])
    run('native-enter-run', [out / 'native-enter'])
    paths = [ROOT / name for name in sources + ['checks/bluetooth-application.c',
             'apps/bluetooth/bluetooth.c', 'apps/bluetooth/bluetooth.h',
             'drivers/uart/a733_uart_platform_native.c', 'checks/bluetooth-return.py']]
    paths.append(source)
    record = dict(passed=True, physical_access=False, cases=results, native_error_results=[-1, 0, 1],
                  scope='Actual application/UART/H4/HCI; modeled module and platform readback completion.',
                  inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths})
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS Bluetooth application return composition and native UART tri-state lock propagation')


if __name__ == '__main__':
    main()
