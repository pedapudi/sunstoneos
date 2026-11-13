#!/usr/bin/env python3
"""Check frozen external Bluetooth wire profiles through native IRQ admission."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE, arm64_const as reg
from harness import environment, machine

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:headset}', '--output', '{output}']]
CHECK_INPUTS = ['checks/os-headset-uart-profile.py', 'checks/os-headset-uart-profile.c',
    'apps/os/os.h', 'apps/os/headset.c', 'include/a733_uart_platform.h',
    'apps/os/radio-assets.py', 'apps/bluetooth/prepare.py', 'apps/bluetooth/bluetooth.c',
    'apps/bluetooth/bluetooth.h', 'drivers/storage/storage_digest.c', 'drivers/uart/a733_uart.c',
    'drivers/net/aic8800_firmware.c', 'tools/session.py']


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(ok, message):
    if not ok:
        raise ValueError(message)


def native_layout(build, output):
    commands = json.loads((build/'compile_commands.json').read_text())
    entry = next(row for row in commands if row['file'].endswith('/apps/os/headset.c'))
    original = str(Path(entry['file']).resolve().parents[2])
    command = [word.replace(original, str(ROOT)) for word in shlex.split(entry['command'])]
    assert '-DSUNSTONE_A733_OS_HEADSET_IRQ=1' in command, 'Native IRQ headset build required'
    command = command[:command.index('-o')]
    object_ = output/'layout.o'
    command += ['-Werror', '-c', str(ROOT/'checks/os-headset-uart-profile.c'), '-o', str(object_)]
    result = subprocess.run(command, cwd=entry['directory'], env=environment(), capture_output=True, text=True)
    (output/'layout-command.json').write_text(json.dumps(command, indent=2)+'\n')
    (output/'layout-compile.log').write_text(result.stdout+result.stderr)
    assert not result.returncode, result.stderr
    with object_.open('rb') as source:
        elf = ELFFile(source)
        symbol = elf.get_section_by_name('.symtab').get_symbol_by_name('os_headset_wire_layout')[0]
        section = elf.get_section(symbol['st_shndx'])
        data = section.data()[symbol['st_value']:symbol['st_value']+symbol['st_size']]
    assert len(data)%64 == 0
    return {name.rstrip(b'\0').decode(): value for name, value in struct.iter_unpack('<56sQ', data)}


def image_parts(image):
    with image.open('rb') as source:
        elf = ELFFile(source)
        segments = [(segment['p_vaddr'], segment.data()) for segment in elf.iter_segments()
                    if segment['p_type'] == 'PT_LOAD']
        symbols = {symbol.name: symbol['st_value'] for symbol in elf.get_section_by_name('.symtab').iter_symbols()
                   if symbol['st_value'] and symbol['st_info']['type'] == 'STT_FUNC'}
    for name in ('os_headset_identity', 'os_headset_profile_valid',
                 'bluetooth_profile_valid', 'storage_digest_bytes', 'a733_uart_plan', 'os_headset_uart_irq'):
        assert name in symbols, name
    return segments, symbols


def native_identity(parts, layout, raw, uploads, copy_address=None):
    segments, symbols = parts
    cpu = machine()
    cpu.mem_map(0x41000000, 0x6000000)
    cpu.mem_map(0x50000000, 0x100000)
    for address, data in segments:
        cpu.mem_write(address, data)
    for address, data in uploads:
        cpu.mem_write(address, data)
    session, sentinel = 0x50001000, 0x500ff000
    cpu.mem_write(session, bytes(raw))
    publications, entered, returns = [], set(), []
    checkpoints = {symbols[name]: name for name in ('os_headset_identity', 'os_headset_profile_valid',
        'bluetooth_profile_valid', 'storage_digest_bytes', 'a733_uart_plan')}

    def ret(value):
        cpu.reg_write(reg.UC_ARM64_REG_X0, value)
        cpu.reg_write(reg.UC_ARM64_REG_PC, cpu.reg_read(reg.UC_ARM64_REG_LR))

    def hook(unused, address, size, context):
        if address in checkpoints:
            entered.add(checkpoints[address])
        if address == symbols['a733_cpu_coherence_publish']:
            publications.append(tuple(cpu.reg_read(r) for r in
                (reg.UC_ARM64_REG_X0, reg.UC_ARM64_REG_X1, reg.UC_ARM64_REG_X2)))
            ret(1)
        elif address == sentinel:
            returns.append(cpu.reg_read(reg.UC_ARM64_REG_X0))
            cpu.emu_stop()

    cpu.hook_add(UC_HOOK_CODE, hook)
    cpu.reg_write(reg.UC_ARM64_REG_SP, 0x500f0000)
    cpu.reg_write(reg.UC_ARM64_REG_X0, session)
    cpu.reg_write(reg.UC_ARM64_REG_X1, 100000)
    cpu.reg_write(reg.UC_ARM64_REG_LR, sentinel)
    cpu.emu_start(symbols['os_headset_identity'], 0, count=2000000)
    assert len(returns) == 1, 'Native identity did not finish'
    accepted = returns[0]
    assert {'os_headset_identity', 'os_headset_profile_valid', 'bluetooth_profile_valid',
            'storage_digest_bytes'} <= entered
    if accepted:
        assert len(publications) == 2 and 'a733_uart_plan' in entered
        copy = publications[0][0]
        if copy_address is not None:
            assert copy == copy_address
        expected = next(data for address, data in uploads if address == layout['profile.address'])
        assert bytes(cpu.mem_read(copy, len(expected))) == expected
        assert publications == [(copy, layout['profile.size'], 100000),
                                (layout['publication.address'], 0x100000, 100000)]
        assert struct.unpack('<I', cpu.mem_read(copy+layout['profile.uart.deferred_irq'], 4))[0] == 1
    else:
        assert not publications and copy_address is not None
        copy = copy_address
        assert struct.unpack('<I', cpu.mem_read(copy+layout['profile.bytes'], 4))[0] == 0
    return dict(accepted=bool(accepted), entered=sorted(entered), publications=publications, copy_address=copy)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    layout = native_layout(args.build, output)
    (output/'layout.json').write_text(json.dumps(layout, indent=2)+'\n')
    parts = image_parts(args.build/'sunstone-a733-os.elf')
    prep = load('wire_bluetooth_prepare', ROOT/'apps/bluetooth/prepare.py')
    assets = load('wire_bluetooth_assets', ROOT/'apps/os/radio-assets.py')
    session_tool = load('wire_session', ROOT/'tools/session.py')
    cache, evidence = {}, {}

    def frozen(name, data):
        path = output/name
        path.write_bytes(bytes(data))
        cache[str(path)] = bytes(data)
        return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest()}

    def document(name, value):
        return frozen(name, json.dumps(value, sort_keys=True).encode())

    for key in ('wiring', 'sid', 'transport', 'scheduling', 'stop'):
        evidence[key] = document(key+'.json', {'role': key,
            'scope': 'Modeled external profile; no physical authority'})
    models = {'models': [{'bytes': size, 'sha256': hashlib.sha256(bytes([i])).hexdigest()}
                        for i, size in enumerate((64, 128))]}
    profile = bytearray(layout['profile.size'])

    def put(member, format_, *values):
        struct.pack_into('<'+format_, profile, layout['profile.'+member], *values)

    put('magic', 'Q', layout['profile.magic_value'])
    put('version', 'I', 4)
    put('bytes', 'I', len(profile))
    for member, value in [('topology', 91), ('sid_authority', 7), ('radio_authority', 13),
                          ('uart_authority', 17), ('wake_authority', 19)]:
        put(member, 'Q', value)
    for key, member in [('wiring', 'wiring_evidence'), ('sid', 'sid_evidence'),
                        ('transport', 'fresh_transport_evidence'), ('scheduling', 'headset.scheduling_evidence'),
                        ('stop', 'bluetooth_stop.wiring_evidence')]:
        put(member, '32s', bytes.fromhex(evidence[key]['sha256']))
    put('pm_latch', '2Q2I', 67, 71, 63, 4)
    put('radio', '5Q4I6I', 73, 0, 0, 0, 91, 0, 2, 3, 2, 1, 2, 3, 400000000, 200000000, 24000000)
    put('wake.authority', 'Q', 19)
    put('uart.owner', 'Q', 79)
    put('uart.route', 'I', 3)
    put('uart.format', '8I', 1, 8, 2, 0, 1, 24000000, 1500000, 0)
    put('uart.deferred_irq', 'I', 1)
    put('identity', '2I4B', 3 << 16, 0, 3, 0, 0, 1)
    firmware = Path.home()/'.cache/sunstoneos/sources/radio/firmware/aic8800d80'
    payloads = []
    for i, (name, address) in enumerate(zip(('fw_adid_8800d80_u02.bin', 'fw_patch_8800d80_u02.bin',
            'fw_patch_table_8800d80_u02.bin', 'fmacfw_8800d80_u02.bin'),
            (0x43010000, 0x43011000, 0x43018000, 0x43020000))):
        path = (firmware/name).resolve()
        data = path.read_bytes()
        cache[str(path)] = data
        payloads.append({'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'address': address})
        struct.pack_into('<Q', profile, layout['profile.firmware_address']+8*i, address)
        struct.pack_into('<Q', profile, layout['profile.firmware_bytes']+8*i, len(data))
    for member, value in [('session_owner', 83), ('runtime_lease_us', 10000000), ('renew_margin_us', 1000000),
                          ('command_timeout_us', 500000), ('frame_timeout_us', 100000), ('binding_timeout_us', 1000000)]:
        put(member, 'Q', value)
    put('headset', '2I6s', 2, 0, b'peer01')
    put('headset.minimum_security', '2I', 2, 1)
    put('headset.scheduling_authority', 'Q', 97)
    put('headset.receive_latency_us', '2Q', 5000, 500000)
    put('headset.samples', 'I4H2IBBH', 120, 128, 160, 18, 17, 100000, 2000000, 0, 0, 0)
    put('recognition', '2Q64s', *(model['bytes'] for model in models['models']),
        b''.join(bytes.fromhex(model['sha256']) for model in models['models']))
    put('bluetooth_stop', '2Q', 101, 91)
    put('bluetooth_stop.expected', '4BI', 1, 0, 1, 1, 0)

    values = {}
    for name, format_ in session_tool.PREFIX:
        if name.startswith('_'):
            continue
        values[name] = bytes(session_tool.width(format_)) if format_.endswith('s') else (
            [0]*session_tool.count(format_) if session_tool.count(format_) > 1 else 0)
    values.update(magic=session_tool.SESSION_MAGIC, version=session_tool.SESSION_VERSION, bytes=592,
        epoch=31, generation=37, lease=41, topology=91, sid_authority=7, workload=0,
        qualification=bytes([3])*32, wiring=bytes([1])*32, memory=bytes([2])*32, recovery=bytes([3])*32,
        cold_master=2, memory_authority=3, recovery_authority=4,
        pmic_owner=103, usb0_owner=107, typec_owner=109, usb2_owner=113,
        usb_supply_uv=[3300000, 3300000, 0, 0, 0], usb0_cold_reference=1,
        fabric=[1, 2, 3], fabric_hz=[400000000, 200000000, 24000000])
    context = {'identity': 'host-fixture', 'port': 'host-only'}
    proof = {'schema': 'a733-radio-sdio-id-v1', **context}
    for group, keys in assets.FACTS.items():
        fact = {'schema': 'a733-radio-sdio-id-v1-'+group, **context,
                **{key: True for key in keys}, 'scope': 'Host model; no physical authority'}
        if group == 'topology':
            fact['rails'] = {'vbat': 'dcdc1', 'pg': 'bldo5', 'pm': 'cldo1', 'vddio': 'bldo5'}
        if group == 'pins':
            for key in ('expected', 'desired'):
                offset = layout['profile.radio.'+key]
                fact[key] = [list(profile[offset+3*i:offset+3*i+3]) for i in range(8)]
            fact['pm_latch'] = 4
        if group == 'startup':
            fact.update(fabric=values['fabric'], fabric_hz=values['fabric_hz'], peri0_400mhz_branch_verified=True)
        proof[group] = document(group+'.json', fact)
    platform = document('platform.json', proof)
    outcomes, copy_address = {}, None
    for case in ('deferred-one', 'stale-hash', 'rebound-identity', 'deferred-zero', 'short-budget'):
        data = bytearray(profile)
        if case == 'deferred-zero':
            struct.pack_into('<I', data, layout['profile.uart.deferred_irq'], 0)
        if case in ('stale-hash', 'rebound-identity'):
            # Keep every semantic predicate valid, so native refusal requires
            # the actual full-profile digest comparison.
            struct.pack_into('<Q', data, layout['profile.headset.response_timeout_us'], 499999)
        if case == 'short-budget':
            struct.pack_into('<Q', data, layout['profile.headset.receive_latency_us'], 3414)
        identity_bytes = profile if case == 'stale-hash' else data
        raw = session_tool.pack('prefix', {**values, 'sid_evidence': hashlib.sha256(identity_bytes).digest()})
        assert prep.board.session_check(raw) == 0
        external = frozen(case+'-profile.bin', data)
        doc = {'schema': assets.SCHEMA, **context, 'epoch': 31, 'generation': 37, 'lease': 41,
               'headset_profile': external, 'platform': platform, 'payloads': payloads,
               **{'bluetooth_'+key: ref for key, ref in evidence.items()}}
        record = {'radio_assets': document(case+'-assets.json', doc), **context}
        # The OS asset adapter validates the same externally supplied bytes and
        # returns their immutable upload plan. It never patches the profile.
        host_accepted = True
        try:
            prep.profile_check(data, raw, workloads=(0,), model_pin=models)
            plan = assets.validate(record, raw+bytes(216), {'radio': False, 'headset': True, 'msbc': False},
                                   cache, prep.board, prep, require, frozen=True, model_pin=models)
            uploads = [(address, cache[path]) for path, address in plan]
            assert next(blob for address, blob in uploads if address == layout['profile.address']) == bytes(data)
        except (ValueError, SystemExit):
            assert case == 'stale-hash'
            host_accepted = False
            # A malformed session cannot reach a board. Exercise native refusal
            # independently with exactly the stale identity and source bytes.
            uploads = [(layout['profile.address'], bytes(data))]
        result = native_identity(parts, layout, raw, uploads, copy_address)
        if copy_address is None:
            copy_address = result['copy_address']
        assert host_accepted == (case != 'stale-hash'), case
        assert result['accepted'] == (case in ('deferred-one', 'rebound-identity')), case
        frozen(case+'-session.bin', raw)
        outcomes[case] = dict(host_accepted=host_accepted, **result)
    result = dict(passed=True, cases=outcomes, physical_access=False,
        image_sha256=hashlib.sha256((args.build/'sunstone-a733-os.elf').read_bytes()).hexdigest(),
        deferred_irq_offset=layout['profile.uart.deferred_irq'],
        source_sha256={name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in CHECK_INPUTS},
        scope='Modeled external immutable profile; actual host validators/upload plan and native image identity/validators/SHA/UART plan. Coherence publication and secure-ID availability modeled; no generic typed headset producer or physical qualification.')
    (output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS five external wire/native IRQ admission cases; authenticated bytes preserved')


if __name__ == '__main__':
    main()
