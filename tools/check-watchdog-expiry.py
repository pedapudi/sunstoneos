#!/usr/bin/env python3
"""Run one explicitly selected watchdog phase with protected ROM recovery."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
IMAGE_HASH = 'fe0735093d01cdf115cc9e605c2e18f64d2bd65042526d66c33348148e388993'
FEL_HASH = '4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e'
ROM_HASH = 'a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa'
REGISTERS = (0x02050014, 0x02050010, 0x02050018, 0x070501c0,
             0x070501c4, 0x070501d4, 0x070501d8, 0x07090108)
REGIONS = (('image', 0x52000, 0x800), ('mailbox', 0x52800, 64),
           ('stack', 0x53000, 0x1000), ('scratch', 0x71000, 128))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def hash_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_prior(record, phase, identity, port):
    require(record.get('complete') is True and record.get('phase') == phase and
            record.get('image_sha256') == IMAGE_HASH and record.get('port') == port and
            record.get('initial_identity') == identity,
            'Arm-disarm evidence does not cover this image and live board instance.')


def restore_registers(transaction, before, observed):
    """Restore only recognized ownership after successful ROM re-enumeration."""
    require(not observed[0] & 1, 'Returned watchdog remains enabled; no restoration attempted.')
    require(observed[5:7] == before[5:7], 'Alternate dispatch changed; restoration refused.')
    require(observed[3] in (0, 0xfa50392f) and
            observed[4] in (before[4], 0x20), 'Unexpected redirect owner after reset.')
    # Each phase verifies the ordering dependency before proceeding to the next.
    transaction('writel', '0x070501c0', '0', 'readl', '0x070501c0', expected_words=[0])
    transaction('writel', '0x070501c4', hex(before[4]),
                'readl', '0x070501c4', expected_words=[before[4]])
    for address, value in zip(REGISTERS[:3], before[:3]):
        transaction('writel', hex(address), hex(0x16aa0000 | value))
    transaction('writel', '0x07090108', hex(before[7]))


def run_phase(phase, transaction, evidence, image_path, image, rom, before,
              wait_return, record, save):
    """Execute a reviewed phase using injected transport and reconnect adapters."""
    nonce = secrets.randbits(32) or 1
    command = 1 if phase == 'arm-disarm' else 2
    request = struct.pack('<16I', 0x57445850, command, nonce, nonce ^ 0xffffffff, *([0] * 12))
    request_path = evidence / 'request.bin'
    request_path.write_bytes(request)
    transaction('write', '0x52000', image_path, 'write', '0x52800', request_path,
                'read', '0x52000', len(image), evidence / 'uploaded.bin',
                'read', '0x52800', 64, evidence / 'uploaded-request.bin')
    require((evidence / 'uploaded.bin').read_bytes() == image and
            (evidence / 'uploaded-request.bin').read_bytes() == request,
            'Upload verification failed; execution refused.')
    # Recheck electrical assertion immediately before either enabling operation.
    transaction(*[argument for _ in range(4) for argument in ('readl', '0x03000024')], low_input=True)
    record['execution_intent'] = {'command': command, 'nonce': nonce, 'watchdog_seconds': 4}
    save()  # Durable intent precedes the single execution request.
    started = time.monotonic()
    transaction('exe', '0x52000')
    if phase == 'arm-disarm':
        transaction('read', '0x52800', 64, evidence / 'result.bin')
        result = struct.unpack('<16I', (evidence / 'result.bin').read_bytes())
        record['result'] = list(result)
        save()
        require(result[:4] == struct.unpack('<4I', request[:16]) and
                result[4:7] == (0x57525032, 0, 5) and result[8:10] == (0, 0) and
                result[7] & 0xf1 == 0x41, 'Enable/disarm did not report complete restoration.')
    else:
        wait_return(started)
        elapsed = time.monotonic() - started
        record['return_seconds'] = elapsed
        save()
        require(2 <= elapsed <= 20, 'ROM return lies outside the declared 2..20 second observation window.')
        # No old DDR or SRAM result is read as reset evidence. The ROM clears SRAM.
        record['reset_evidence_limit'] = ('New ROM instance and elapsed host time; '
                                        'no retained final-stage marker or qualified reset-cause register.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=('arm-disarm', 'expiry'), required=True)
    parser.add_argument('--confirm-bridge-stable', action='store_true', required=True,
                        help='Coordinator confirms BOOT/GND bridge stays attached through reset.')
    parser.add_argument('--input-evidence', type=Path, required=True)
    parser.add_argument('--arm-disarm-evidence', type=Path)
    parser.add_argument('--fel', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--image', type=Path, default=CACHE / 'build/watchdog-expiry/probe.bin')
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    require(args.phase != 'expiry' or args.arm_disarm_evidence is not None,
            'Expiry requires the successful arm-disarm session record.')
    image, rom = args.image.read_bytes(), args.rom.read_bytes()
    for path, expected in ((args.image, IMAGE_HASH), (args.rom, ROM_HASH), (args.fel, FEL_HASH)):
        require(hash_file(path) == expected, f'Input differs from reviewed bytes: {path}')
    input_evidence = json.loads(args.input_evidence.read_text())
    require(input_evidence.get('complete') is True and input_evidence.get('asserted') is True and
            input_evidence.get('workspace_restored') is True and
            input_evidence.get('rom_sha256') == ROM_HASH,
            'Successful physical low-input inspection evidence is required.')
    access = load_module('watchdog_access', ROOT / 'tools/usb-access.py')
    sys.path.insert(0, str(Path.home() / 'git/mimus/tools'))
    import usb_guard
    usb_guard.check_health()
    args.evidence.mkdir(parents=True, exist_ok=False)
    record = {'phase': args.phase, 'image_sha256': IMAGE_HASH, 'rom_sha256': ROM_HASH,
              'fel_sha256': FEL_HASH, 'runner_sha256': hash_file(Path(__file__)),
              'input_evidence_sha256': hash_file(args.input_evidence),
              'complete': False, 'transactions': [], 'bridge_confirmed_stable': True}
    node, identity, port_config = None, None, None
    transport_failed = False
    workspace_started = False

    def save():
        temporary = args.evidence / 'session.tmp'
        with temporary.open('w') as stream:
            json.dump(record, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(args.evidence / 'session.json')
        descriptor = os.open(args.evidence, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def transaction(*arguments, expected_words=None, low_input=False):
        nonlocal transport_failed
        require(not transport_failed, 'Transport failed; all further requests are disabled.')
        entry = {'arguments': [str(argument) for argument in arguments]}
        record['transactions'].append(entry)
        save()
        try:
            require(access.identity(node) == identity, 'Device instance changed unexpectedly.')
            command = [str(args.fel), '-d', f'{int(node.parent.name)}:{int(node.name)}',
                       *map(str, arguments)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=15)
            entry.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
            require(result.returncode == 0, 'FEL request failed: ' + result.stderr.strip())
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            transport_failed = True
            entry['failure'] = str(error)
            save()
            raise
        save()
        if expected_words is not None or low_input:
            words = [int(word, 16) for word in result.stdout.split()]
            if expected_words is not None:
                require(words == expected_words, 'Register verification failed.')
            if low_input:
                require(len(words) == 4 and all(0 <= word <= 0xffffffff and not word & 0x100
                                               for word in words), 'BOOT input is not consistently low.')
        return result.stdout

    def read_registers():
        words = [int(word, 16) for word in transaction(*[argument for address in REGISTERS
                  for argument in ('readl', hex(address))]).split()]
        require(len(words) == len(REGISTERS), 'Unexpected register response length.')
        return [word & 0xffff if index < 3 else word for index, word in enumerate(words)]

    def save_sram(prefix):
        transaction(*[argument for name, address, length in REGIONS for argument in
                      ('read', hex(address), length, args.evidence / f'{prefix}-{name}.bin')])
        for name, _, _ in REGIONS:
            with (args.evidence / f'{prefix}-{name}.bin').open('rb') as stream:
                os.fsync(stream.fileno())

    def restore_sram(prefix):
        transaction(*[argument for name, address, _ in REGIONS for argument in
                      ('write', hex(address), args.evidence / f'{prefix}-{name}.bin')])
        transaction(*[argument for name, address, length in REGIONS for argument in
                      ('read', hex(address), length, args.evidence / f'{prefix}-{name}-restored.bin')])
        require(all((args.evidence / f'{prefix}-{name}.bin').read_bytes() ==
                    (args.evidence / f'{prefix}-{name}-restored.bin').read_bytes()
                    for name, _, _ in REGIONS), 'SRAM restoration verification failed.')

    def inspect_rom(prefix):
        for name, address, length in (('reset', 0, 0x2f4), ('input', 0x10100, 0x100)):
            path = args.evidence / f'{prefix}-{name}.bin'
            transaction('memmove', '0x52000', hex(address), length, 'read', '0x52000', length, path)
            require(path.read_bytes() == rom[address:address + length], 'Physical recovery ROM differs.')

    def wait_return(started):
        nonlocal node, identity
        previous = identity
        while time.monotonic() - started < 20:
            candidate = access.session_node(port_config)
            if candidate is not None:
                try:
                    candidate_identity = access.identity(candidate)
                    if candidate_identity != previous and os.access(candidate, os.R_OK | os.W_OK):
                        access.require_session(candidate)
                        access.require_rom(candidate)
                    else:
                        candidate = None
                except FileNotFoundError:
                    candidate = None
                if candidate is not None:
                    node, identity = candidate, candidate_identity
                    record.update(returned_node=str(node), returned_identity=identity)
                    save()
                    require('soc=00001903(A733)' in transaction('version'), 'Returned target is not A733.')
                    return
            time.sleep(0.05)
        raise ValueError('No distinct accessible ROM instance in 20 seconds; no reset or request retry.')

    try:
        with open('/tmp/mimus-board.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            node = access.discover()
            access.require_session(node)
            access.require_rom(node)
            identity = access.identity(node)
            port = access.target(node)
            port_config = {'port': port.name, 'sysfs': str(port.resolve())}
            require(input_evidence.get('identity') == identity and input_evidence.get('port') == port.name,
                    'Input evidence belongs to another board instance.')
            if args.phase == 'expiry':
                prior = json.loads(args.arm_disarm_evidence.read_text())
                validate_prior(prior, 'arm-disarm', identity, port.name)
                record['arm_disarm_evidence_sha256'] = hash_file(args.arm_disarm_evidence)
            record.update(initial_node=str(node), initial_identity=identity, port=port.name)
            save()
            require('soc=00001903(A733)' in transaction('version'), 'Unexpected SoC.')
            save_sram('before')
            workspace_started = True
            inspect_rom('before')
            before = read_registers()
            record['registers_before'] = before
            save()
            require(not before[0] & 1 and before[3] == 0 and before[5] == 0,
                    'Watchdog or ROM dispatch has an existing owner.')
            if args.phase == 'expiry':
                with usb_guard.reenumeration('sunstoneos-a733', 'protected watchdog expiry and ROM return', 1):
                    run_phase(args.phase, transaction, args.evidence, args.image, image, rom,
                              before, wait_return, record, save)
                save_sram('returned')
                inspect_rom('returned')
                observed = read_registers()
                record['registers_returned'] = observed
                save()
                restore_registers(transaction, before, observed)
            else:
                with usb_guard.reenumeration('sunstoneos-a733', 'watchdog enable/disarm with protected failure return', 1) as event:
                    run_phase(args.phase, transaction, args.evidence, args.image, image, rom,
                              before, wait_return, record, save)
                    event['count'] = 0
            after = read_registers()
            record['registers_restored'] = after
            save()
            require(after == before, 'Saved register state was not restored.')
            restore_sram('returned' if args.phase == 'expiry' else 'before')
            record.update(complete=True, workspace_restored=True,
                          ddr_initialized=False if args.phase == 'expiry' else None)
            save()
            print(f'PASS observed {args.phase}; evidence: {args.evidence}')
            if args.phase == 'expiry':
                print('DDR remains uninitialized. Reset-cause attribution and bus-stall recovery are not qualified.')
    except (OSError, ValueError, RuntimeError, struct.error, subprocess.TimeoutExpired) as error:
        record.update(failure=str(error), transport_failed=transport_failed)
        save()
        if identity is not None and (workspace_started or transport_failed):
            access.quarantine_instance(node, 'Watchdog diagnostic stopped; inspect host backups before further access.', identity)
        raise SystemExit(f'Stopped: {error}\nEvidence: {args.evidence}') from error


if __name__ == '__main__':
    main()
