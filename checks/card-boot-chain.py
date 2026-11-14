#!/usr/bin/env python3
"""Check that boot-once admission requires a measured watchdog recovery for every stage of the card boot chain.

tools/boot_chain.py is the shared WDT0 model: it classifies each arming as from mode 0 or as a re-arm without a disarm,
and grants a stage recovery only when the board has shown that arming expire in the same state. checks/card-fel-recovery.py
and checks/stage2-session.py run the gate and stage-2 code and compare their armings with the table; this check covers
the model, the table's board records and the runner's refusal. docs/designs/card-boot-chain.md explains the chain.
"""
import argparse
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
KEY = 0x16aa0000


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    chain = module('check_boot_chain', 'tools/boot_chain.py')
    mode = chain.WDT_MODE
    cases = []

    # The WDT0 model: only keyed mode writes count, and an arming is from mode 0 only when WDT0 read disarmed before it.
    for name, writes, before, expected in (
            ('runner-arms-from-0', [(mode, KEY | 0x41)], 0, [(chain.FROM_ZERO, 0x41)]),
            ('rearm-over-retained-0x41', [(mode, KEY | 0xb1)], 0x41, [(chain.REARM, 0xb1)]),
            ('disarm-then-arm', [(mode, KEY), (mode, KEY | 0xb1)], 0x41, [(chain.FROM_ZERO, 0xb1)]),
            ('unkeyed-disarm-is-ignored', [(mode, 0), (mode, KEY | 0x41)], 0xb1, [(chain.REARM, 0x41)]),
            ('other-registers-ignored', [(0x02050018, KEY | 2), (0x02050010, KEY | 1)], 0, []),
            ('disarm-only', [(mode, KEY)], 0x41, [])):
        assert chain.armings(writes, before) == expected, (name, chain.armings(writes, before))
        cases.append('model-' + name)

    # Every board record the table cites exists in the evidence files.
    ids = set()
    for path in (ROOT / 'docs/evidence').glob('*.jsonl'):
        for line in path.read_text().splitlines():
            if line.strip():
                ids.add(json.loads(line).get('id'))
    for key, records in chain.MEASURED.items():
        assert key[2] == chain.FEL and records and all(r.split(':')[0] in ids for r in records), key
    cases.append('measured-armings-cite-board-records')

    # Today's chain: every stage after the gate expires after boot0's PMIC writes, which no board run has shown.
    problems = chain.unproven()
    assert [p.split(' ')[0] for p in problems] == [s['name'] for s in chain.STAGES] and all('expire after boot0 trained DRAM' in p for p in problems), problems
    assert chain.ACCEPTED == {}, 'An accepted risk is a coordinator decision with its reference, never a default'
    cases.append('boot0-risk-leaves-every-stage-unproven')

    # An acceptance of that risk grants only armings that the board has shown to expire in a FEL session.
    accepted = {chain.AFTER_BOOT0: {'by': 'model fixture', 'date': '2026-11-13', 'reference': 'none'}}
    assert chain.unproven(accepted=accepted) == []
    rearmed = [dict(s, arming=chain.REARM) for s in chain.STAGES]
    assert len(chain.unproven(rearmed, accepted=accepted)) == len(rearmed)
    cases.append('accepted-boot0-risk-still-refuses-a-rearm')
    # A FEL measurement of the re-arm would grant it once the boot0 risk is resolved too.
    measured = dict(chain.MEASURED)
    measured.update({(chain.REARM, s['mode'], chain.FEL): ('fixture',) for s in chain.STAGES})
    assert chain.unproven(rearmed, measured=measured) and not chain.unproven(rearmed, measured=measured, accepted=accepted)
    cases.append('rearm-needs-a-measurement-and-the-boot0-risk-resolved')
    # The pre-fix chain (re-arms in the gate and stage 2) is what ran on 2026-11-13; with the current table it is refused
    # for both reasons.
    assert all('re-armed without a disarm' in p for p in chain.unproven(rearmed))
    cases.append('pre-fix-chain-refused')

    # The runner refuses a boot-once run before it reads the slot manifest.
    images = module('check_chain_board_images', 'tools/board_images.py')
    try:
        images.boot_once_slot(SimpleNamespace(image=args.output / 'absent.elf', boot_once_slot=args.output / 'absent.json'))
    except ValueError as error:
        assert 'no measured watchdog recovery' in str(error) and all(s['name'] in str(error) for s in chain.STAGES), error
    else:
        raise AssertionError('A boot-once run was admitted while a chain stage has no measured recovery')
    cases.append('runner-refuses-boot-once')

    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': cases,
                                                         'unproven': problems}, indent=2) + '\n')
    print('PASS card boot chain: %d cases; %d stages without a measured recovery' % (len(cases), len(problems)))


if __name__ == '__main__':
    main()
