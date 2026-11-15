#!/usr/bin/env python3
"""Run the power-domain owner's host models, compare its domain-2 programming with the pinned vendor row, and refuse its mutants.

checks/power-domains-pck.c composes drivers/power/a733_power_domains.c with the actual PMIC provider, rail ledger and
supply verification over modeled PCK600, CCU and R_CCU registers (checks/power-domains-model.h). This check runs:

- the whole model, which covers domains 1, 4 and 5;
- its domain-2 cases (--domain2): no master-gate read, the delay row
  and ON command, adoption, refusals that set only bit 14, timeout retention, no power-off, the transition record
  and the back-to-back measurement mode;
- its access traces of domains 1, 4 and 5 (--domain-traces), which must equal checks/data/power-domains-traces.txt.
  That file is the same output from the owner at bringup 3ab4c296d, before domain 2 was added, so adding domain 2
  changes no access of the other domains;
- checks/power-domains-service.c, the service task's PMIC and step bounds;
- verified_at, the DCDC2 supply-verification time: written once, at the step that first publishes DCDC2 as verified,
  both with a settled supply (DCDC2 alone, at its own result) and without (with the done bit).

The five delay words and the policy nibble that the owner writes to domain 2 must equal the SUN60IW2_PCK_VE_DEC row
of the hash-pinned vendor pck600_domains.c, written at the offsets of its sun60iw2 PMU table (sunxi_pd_init, then
sunxi_do_pmu_set_power_domain with COMMAND_ON). Each mutant changes one text in the owner; a model run must fail.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from harness import CACHE, CLANG, SANITIZE, environment

sys.dont_write_bytecode = True
GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]
VENDOR = CACHE / 'sources/gpu-power/drivers/pm_domain/pck600_domains.c'
VENDOR_SHA256 = '2d9c2b60b3b264d1732538815d12570e45386d30b280a1d0b4f5b9780bdc49ee'
BINDING = CACHE / 'sources/gpu-power/include/dt-bindings/power/sun60iw2-power.h'
BINDING_SHA256 = 'f5da443de3a15290511f60c5a8697bc01ec714985eb876108e1da9db76fadbe7'
UNITS = ['drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
         'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
         'drivers/power/a733_pmic_consumer.c', 'drivers/gpu/a733_gpu_power.c']
CHECK_INPUTS = ['drivers/power/a733_power_domains.c', 'include/a733_power_domains.h', 'include/a733_power_domain_row.h',
                'drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c', 'drivers/clock/a733_clock_leases.c',
                'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
                'drivers/power/a733_pmic_consumer.c', 'drivers/gpu/a733_gpu_power.c', 'checks/power-domains-pck.c',
                'checks/power-domains-service.c', 'checks/power-domains-model.h', 'checks/a733-pmic.c',
                'checks/data/power-domains-traces.txt']
TRACES = ROOT / 'checks/data/power-domains-traces.txt'
OWNER_SOURCE = 'drivers/power/a733_power_domains.c'
MUTANTS = [
    ('{2,0,0,A733_RAIL_DCDC2', '{2,BIT(0),BIT(14),A733_RAIL_DCDC2', 'domain 2 requires the critical VE gates'),
    ('A733_POWER_DOMAIN5_FAULT,1}', 'A733_POWER_DOMAIN5_FAULT,0}', 'domain 5 gates required before ON'),
    ('{2,0,0,A733_RAIL_DCDC2,A733_POWER_DCDC2_VERIFIED,A733_POWER_DOMAIN2_FAULT',
     '{2,0,0,A733_RAIL_DCDC2,A733_POWER_DCDC2_VERIFIED,A733_POWER_DOMAIN5_FAULT', 'domain 2 fault on bit 13'),
    ('m->state=ppu ? A733_POWER_DOMAIN_ON : A733_POWER_DOMAIN_OFF;', 'm->state=A733_POWER_DOMAIN_OFF;',
     'adoption commands an ON domain'),
    ('    if((*policy&15)!=(status&15))return -1;\n', '', 'policy different from status admitted'),
    ('return (status&15)==0 ? 0 : (status&15)==8 ? 8 : -1;', 'return (status&0x10f)==0 ? 0 : (status&0x10f)==8 ? 8 : -1;',
     'refusal on status bit 8'),
    ('t->spin_reads+=spin!=0;', 't->spin_reads+=1;', 'per-step reads counted as spin reads'),
    ('else {t->pending_at=now;t->pending_word=v;}', 'else {t->pending_word=v;}', 'last pending read untimed'),
    ('(!d->measure_transitions && now-m->command_at>=A733_POWER_PCK_SPIN_US)', '(now-m->command_at>=A733_POWER_PCK_SPIN_US)',
     'measurement mode ignored'),
    ('{return id==1 ? 0 : id==4 ? 1 : id==5 ? 2 : id==2 ? 3 : -1;}', '{return id==1 ? 0 : id==4 ? 1 : id==5 ? 2 : -1;}',
     'domain 2 without a slot'),
    ('if(!rail4)d->verified_at=now;', 'd->verified_at=now;', 'settled DCDC4 result rewrites verified_at'),
    ('if(!rail4)d->verified_at=now;', '', 'settled supply leaves verified_at unwritten'),
    ('if(!d->settled_supply && (d->supply_verified&A733_POWER_DCDC2_VERIFIED))d->verified_at=now;',
     'if(d->supply_verified&A733_POWER_DCDC2_VERIFIED)d->verified_at=now;', 'done bit rewrites a settled verified_at'),
    ('if(!d->settled_supply && (d->supply_verified&A733_POWER_DCDC2_VERIFIED))d->verified_at=now;',
     'if(!d->settled_supply)d->verified_at=now;', 'verified_at written with DCDC2 unverified'),
    ('if(!d->settled_supply && (d->supply_verified&A733_POWER_DCDC2_VERIFIED))d->verified_at=now;', '',
     'boot verification leaves verified_at unwritten'),
    ('d->running=1;d->started_at=now;', 'd->running=1;', 'first step not stamped'),
]

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
runtime = {**environment(), 'UBSAN_OPTIONS': 'halt_on_error=1', 'ASAN_OPTIONS': 'detect_leaks=0', 'TMPDIR': str(out)}
flags = ['-std=c11', '-Wall', '-Wextra', '-O1', '-g', *SANITIZE]


def build(model, binary, owner_root=ROOT, strict=True):
    """owner_root supplies the owner's source and header (a mutant tree)."""
    units = [ROOT / u for u in UNITS] + [owner_root / OWNER_SOURCE]
    command = [CLANG, *flags, *(['-Werror'] if strict else []), '-I', owner_root / 'include',
               '-I', ROOT / 'include', '-I', ROOT / 'checks', ROOT / 'checks' / model, *units, '-o', binary]
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=runtime)
    binary.with_suffix('.build.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]


def run(binary, *extra):
    result = subprocess.run([str(binary), *extra], capture_output=True, text=True, env=runtime, timeout=600)
    binary.with_name(binary.name + ''.join(extra).replace('-', '_') + '.log').write_text(result.stdout + result.stderr)
    return result


def vendor_row():
    """The VE_DEC delay row and the sun60iw2 PMU offsets from the pinned source."""
    assert hashlib.sha256(VENDOR.read_bytes()).hexdigest() == VENDOR_SHA256, 'pinned pck600_domains.c differs'
    assert hashlib.sha256(BINDING.read_bytes()).hexdigest() == BINDING_SHA256, 'pinned sun60iw2-power.h differs'
    assert re.search(r'#define SUN60IW2_PCK_VE_DEC\s+2\b', BINDING.read_text())
    text = VENDOR.read_text()
    row = re.search(r'\[SUN60IW2_PCK_VE_DEC\]\s*=\s*DOMAIN\(SUN60IW2_PCK_VE_DEC,([^)]*)\)', text)
    ctrl0, ctrl1, logic0, logic1, off2on, smask = (int(v, 16) for v in row[1].split(','))
    pmu = re.search(r'sun60iw2_pck600_pmu = \{(.*?)\};', text, re.S)[1]
    offset = {k: int(v, 16) for k, v in re.findall(r'\.(\w+_offset) = (0x[0-9a-f]+)', pmu)}
    # sunxi_pd_init writes the five words in this order (pck600_domains.c lines 126-142).
    words = [(offset['device_ctrl0_delay_offset'], ctrl0), (offset['device_ctrl1_delay_offset'], ctrl1),
             (offset['logic_power_switch0_delay_offset'], logic0), (offset['logic_power_switch1_delay_offset'], logic1),
             (offset['off2on_delay_offset'], off2on)]
    command_on = int(re.search(r'#define COMMAND_ON\s+(0x[0-9a-f]+)', text)[1], 16)
    return words, offset['pwr_offset'], smask, command_on


words, power_offset, status_mask, command_on = vendor_row()
binary = out / 'pck'
build('power-domains-pck.c', binary)
whole = run(binary)
assert whole.returncode == 0, whole.stdout[-2000:] + whole.stderr[-2000:]
assert whole.stdout.splitlines()[-1].startswith('PASS power-domain owner')
domain2 = run(binary, '--domain2')
assert domain2.returncode == 0, domain2.stdout[-2000:] + domain2.stderr[-2000:]
lines = domain2.stdout.splitlines()
assert lines[-1].startswith('PASS domain 2')
written = [(int(a, 16), int(v, 16)) for a, v in (line.split()[1:] for line in lines if line.startswith('domain2-delay '))]
assert written == words, (written, words)
policy = [int(line.split()[1], 16) for line in lines if line.startswith('domain2-policy ')]
assert power_offset == 0 and len(policy) == 1 and policy[0] & status_mask == command_on
assert policy[0] & ~status_mask == 0xa5a50100, 'the ON command must keep the policy bits outside the status mask'
record = json.loads(next(line for line in lines if line.startswith('domain2-record ')).split(' ', 1)[1])
measured = json.loads(next(line for line in lines if line.startswith('domain2-measured ')).split(' ', 1)[1])

# The other domains' accesses against the committed traces from the owner before domain 2.
traces = run(binary, '--domain-traces')
assert traces.returncode == 0, traces.stderr[-2000:]
assert traces.stdout == TRACES.read_text(), 'domain 1, 4 or 5 accesses differ from checks/data/power-domains-traces.txt'
trace_lines = len(traces.stdout.splitlines())
assert trace_lines > 100

service = out / 'service'
build('power-domains-service.c', service)
result = run(service)
assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]
assert result.stdout.splitlines()[-1].startswith('PASS PMIC service task')

