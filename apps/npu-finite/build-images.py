#!/usr/bin/env python3
"""Build the three watchdog-admitting finite NPU images and their run instructions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CACHE = Path.home() / '.cache/sunstoneos'
PROFILES = {
    'startup-only': ('--startup-only', 0, [0x0601, 0x0602, 0x0603, 0x0608, 0x0609, 0x060a, 0x060b]),
    'single-inference': ('--single-inference', 1,
        [0x0601, 0x0602, 0x0603, 0x0604, 0x0605, 0x0606, 0x0607, 0x0608, 0x0609, 0x060a, 0x060b]),
    'runtime': ('--runtime', 2,
        [0x0601, 0x0620, 0x0621, 0x0622, 0x0623, 0x0624,
         0x0621, 0x0622, 0x0623, 0x0624, 0x0608, 0x0625, 0x0609, 0x060a, 0x060b]),
}
PASS_ORDER = [*range(0x0630, 0x0637), 0x0637, 0x0637, 0x0637, *range(0x0638, 0x0644)]
FAILURES = """| Last progress stage | Failure signature |
| --- | --- |
| Before EL3 entry | No matching RTC session tag; launcher/admission failure or reset before entry. |
| `0x0101` | EL3 admission/setup failure; first-fault words or EL3_STOP identify the detail. |
| `0x0102` | EL1 entry or task construction failed before client startup. |
| `0x0601` | Platform readiness did not arrive: inspect the first NPU failure, clocks, supply or routing. |
| `0x0630`–`0x0643` | Startup register-pass sub-step; the marker precedes its reads. A clean refusal preserves the first failing register/site, then the client may write retained reason 9. |
| `0x0602` or `0x0620` | Controller/bootstrap OPEN did not finish; inspect reset, IRQ status and idle evidence. |
| `0x0603` | Bootstrap succeeded; the next client operation did not complete. |
| `0x0604` or `0x0622` | Numerical completion did not arrive, or the controller retained a hardware fault. |
| `0x0605` or `0x0623` | Completion arrived; output transfer or its validation did not finish. |
| `0x0606` or `0x0624` | Output copied; reap, next network or close did not finish. A numerical mismatch can still return normally with status 1. |
| `0x0607` | Reap succeeded; close did not finish. |
| `0x0608` or `0x0625` | CLOSE or interrupt-route retirement did not finish. |
| `0x0609` | Local stop, SRAM handback or quiescence did not finish. |
| `0x060a` | Quiescence succeeded; receipt publication or final liveness check failed. |
| `0x060b` | Exit began; monitor cleanup/handback/disarm did not finish. |
| `0x0621` | Runtime LOAD did not finish or refused the selected model. |
| `0x062e` or `0x062f` | Runtime helper refused or failed; value contains its reason. |
| `0x06ff` | Finite client retained; value 9 means readiness wait lost liveness. The symbolized first failure identifies the earlier bootstrap predicate or startup register check. Startup diagnostics include the register offset/full value or lifecycle stage. |
| `0x0111` | EL3 stopped and shortened WDT0; decode the preserved first failure and stop detail. |
| `0x0503` | Mode value must be zero and normal reports must validate. Failed disarm parks instead of returning. |
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=CACHE / 'build/npu-wdt-1031')
    parser.add_argument('--baseline', type=Path, default=CACHE / 'evidence/npu-finite-quiescence-20261018')
    parser.add_argument('--artifacts', type=Path, default=CACHE / 'build/npu-board/session-artifacts.json')
    parser.add_argument('--template', type=Path, default=CACHE /
        'evidence/sol-resume-20261031/port-3-3/npu-provider-construction/readback-session.bin')
    args = parser.parse_args()
    build = args.build.resolve()
    images = build / 'images'
    images.mkdir(parents=True, exist_ok=True)
    if any((images / name).exists() for name in PROFILES):
        parser.error('Image output directories already exist; preserve them and select a fresh --build.')
    template = args.template.read_bytes()
    if len(template) != 456:
        parser.error('The native NPU configuration template must contain 456 bytes.')
    (images / 'session-template.bin').write_bytes(template)
    provenance = {'template_source': str(args.template.resolve()), 'template_sha256': digest(args.template),
        'template_bytes': len(template), 'purpose': 'PMIC/pin authority only; live pin settings, clocks and fresh identities are rebound',
        'physical_qualification': False, 'images': []}
    for index, (name, (flag, jobs, stages)) in enumerate(PROFILES.items(), 1):
        directory = images / name
        command = [sys.executable, str(ROOT / 'apps/npu-finite/build.py'), '--evidence', str(directory),
                   '--baseline', str(args.baseline.resolve()), '--artifacts', str(args.artifacts.resolve()), flag]
        with (build / f'{name}-build.log').open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        elf_hash, binary_hash = digest(directory / 'image.elf'), digest(directory / 'image.bin')
        terminal = ('NPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0' if not jobs else
                    f'NPU_INFERENCE=MATCH; inference_jobs={jobs}')
        sequence = ' → '.join(f'`0x{s:04x}`' for s in [0x0101, 0x0102, *stages, 0x0503])
        pass_sequence = ' → '.join(f'`0x{s:04x}`' for s in PASS_ORDER)
        preparation_hash = digest(ROOT / 'apps/npu-finite/development.py')[:8]
        run = build / 'runs' / f'{index:02d}-{name}-{elf_hash[:12]}-{preparation_hash}'
        commands = (f'SUNSTONE_USB_PORT=3-3 {sys.executable} {ROOT}/tools/run-ddr.py '
                    f'--fel {CACHE}/build/sunxi-a733/sunxi-fel --evidence {run}-ddr\n'
                    f'{sys.executable} {ROOT}/tools/dev-run.py npu --image {directory}/image.elf '
                    f'--records {images}/session-template.bin --port 3-3 --evidence {run}\n')
        readme = f'''# Finite NPU image: {name}

Observed build artifacts, not board qualification:

- ELF SHA-256: `{elf_hash}`
- Binary SHA-256: `{binary_hash}`
- Watchdog marker: mode `0xb1`, application window 20 seconds.
- Default bin-4 operating point: 852 MHz at verified 800 mV.
- Configuration template SHA-256: `{provenance['template_sha256']}`.

Inferred healthy terminal report: `{terminal}`. Expected status is 0,
IRQ completions are {jobs + 1}, IRQ stopped is 1, and the image returns normally.
{'Startup-only performs no numerical inference and does not claim functional success.' if not jobs else 'Every copied output must match its linked reference.'}

Expected client/return breadcrumb order: {sequence}.
Register-pass sub-steps: {pass_sequence}. `0x0637` values are 0, 1 and 2.
The readiness-wait client can interleave with these markers; OPEN follows
the completed pass. Names and the full stage comparison are in
apps/npu-finite/FABRIC-STAGE.md.
LAST retains only the most recent stage; the separate first-failure words survive later stages.

{FAILURES}

Coordinator commands, in image order startup-only, single-inference, runtime:

```sh
{commands}```

Use the coordinator's recovered, assigned port if it differs from 3-3. Stop at
the first transport failure. These commands have not been executed here.
Expected runner summary is `npu: returned; matched`. A watchdog return requires
fresh DDR initialization before another attempt and never restores old SRAM.
The template supplies historical authority; development preparation binds
new session identities and live PL0/PL1 mux, pull and drive settings. It also
reads live SID, fabric, routing and local reset state. Expected and desired
PMIC pin settings are equal; the native pin provider uses its existing
disable/configure/restore sequence during acquisition.
The captured MBUS word and rate remain in the evidence; the session names
the expected post-EL3 600 MHz state using tools/vendor-mbus.py. A skipped
restore still fails the native exact comparison.
Active-NPU watchdog recovery remains physically unqualified.
The startup failure diagnostic is described in apps/npu-finite/STARTUP-FAILURE.md.
Run startup-only first; a repeated retained value 9 must now carry the earlier
symbolized predicate or register comparison. Stop before the inference images
unless startup-only returns normally.
'''
        (directory / 'README.md').write_text(readme)
        provenance['images'].append(dict(name=name, elf_sha256=elf_hash,
            binary_sha256=binary_hash, expected_report=terminal, command=command))
    (images / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    subprocess.run([sys.executable, str(ROOT / 'apps/npu-finite/images-check.py'),
                    '--images', str(images)], check=True)


if __name__ == '__main__':
    main()
