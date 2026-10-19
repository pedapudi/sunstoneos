#!/usr/bin/env python3
"""Check the expanded CPU0 report through the shared runner without hardware."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


host = module('cache_host', ROOT/'tools/primary-cache-host.py')
harness = module('primary_harness', ROOT/'checks/primary-publication-host.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bundle', 'model', 'survey', 'fel', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((args.build/'manifest.json').read_text())
    el1 = manifest.get('el1', False)
    extent = host.probe.report_spec(manifest)
    cases = host.EL1_CASES if el1 else host.CASES
    reports = {}
    if el1:
        model = json.loads(args.model.read_text())
        for name in ('success', 'baseline-refusal'):
            path = args.model.parent/('report-'+name+'.bin')
            assert host.digest(path) == next(c['report_sha256'] for c in model['cases'] if c['case'] == name)
            reports[name] = path.read_bytes()
    host.load(args.build, args.bundle/'image.elf', manifest)
    proof = {'schema': 'a733-primary-cache-software-v1', 'physical_access': False,
        'el1': el1, 'manifest_sha256': host.digest(args.build/'manifest.json'),
        'host_sources': {n: host.digest(ROOT/n) for n in host.HOST_SOURCES},
        'instruction_evidence': {'path': str(args.model.resolve()), 'sha256': host.digest(args.model)},
        'host_cases': dict.fromkeys(cases, True)}
    fixture = args.output/'MODEL-never-deploy.json'
    fixture.write_text(json.dumps(proof, indent=2)+'\n')
    # Reuse transport/restoration simulation with the new report reservation.
    harness.primary = SimpleNamespace(probe=SimpleNamespace(REPORT=extent,
        OVERLAY=host.probe.primary.probe.OVERLAY, MAGIC=host.probe.primary.probe.MAGIC))
    contract = (args.bundle/'contract.bin').read_bytes()
    epoch = struct.unpack_from('<Q', contract, 24)[0]
    # Native admission derives this deadline after entry; the upload has zero.
    deadline = 24000000
    accepted = {}
    for case in cases:
        def report(data, dram, unused):
            result = bytearray(reports['baseline-refusal' if case == 'returned-local-refusal' else 'success']) if el1 else bytearray(extent[1])
            result[:256] = data
            if case in ('wrong-epoch-restored', 'incomplete-el1-restored'):
                struct.pack_into('<Q', result, 24, 4)
            before = (*struct.unpack_from('<4Q', data, 16*8), 0x731)
            active = (before[0] | 0x1005, 0x80823019, 0x421f2000, 0x44ff00, before[4] & ~0x200)
            struct.pack_into('<20Q', result, 0x2000, 3, epoch+1 if case == 'wrong-epoch-restored' else epoch,
                             deadline, 1, *before, *active, *before, 0)
            if el1:
                for offset in (0xa08, 0x2808):
                    struct.pack_into('<2Q', result, offset, epoch, deadline)
                struct.pack_into('<Q', result, 0x1a20, deadline)
                if case == 'returned-local-refusal':
                    result[0x1a00:] = bytes(len(result)-0x1a00)
                elif case == 'incomplete-el1-restored':
                    struct.pack_into('<Q', result, 0x2800, 2)
            return bytes(result)
        if case == 'changed-input-refused':
            manifest = json.loads((args.build/'manifest.json').read_text())
            manifest['report']['bytes'] -= 4
            try:
                host.load(args.build, args.bundle/'image.elf', manifest)
            except ValueError:
                pass
            else:
                raise AssertionError('Changed report reservation accepted')
        elif case == 'changed-entry-patch-refused':
            changed = json.loads((args.build/'manifest.json').read_text())
            changed['native_patches'][0]['replacement'] ^= 4
            try: host.load(args.build, args.bundle/'image.elf', changed)
            except ValueError: pass
            else: raise AssertionError('Changed entry patch accepted')
        elif case in ('wrong-epoch-restored', 'incomplete-el1-restored'):
            sub = args.output/case
            sub.mkdir()
            harness.run_case(SimpleNamespace(**{**vars(args), 'output': sub}),
                             'invalid-report-restored', fixture, report)
        elif case == 'returned-local-refusal':
            sub = args.output/case; sub.mkdir()
            harness.run_case(SimpleNamespace(**{**vars(args), 'output': sub}), 'complete-return', fixture, report)
            returned = json.loads((sub/'complete-return/runtime/session.json').read_text())['result']
            assert returned['local_refusal'] and not returned['primary_cached_el1_completed']
        else:
            harness.run_case(args, case, fixture, report)
        accepted[case] = True
        (args.output/'accepted-cases.json').write_text(json.dumps(accepted, indent=2)+'\n')
        print('PASS '+case, flush=True)
    proof.update(host_cases=accepted, check_source_sha256=host.digest(Path(__file__)),
        limits='Actual shared runner; simulated FEL, queue and report. Physical cache adoption remains untested.')
    (args.output/'software.json').write_text(json.dumps(proof, indent=2)+'\n')


if __name__ == '__main__':
    main()