refused = []
for i, (old, new, name) in enumerate(MUTANTS):
    text = (ROOT / OWNER_SOURCE).read_text()
    assert text.count(old) == 1, (name, old)
    root = out / 'mutants' / str(i)
    (root / OWNER_SOURCE).parent.mkdir(parents=True)
    (root / OWNER_SOURCE).write_text(text.replace(old, new))
    (root / 'include').mkdir()
    (root / 'include/a733_power_domains.h').write_bytes((ROOT / 'include/a733_power_domains.h').read_bytes())
    mutant = root / 'pck'
    build('power-domains-pck.c', mutant, owner_root=root, strict=False)  # a mutant may leave a variable unused
    failed = [mode for mode in ([], ['--domain2']) if run(mutant, *mode).returncode != 0]
    assert failed, ('mutant survived', name)
    refused.append(dict(mutant=name, failed=[' '.join(m) or 'whole model' for m in failed]))

summary = dict(vendor_sha256=VENDOR_SHA256, domain2_delay_row=[f'{a:#x}={v:#x}' for a, v in written],
               domain2_policy=f'{policy[0]:#010x}', slow_record=record, measured_record=measured,
               unchanged_trace_lines=trace_lines, mutants_refused=refused,
               inputs={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in CHECK_INPUTS})
(out / 'result.json').write_text(json.dumps(summary, indent=1) + '\n')
print(f'PASS power-domain owner models: domains 1, 4 and 5 with {trace_lines} accesses as committed, '
      f'domain 2 programmed as the vendor VE_DEC row, the service task, {len(refused)} mutants refused')
