#!/usr/bin/env python3
"""Run linked resident NPU wait/deadline code and cold-authority checks.

The existing NPU model supplies RTOS/provider effects. No inference or physical
reset is executed. Cold authority mutations isolate the producer's semantic
checks after its independently checked reference hashes.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from harness import Image, refused
from unicorn import UC_HOOK_CODE

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


native = module('resident_npu_native', ROOT / 'checks/os-npu.py')
card = module('resident_npu_card', ROOT / 'apps/os/card.py')


class Wait(native.Machine):
    def model_api(self, cpu, name, args):
        if (self.variant == 'open-wait' and name == 'os_npu_open') or (
                self.variant == 'stop-wait' and name == 'os_npu_stopped'):
            self.api_calls.append(name)
            self.ret(0)
        else:
            super().model_api(cpu, name, args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'fixture', 'entry', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--late-stop-only', action='store_true', help='Check the live terminal proof on the final combined image')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = Image(args.image, layout_names=native.NAMES + ('npu_service',))
    raw = (args.entry / 'runtime-session.bin').read_bytes()
    results = []
    if args.late_stop_only:
        late_stop(image, raw, results)
        record(args, results)
        return
    machine = Wait(image, raw)
    machine.put(machine.board + 160, 900000000)
    clocks = machine.shared + machine.L['a733_native_usb']['clocks']
    counter = machine.shared + machine.L['a733_native_usb']['counter']
    assert machine.call('a733_clock_leases_init', clocks, image.address('a733_counter_clock_now'), counter) == 1
    assert machine.call('os_npu_start') == 1
    assert machine.get(machine.n + machine.nL['end']) == machine.now + 60000000
    results.append({'case': 'resident-constructor-sixty-second-cap', 'deadline': machine.now + 60000000})

    seen = image.address('seen', unit='apps/os/services.c') + 7 * 8
    assert machine.call('os_npu_progress') == 1
    heartbeat = machine.get(seen)
    assert heartbeat == machine.now | 1
    machine.now = machine.get(machine.n + machine.nL['end'])
    assert machine.call('os_npu_progress') == 0xffffffff and machine.failed
    assert machine.get(seen) == heartbeat
    results.append({'case': 'deadline-expiry-fails-before-heartbeat'})

    for variant in ('open-wait', 'stop-wait', 'normal-refusal'):
        machine = Wait(image, raw)
        machine.variant = variant
        machine.api = {'load': 0, 'closed': 0}
        machine.put(machine.n + machine.nL['started'], 1, 4)
        machine.put(machine.n + machine.nL['end'], machine.now + 50000)
        machine.put(machine.n + machine.nL['gate'], ((machine.now + 1000000) << 3) | 1)
        machine.call('os_npu_client_task', 0, budget=1000000)
        published = image.address('published', unit='apps/os/npu-client.c')
        layout = machine.L['os_npu_report']
        fields = {key: machine.get(published + layout[key], 4) for key in ('finished', 'stopped', 'retained')}
        failed = variant != 'normal-refusal'
        assert machine.suspended and machine.failed == failed
        assert fields == {'finished': 1, 'stopped': int(not failed), 'retained': int(failed)}, fields
        assert machine.call('os_npu_complete') == int(not failed)
        if failed:
            assert machine.now == 51000 and machine.get(seen) == 26001
        results.append({'case': variant, 'report': fields, 'api_calls': machine.api_calls,
                        'last_heartbeat': machine.get(seen), 'ended_us': machine.now})

    # A stopped flag alone, an unpublished report, or a retained report cannot
    # exempt the NPU service from the supervisor's health requirement.
    for finished, stopped, retained, expected in ((0, 1, 0, 0), (1, 0, 0, 0), (1, 1, 1, 0), (1, 1, 0, 1)):
        for name, value in (('finished', finished), ('stopped', stopped), ('retained', retained)):
            machine.put(published + layout[name], value, 4)
        assert machine.call('os_npu_complete') == expected
    results.append({'case': 'terminal-exemption-requires-published-stopped-unretained'})

    policy_path = args.fixture / 'bundle/cold-policy.json'
    policy = json.loads(policy_path.read_text())
    session = (args.fixture / 'bundle/slot/os-session.bin').read_bytes()
    frozen = {name: (policy_path.parent / ref['path']).read_bytes() for name, ref in policy['evidence'].items()}
    for name, data in frozen.items():
        assert hashlib.sha256(data).hexdigest() == policy['evidence'][name]['sha256']
    card.npu_policy(policy, session, policy_path.parent, dict(frozen))
    for name, field, value, reason in (
            ('clock', 'mhz', 1008, 'vendor top point'),
            ('clock', 'sid_dvfs_word', 0x010000, 'vendor top point'),
            ('power', 'supply_uv', 900000, '800 mV'),
            ('supervision', 'work_us', 60000001, 'finite-work supervision'),
            ('supervision', 'one_attempt_per_boot', False, 'finite-work supervision'),
            ('supervision', 'supervisor', 999, 'finite-work supervision'),
            ('admission', 'ddr_sha256', '0' * 64, 'another pool'),
            ('ddr', 'remote_writers_excluded', False, 'DDR reservation'),
            ('clock', 'image_sha256', '0' * 64, 'bound to this board'),
            ('power', 'sid_authority', 999, 'bound to this board'),
            ('supervision', 'schema', 'a733-npu-supervision-v1', 'condition set')):
        changed = dict(frozen)
        fact = json.loads(changed['npu_' + name])
        fact[field] = value
        changed['npu_' + name] = json.dumps(fact).encode()
        refused(lambda: card.npu_policy(policy, session, policy_path.parent, changed), reason)
        results.append({'case': 'cold-' + name + '-' + field})
    changed = dict(frozen)
    fact = json.loads(changed['npu_clock'])
    ref = fact['conditions'].pop('cold-boot-cross-pll-disturbance-qualified')
    fact['conditions']['supervised-cross-pll-disturbance-risk'] = ref
    changed['npu_clock'] = json.dumps(fact).encode()
    refused(lambda: card.npu_policy(policy, session, policy_path.parent, changed), 'condition set')
    results.append({'case': 'supervised-clock-authority-cannot-admit-resident'})
    record(args, results)


def record(args, results):
    value = {'passed': True, 'physical_access': False, 'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
              'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'cases': results}
    (args.output / 'result.json').write_text(json.dumps(value, indent=2) + '\n')
    print('PASS', len(results), 'resident NPU cases')



def late_stop(image, raw, results):
    machine = Wait(image, raw)
    machine.now = 6001000
    service = machine.rt + machine.L['npu_runtime']['service']
    layout = machine.L['npu_service']
    published = image.address('published', unit='apps/os/npu-client.c')
    report = machine.L['os_npu_report']
    for key in ('started', 'stopped', 'active'):
        machine.put(machine.n + machine.nL[key], 1, 4)
    for key in ('initialized', 'irq_bound', 'irq_stopped'):
        machine.put(service + layout[key], 1, 4)
    for key in ('finished', 'stopped'):
        machine.put(published + report[key], 1, 4)
    machine.put(image.address('instance', unit='drivers/npu/npu_service.c'), service)
    seen = image.address('seen', unit='apps/os/services.c')
    for i in range(8):
        machine.put(seen + i * 8, machine.now if i != 7 else 1001)
    before = bytes(machine.u.mem_read(published, native.report.REPORT_BYTES))
    assert machine.call('os_npu_complete') == 1
    assert machine.call('os_services_healthy', machine.now) == 1
    notification = next(name for name in ('vTaskGenericNotifyGiveFromISR', 'vTaskNotifyGiveFromISR') if name in image.symbols)
    address = image.address(notification)
    notified = []
    def notify(cpu, pc, size, unused):
        notified.append(True)
        machine.ret()
    machine.u.hook_add(UC_HOOK_CODE, notify, begin=address, end=address)
    machine.call('npu_service_irq_notify_from_isr', service, 1, 0x500e0000)
    assert notified and not machine.get(service + layout['irq_stopped'], 4)
    assert machine.get(service + layout['irq_pending'], 4)
    assert machine.call('os_npu_complete') == 0
    assert machine.call('os_services_healthy', machine.now) == 0
    assert bytes(machine.u.mem_read(published, len(before))) == before
    results.append({'case': 'actual-late-stop-isr-revokes-health-with-immutable-result'})
    # Consuming pending status cannot hide the worker's persistent fault.
    machine.put(service + layout['irq_pending'], 0, 4)
    machine.put(service + layout['irq_stopped'], 1, 4)
    machine.put(machine.rt + machine.L['npu_runtime']['failed'], 1, 4)
    assert machine.call('os_npu_complete') == 0
    assert machine.call('os_services_healthy', machine.now) == 0
    results.append({'case': 'atomic-runtime-fault-keeps-terminal-exemption-revoked'})
    # Before activation, the thermal refusal needs no IRQ-stop receipt.
    machine.put(machine.rt + machine.L['npu_runtime']['failed'], 0, 4)
    machine.put(machine.n + machine.nL['active'], 0, 4)
    machine.put(service + layout['irq_stopped'], 0, 4)
    assert machine.call('os_npu_complete') == 1
    assert machine.call('os_services_healthy', machine.now) == 1
    results.append({'case': 'pre-activation-refusal-retains-valid-terminal-exemption'})


if __name__ == '__main__':
    main()
