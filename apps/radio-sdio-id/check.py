#!/usr/bin/env python3
"""Execute the native application with modeled providers/transport; no hardware access."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'checks'))
import harness
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_PC,
    UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE, UC_ARM64_REG_CPACR_EL1)
spec = importlib.util.spec_from_file_location('radio_id_build', Path(__file__).with_name('build.py'))
producer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(producer)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixtures(out):
    """Synthetic assertions exercise the producer. They authorize no board."""
    record = {'schema': producer.SCHEMA, 'identity': 'software-model-not-a-board',
              'port': 'model', 'lifetime': 'model-only', 'four_bit': 1}
    facts = {}
    for group, names in producer.FACTS.items():
        facts[group] = {**{k: record[k] for k in ('identity', 'port', 'lifetime')},
                        'schema': producer.SCHEMA + '-' + group, 'reviewed': True,
                        'physical_access': False, **{k: True for k in names}}
    facts['topology'].update(rails={'vbat': 'dcdc1', 'pg': 'dcdc8', 'pm': 'dcdc8', 'vddio': 'dcdc8'},
                             routing_evidence='Synthetic model only; no fitted board claim.')
    facts['pins'].update(pm_latch=4, latch_origin='verified-writer-history',
        latch_evidence='Synthetic model only; not a measured latch.', expected=[[0, 0, 0]] * 8,
        desired=[[2, 0, 0]] * 6 + [[0, 0, 0], [1, 0, 0]])
    facts['startup'].update(fabric=[0, 0x80000000, 0x80000000], fabric_hz=[24000000] * 3,
                            peri0_400mhz_branch_verified=True)
    facts['bootstrap']['pad'] = 1
    path = out / 'model-authority.json'

    def write(r, f):
        for group, value in f.items():
            p = out / ('model-' + group + '.json')
            p.write_text(json.dumps(value, sort_keys=True))
            r[group] = {'path': str(p), 'sha256': sha(p)}
        path.write_text(json.dumps(r, sort_keys=True))

    write(record, facts)
    source, _, _ = producer.profile_source(path)
    mutations = [
        ('topology', 'fitted_board_routes_verified', False, 'required fact absent'),
        ('pins', 'latch_origin', 'input-data-read', 'cannot establish output latch'),
        ('pins', 'pm_latch', 64, 'complete PM latch'),
        ('pins', 'lifetime', 'another-model-lifetime', 'board/lifetime mismatch'),
        ('startup', 'peri0_400mhz_branch_verified', False, 'parent evidence absent'),
        ('bootstrap', 'write_40500058_40_authorized', False, 'required fact absent'),
        ('recovery', 'whole_board_power_removal', False, 'required fact absent')]
    results = []
    for group, key, value, reason in mutations:
        changed = copy.deepcopy(facts); changed[group][key] = value
        write(copy.deepcopy(record), changed)
        harness.refused(lambda: producer.profile_source(path), reason)
        results.append(group + ':' + key)
    write(record, facts)
    record.pop('pins'); path.write_text(json.dumps(record))
    harness.refused(lambda: producer.profile_source(path), 'missing pins evidence')
    results.append('missing-pins')
    write(record, facts)
    record['topology']['sha256'] = '0' * 64; path.write_text(json.dumps(record))
    harness.refused(lambda: producer.profile_source(path), 'topology evidence hash')
    results.append('changed-evidence')
    write(record, facts)
    return source, results


def run(image, case):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0x40000000, 0x04000000)
    for a, data in image.parts:
        if data: u.mem_write(a, data)
    u.reg_write(UC_ARM64_REG_SP, 0x43ff0000)
    u.reg_write(UC_ARM64_REG_PSTATE, 5)
    u.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    unit = '/apps/radio-sdio-id/main.c'
    L = image.layouts
    radio = image.address('radio', unit=unit)
    bb = image.address('bb_power')
    trans = image.address('transport', unit=unit)
    host = radio + L['a733_radio_platform']['host']
    card = radio + L['a733_radio_platform']['card']
    profile = image.address('radio_id_profile')
    state = {'case': case, 'time': 100, 'exit': None, 'calls': [], 'messages': [], 'deadlines': [], 'retains': 0}
    deadline = 1500100
    def put(a, v, n=4): u.mem_write(a, v.to_bytes(n, 'little'))
    def field(a, typ, key, v, n=4): put(a + L[typ][key], v, n)
    def get(a, n=8): return int.from_bytes(u.mem_read(a, n), 'little')
    def ret(v=1):
        u.reg_write(UC_ARM64_REG_X0, v)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))
    def string(a):
        b = bytearray()
        while (v := u.mem_read(a + len(b), 1)[0]): b.append(v)
        return b.decode()
    put(0x42000000 + 64, 2400, 8)
    if case == 'missing-authority': field(profile, 'radio_id_profile', 'authority', 0, 8)
    if case == 'bad-latch':
        field(profile + L['radio_id_profile']['pm_latch'], 'a733_pin_latch_handoff', 'known_mask', 3)
    if case == 'one-bit': field(profile, 'radio_id_profile', 'four_bit', 0)
    hooks = ['platform_init', 'platform_puts', 'platform_exit', 'platform_panic',
             'bb_power_snapshot_init', 'bb_power_wait_ready', 'bb_power_now', 'vTaskDelay', 'vTaskStartScheduler',
             'a733_radio_platform_native_init', 'a733_radio_platform_init', 'a733_radio_platform_four_bit',
             'a733_radio_platform_poll', 'a733_radio_platform_borrow', 'a733_radio_platform_live',
             'a733_radio_platform_retain', 'aic8800_transport_init', 'aic8800_transport_setup',
             'aic8800_transport_read_identity', 'aic8800_transport_poll', 'aarch64_device_translate']
    if case == 'constructor':
        hooks = ['platform_init', 'platform_puts', 'platform_exit', 'platform_panic',
                 'xTaskCreateStatic', 'xQueueCreateMutexStatic', 'vTaskStartScheduler']
    addresses = {image.address(n): n for n in hooks}
    continuation = 0x43fe0000
    u.mem_write(continuation, bytes.fromhex('c0035fd6'))
    def code(cpu, address, size, cookie):
        if case == 'constructor':
            word = int.from_bytes(u.mem_read(address, 4), 'little')
            kind, reg = word & 0xffffffe0, word & 31
            if kind in (0xd5384240, 0xd53be000, 0xd53be020):
                value = 4 if kind == 0xd5384240 else 24000000 if kind == 0xd53be000 else state['time'] * 24
                u.reg_write(UC_ARM64_REG_X0 + reg, value)
                u.reg_write(UC_ARM64_REG_PC, address + 4)
                return
            if address not in addresses: return
        if address == continuation:
            result = u.reg_read(UC_ARM64_REG_X0)
            state['admission'] = result
            u.reg_write(UC_ARM64_REG_LR, state['poll_return'])
            ret(2 if result == 1 else 3)
            return
        name = addresses[address]
        args = [u.reg_read(UC_ARM64_REG_X0 + i) for i in range(8)]
        state['calls'].append(name)
        if state['retains']:
            assert name in ('platform_puts', 'platform_exit'), name
        if name == 'xQueueCreateMutexStatic':
            state['mutexes'] = state.get('mutexes', 0) + 1
            assert args[1]
            ret(args[1]); return
        if name == 'xTaskCreateStatic':
            state.setdefault('tasks', []).append(args[0])
            assert args[6]
            ret(args[6]); return
        if case == 'constructor' and name == 'vTaskStartScheduler':
            pins = bb + L['bb_power']['pins']
            pmic = bb + L['bb_power']['pmic']
            handoff = pins + L['a733_pins']['handoff']
            auth = handoff + L['a733_pin_handoff']['authorized']
            assert [get(auth + b * 4, 4) for b in (6, 11, 12)] == [63, 3, 3]
            latch_bytes = L['radio_id_profile']['platform'] - L['radio_id_profile']['pm_latch']
            latch = handoff + L['a733_pin_handoff']['latch'] + latch_bytes
            assert get(latch + L['a733_pin_latch_handoff']['known_mask'], 4) == 63
            assert get(latch + L['a733_pin_latch_handoff']['value'], 4) == 4
            assert get(pmic + L['a733_pmic']['pins']) == pins
            assert get(radio + L['a733_radio_platform']['pins']) == pins
            assert get(radio + L['a733_radio_platform']['pmic']) == pmic
            assert get(radio + L['a733_radio_platform']['clocks']) == bb + L['bb_power']['clocks']
            assert get(radio + L['a733_radio_platform']['rails']) == bb + L['bb_power']['rails']
            assert get(radio + L['a733_radio_platform']['initialized'], 4) == 1
            assert get(radio + L['a733_radio_platform']['four_bit'], 4) == 1
            assert get(host + L['sunxi_sdio']['initialized'], 4) == 1
            native = image.address('native', unit=unit)
            assert get(native + L['a733_radio_platform_native']['locks']) == bb + L['bb_power']['locks']
            assert get(native + L['a733_radio_platform_native']['owner_task']) == get(bb + L['bb_power']['client'])
            assert state['mutexes'] == 4 and len(state['tasks']) == 2
            assert state['tasks'][1] == image.address('radio_id_client')
            state['constructed'] = True
            u.emu_stop(); return
        if name == 'platform_exit':
            state['exit'] = args[0]; u.emu_stop()
        elif name == 'platform_panic':
            raise AssertionError(string(args[0]))
        elif name == 'platform_puts':
            state['messages'].append(string(args[0])); ret()
        elif name == 'bb_power_snapshot_init':
            assert args[0] == image.address('radio_id_client')
            handoff = bb + L['bb_power']['handoff']
            auth = handoff + L['a733_pin_handoff']['authorized']
            assert get(auth + 6 * 4, 4) == 0x3f and get(auth + 12 * 4, 4) == 3
            # The unchanged shared constructor sets PL0/1; radio does not replace it.
            put(auth + 11 * 4, 3)
            field(bb, 'bb_power', 'deadline', deadline, 8)
            field(bb, 'bb_power', 'client', 0x43fd0000, 8)
            ret()
        elif name == 'a733_radio_platform_native_init':
            assert args[1] == bb + L['bb_power']['locks'] and args[2] == 0x43fd0000
            assert args[3] == image.address('admission', unit=unit) and args[4] == radio
            ret()
        elif name == 'a733_radio_platform_init':
            assert args[0] == radio
            assert args[4:] == [bb + L['bb_power'][k] for k in ('clocks', 'pins', 'rails', 'pmic')]
            layout = L['a733_radio_platform_config']
            state['config'] = args[1]
            n = L['radio_id_profile']['transport'] - L['radio_id_profile']['platform']
            u.mem_write(radio + L['a733_radio_platform']['config'], bytes(u.mem_read(args[1], n)))
            assert get(args[1] + layout['deadline_us']) == get(args[1] + layout['lifetime_deadline_us']) == deadline
            field(radio, 'a733_radio_platform', 'initialized', 1)
            field(host, 'sunxi_sdio', 'wide', int(case not in ('one-bit', 'fallback')))
            field(host, 'sunxi_sdio', 'card_hz', 400000 if case in ('one-bit', 'fallback') else 25000000)
            ret()
        elif name == 'vTaskStartScheduler':
            u.reg_write(UC_ARM64_REG_PC, image.address('radio_id_client'))
        elif name == 'bb_power_now': ret(state['time'])
        elif name == 'vTaskDelay':
            assert args[0] == 1
            state['time'] += 1000; ret()
        elif name == 'a733_radio_platform_poll':
            token = radio + L['a733_radio_platform']['clock']
            field(token, 'a733_clock_token', 'id', 1, 8)
            field(token, 'a733_clock_token', 'owner', 7, 8)
            state['poll_return'] = u.reg_read(UC_ARM64_REG_LR)
            u.reg_write(UC_ARM64_REG_X0, radio)
            u.reg_write(UC_ARM64_REG_X0 + 1, radio + L['a733_radio_platform']['config'])
            u.reg_write(UC_ARM64_REG_X0 + 2, token)
            u.reg_write(UC_ARM64_REG_LR, continuation)
            u.reg_write(UC_ARM64_REG_PC, image.address('admission', unit=unit))
        elif name == 'aarch64_device_translate':
            u.mem_write(args[2], struct.pack('<QII', args[0] + (4096 if case == 'mapping' else 0), 0, 3)); ret()
        elif name == 'a733_radio_platform_borrow':
            assert args[0] == radio and args[1] == 7
            put(args[3], card + (8 if case == 'wrong-card' else 0), 8); ret()
        elif name == 'a733_radio_platform_live':
            state['deadlines'].append(args[2]); ret(2)
        elif name == 'aic8800_transport_init':
            assert args[:2] == [trans, card]
            assert args[2] == profile + L['radio_id_profile']['transport']; ret()
        elif name == 'aic8800_transport_setup':
            state['deadlines'].append(args[1]); ret(0 if case == 'setup-refused' else 1)
        elif name == 'aic8800_transport_read_identity':
            assert args[1] == profile + L['radio_id_profile']['bootstrap']
            state['deadlines'].append(args[2]); state['identity'] = True
            ret(0 if case == 'identity-refused' else 1)
        elif name == 'aic8800_transport_poll':
            if case == 'deadline': ret(); return
            if not state.get('identity'):
                field(trans, 'aic8800_sdio_transport', 'state', 2)
            else:
                if case == 'late-completion': state['time'] = deadline + 1
                field(trans, 'aic8800_sdio_transport', 'state', 5 if case == 'transport-fault' else 4)
                field(trans, 'aic8800_sdio_transport', 'completed_transfers', 6, 8)
                put(trans + L['aic8800_sdio_transport']['have'], 0x101, 2)
                identity = trans + L['aic8800_sdio_transport']['identity']
                put(identity, 0x03000000); put(identity + 4, 1)
            ret()
        elif name == 'a733_radio_platform_retain':
            state['retains'] += 1
            field(radio, 'a733_radio_platform', 'fault', 1)
            field(radio, 'a733_radio_platform', 'retained', 1); ret(3)
        else: ret()
    if case == 'constructor':
        u.hook_add(UC_HOOK_CODE, code)
    else:
        for at in [*addresses, continuation]: u.hook_add(UC_HOOK_CODE, code, begin=at, end=at)
    u.emu_start(image.address('kernel_main'), 0, count=8000000)
    if case == 'constructor':
        assert state.get('constructed') and state['exit'] is None
        return state
    refusal = case in ('missing-authority', 'bad-latch')
    success = case in ('one-bit', 'four-bit')
    assert state['exit'] == (7 if refusal else 0 if success else 1), state
    if refusal:
        assert 'bb_power_snapshot_init' not in state['calls'] and not state['retains']
    else:
        assert state['retains'] == 1
        values = struct.unpack('<16Q', bytes(u.mem_read(0x42000080, 128)))
        assert values[2] == (0 if success else 1) and values[15] == 1
        state['result'] = dict(zip(('magic', 'authority', 'outcome', 'polls', 'transport_error', 'host_error',
            'host_status', 'host_irq', 'host_bytes', 'wide', 'card_hz', 'transfers', 'chip', 'subrevision',
            'elapsed_us', 'retained'), values))
        if success: assert values[11:14] == (6, 0x03000000, 1)
        if case == 'deadline': assert state['time'] == deadline and values[3] <= 1500
        assert all(d == deadline for d in state['deadlines'])
    calls = state.pop('calls')
    state['call_counts'] = {n: calls.count(n) for n in set(calls)}
    return state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cases', nargs='+')
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    source, producer_cases = fixtures(a.output)
    image_path = a.evidence / 'build/probe.elf'
    inputs = json.loads((a.evidence / 'input-identities.json').read_text())
    assert inputs['refusal_image'] and sha(image_path) == inputs['elf_sha256']
    commands = json.loads((a.evidence / 'commands.json').read_text())
    compile_command = next(c for c in commands if str(a.evidence / 'profile.c') in c)
    model_source, model_object = a.output / 'profile.c', a.output / 'profile.obj'
    model_source.write_text(source)
    command = [str(model_source) if x == str(a.evidence / 'profile.c') else
               str(model_object) if x == compile_command[-1] else x for x in compile_command]
    command[command.index('-MF') + 1] = str(a.output / 'profile.d')
    subprocess.run(command, check=True)
    link = next(c for c in commands if '-nostdlib' in c)
    model_image = a.output / 'model.elf'
    link = [str(model_object) if x == compile_command[-1] else str(model_image) if x == str(image_path)
            else '-Wl,-Map,' + str(a.output / 'model.map') if x.startswith('-Wl,-Map,') else x for x in link]
    subprocess.run(link, check=True)
    image = harness.Image(model_image, layout_names=['radio_id_profile', 'a733_radio_platform',
        'a733_radio_platform_config', 'aic8800_sdio_transport', 'bb_power', 'sunxi_sdio',
        'a733_pin_handoff', 'a733_pin_latch_handoff', 'a733_clock_token', 'a733_pmic',
        'a733_pins', 'a733_radio_platform_native'])
    cases = a.cases or ['constructor', 'missing-authority', 'bad-latch', 'one-bit', 'four-bit', 'fallback', 'mapping',
                        'wrong-card', 'setup-refused', 'identity-refused', 'transport-fault', 'deadline', 'late-completion']
    result = {'physical_access': False, 'software_only_profile': True, 'execution_admitted': False,
              'scope': 'native application and admission; modeled providers, SDIO platform and transport effects',
              'native_elf_sha256': sha(image_path), 'model_elf_sha256': sha(model_image),
              'producer_cases': producer_cases, 'compile': command, 'link': link, 'cases': []}
    for case in cases:
        try:
            result['cases'].append(run(image, case))
        except Exception as error:
            result.update(failed_case=case, error=repr(error))
            (a.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
            raise
        (a.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(cases)} native application cases and {len(producer_cases)} producer refusals passed; no physical access')


if __name__ == '__main__': main()
