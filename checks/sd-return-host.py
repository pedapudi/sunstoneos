#!/usr/bin/env python3
"""Bind the new return decoder to an actual linked producer log, without USB."""
import argparse
import copy
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    tool = load('sd_return_host', ROOT / 'tools/storage-read-returning.py')
    raw = (a.native / 'nominal.input.bin').read_bytes(); log = (a.native / 'nominal.log').read_text()
    value = tool.decode_log(log, raw, 0); assert value['complete'] and value['words']['SD_RESTORED'] == 1
    refusals = []
    for name, text, reason in (
        ('missing receipt', log.replace('SD_RESTORED=0x0000000000000001\n', ''), 'missing SD restoration receipt'),
        ('duplicate receipt', log+'SD_RESTORED=1\n', 'duplicate result word'),
        ('false receipt', log.replace('SD_RESTORED=0x0000000000000001', 'SD_RESTORED=0'), 'missing SD restoration receipt'),
        ('unknown version', log.replace('SD_RETURN_VERSION=0x0000000000000001', 'SD_RETURN_VERSION=2'), 'unsupported return version')):
        refusals.append(dict(name=name, rejection=harness.refused(lambda: tool.decode_log(text, raw, 0), reason)))
    assert not tool.decode_log(log, raw, 1)['complete']
    refusals.append(dict(name='missing status', rejection=harness.refused(
        lambda: tool.decode_log(log, raw), 'missing successful return status')))
    # Earlier frozen capture format remains decodable, without granting it the
    # restoration receipt of this new image.
    legacy = '\n'.join(x for x in log.splitlines() if not x.startswith(('SD_RETURN_VERSION=', 'SD_RESTORED=')))+'\n'
    assert tool.decode_log(legacy, raw, 0)['complete']
    outcomes = json.loads((ROOT / 'apps/storage-read-returning/controlled.json').read_text())['outcomes']
    assert {o['name'] for o in outcomes} == {'sd-input', 'sd-providers', 'sd-return-admission'}
    assert all(o['status'] == 1 for o in outcomes)
    # The linked task fixture starts below kernel_main. Model only its two
    # identity lines, whose format comes from main.c; retain the actual task
    # and monitor output for all restoration and card facts.
    given = tool.parse_input(raw)
    strict_log = log + f"SD_PROFILE=0x{given['profile']:016x}\nSD_GENERATION=0x{given['generation']:016x}\n"
    strict = tool.decode_log(strict_log, raw, 0)
    terminal = dict(magic=0x534b4552, stage=5, status=0)
    reference = copy.deepcopy(strict)
    assert tool.validate_successful_return(strict, terminal, raw, reference)['normal_successor_eligible']
    strict_refusals = []
    for word in ('SD_PROFILE', 'SD_GENERATION', 'SD_RETURN_VERSION', 'SD_PMIC_PARKED', 'SD_CLOCK_OFF', 'SD_RESTORED'):
        for absent in (False, True):
            bad = copy.deepcopy(strict)
            if absent: bad['words'].pop(word)
            else: bad['words'][word] = 0
            reason = 'return identity' if word in ('SD_PROFILE', 'SD_GENERATION') else 'restored-return receipts'
            strict_refusals.append(harness.refused(
                lambda: tool.validate_successful_return(bad, terminal, raw, reference), reason))
    for key in ('magic', 'stage', 'status'):
        strict_refusals.append(harness.refused(lambda: tool.validate_successful_return(
            strict, terminal | {key: 9}, raw, reference), 'unsuccessful terminal'))
    for key, replacement in (('bus_width', 1), ('timing', 3), ('card_hz', 150000000), ('signal_1v8', True)):
        bad = copy.deepcopy(strict); bad['card'][key] = replacement
        strict_refusals.append(harness.refused(lambda: tool.validate_successful_return(
            bad, terminal, raw, reference), 'card mode'))
    for key in ('cid', 'crc32'):
        bad_reference = copy.deepcopy(reference)
        if key == 'cid': bad_reference['card'][key] = '0'*32
        else: bad_reference['reads'][0][key] ^= 1
        strict_refusals.append(harness.refused(lambda: tool.validate_successful_return(
            strict, terminal, raw, bad_reference), 'same card' if key == 'cid' else 'CRC mismatch'))
    run = a.output / 'modeled-run'; run.mkdir()
    (run / 'kernel.log').write_text(strict_log)
    session = dict(complete=True, launch_attempted=True, transport_failed=False, automatic_access=True,
                   mode='kernel', returned_node='/dev/bus/usb/003/099', result=terminal,
                   model='not physical; exact image identity belongs to frozen runner')
    (run / 'session.json').write_text(json.dumps(session))
    input_path = a.output / 'strict.input.bin'; input_path.write_bytes(raw)
    provenance = dict(return_policy=tool.RESTORED_RETURN, input_sha256=tool.digest(input_path),
                      approval_sha256='a'*64, request_sha256='b'*64)
    input_path.with_suffix('.json').write_text(json.dumps(provenance))
    reference_path = a.output / 'modeled-reference.json'; reference_path.write_text(json.dumps(reference))
    decode_argv = ['decode', '--run', str(run), '--input', str(input_path), '--reference', str(reference_path),
                   '--output', str(a.output / 'strict-result.json'), '--require-restored-return']
    with contextlib.redirect_stdout(io.StringIO()): assert tool.main(decode_argv) == 0
    assert json.loads((a.output / 'strict-result.json').read_text())['successful_return']['normal_successor_eligible']
    for key, replacement in (('return_policy', tool.POWER_REMOVAL), ('input_sha256', '0'*64)):
        input_path.with_suffix('.json').write_text(json.dumps(provenance | {key: replacement}))
        strict_refusals.append(harness.refused(lambda: tool.main(decode_argv), 'does not authorize restored return'))
    input_path.with_suffix('.json').unlink()
    try: tool.main(decode_argv)
    except FileNotFoundError: strict_refusals.append('missing producer sidecar')
    else: raise AssertionError('accepted missing producer sidecar')
    input_path.with_suffix('.json').write_text(json.dumps(provenance))
    session['transport_failed'] = True; (run / 'session.json').write_text(json.dumps(session))
    strict_refusals.append(harness.refused(lambda: tool.main(decode_argv), 'successful ROM return'))
    fixture = load('sd_return_policy_fixture', ROOT / 'checks/storage-read-returning-host.py')
    ddr, survey = fixture.lifetime(a.output, 'policy')
    request = a.output / 'request.json'
    request_value = {'fields': {'A': {'initial': ['authorize_one_cmd0_initialization'],
                                    'recovery': sorted(tool.LEGACY_RECOVERY)}}}
    request.write_text(json.dumps(request_value))
    fields = {'initial': {'authorize_one_cmd0_initialization': True},
              'recovery': dict.fromkeys(tool.RESTORED_RECOVERY, True)}
    approval = fixture.approval(a.output / 'approval.json', 'A', fields)
    tool.sign(approval, ddr, survey, 'MODEL', tool.RESTORED_RETURN, request)
    tool.approved(approval, request, ddr, survey, 'A', tool.RESTORED_RETURN)
    strict_refusals.append(harness.refused(lambda: tool.approved(
        approval, request, ddr, survey, 'A'), 'another return policy'))
    fabric = a.output / 'modeled-fabric'; fabric.mkdir()
    ddr_value = json.loads((ddr / 'session.json').read_text())
    (fabric / 'session.json').write_text(json.dumps(dict(schema=tool.READ_SCHEMA, complete=True,
        scratch_restored=True, port='3-3', node=ddr_value['device'], identity=ddr_value['node_identity'],
        words={f'0x{address:08x}': word for address, word in zip(tool.FABRIC, (0x03000002, 0xc3000000, 0x84000000))},
        model='not physical')))
    board = a.output / 'modeled-board.json'
    board.write_text(json.dumps({k: v for k, v in fixture.CONFIG.items() if not k.startswith('fabric')}))
    input_argv = ['input', '--approval', str(approval), '--request', str(request), '--ddr', str(ddr),
                  '--survey', str(survey), '--fabric-read', str(fabric), '--config', str(board),
                  '--output', str(a.output / 'policy.input.bin'), '--profile', '2', '--plan', 'first-read',
                  '--return-policy', tool.RESTORED_RETURN]
    with contextlib.redirect_stdout(io.StringIO()): assert tool.main(input_argv) == 0
    made = json.loads((a.output / 'policy.input.json').read_text())
    assert made['return_policy'] == tool.RESTORED_RETURN and made['request_sha256'] == tool.digest(request)
    assert tool.parse_input((a.output / 'policy.input.bin').read_bytes())['plan'] == tool.FIRST_READ_PLAN

    value = json.loads(approval.read_text())
    for word in tool.RESTORED_RECOVERY:
        bad = copy.deepcopy(value); bad['fields']['recovery'][word] = False
        rejected = a.output / 'rejected-approval.json'; rejected.write_text(json.dumps(bad))
        strict_refusals.append(harness.refused(lambda: tool.approved(
            rejected, request, ddr, survey, 'A', tool.RESTORED_RETURN), 'not approved'))
    request.write_text(json.dumps(request_value) + ' ')
    strict_refusals.append(harness.refused(lambda: tool.approved(
        approval, request, ddr, survey, 'A', tool.RESTORED_RETURN), 'another request'))
    request_value['fields']['A']['recovery'].append('another_hardware_obligation')
    request.write_text(json.dumps(request_value))
    strict_refusals.append(harness.refused(lambda: tool.recovery_fields(
        request, 'A', tool.RESTORED_RETURN), 'exact reviewed'))
    # Exercise actual argparse/main default with explicitly modeled host
    # lifetime authority; this fixture does not sign a physical approval.
    config = {'pmic_pins': [[2,1,1]]*2, 'expected_pins': [[0,0,0]]*7,
              'desired_pins': [[2,1,1]]*6+[[0,1,1]]}
    cfg = a.output / 'config.json'; cfg.write_text(json.dumps(config))
    tool.lifetime = lambda *args: None
    selected = []
    tool.approved = lambda *args: selected.append(args[-1])
    tool.fabric_read = lambda *args: (([0,1<<31,1<<31], [24000000]*3), [0,1<<31,1<<31])
    tool.digest = lambda path: 'model'
    argv = ['input']
    for name in ('approval','request','ddr','survey','fabric-read'): argv += ['--'+name, str(a.output / name)]
    argv += ['--config', str(cfg), '--output', str(a.output / 'default.input.bin')]
    strict_refusals.append(harness.refused(lambda: tool.main(argv + ['--return-policy', tool.RESTORED_RETURN]),
                                           '--profile 2 --plan first-read'))
    strict_refusals.append(harness.refused(lambda: tool.main(argv + ['--return-policy', tool.RESTORED_RETURN,
                                           '--plan', 'first-read', '--profile', '3']), 'first-read requires profile 2'))
    with contextlib.redirect_stdout(io.StringIO()): assert tool.main(argv) == 0
    assert tool.parse_input((a.output / 'default.input.bin').read_bytes())['profile'] == 2 and selected == [tool.BUNDLES[2]]
    (a.output / 'result.json').write_text(json.dumps(dict(result='pass', physical=False,
        native=str(a.native), refusals=refusals, strict_return_refusals=strict_refusals,
        identity_prefix='modeled kernel_main; task/monitor log from actual linked producer', default_profile=2, controlled_before_tasks_only=True), indent=2)+'\n')


if __name__ == '__main__': main()
