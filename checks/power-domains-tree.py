#!/usr/bin/env python3
"""Check the source-tree facts that give PCK600 domains 1 and 5 one owner.

docs/power-domains.md makes drivers/power/a733_power_domains.c the only writer
of PCK600 domains 1 (DE_SYS), 4 (NPU) and 5 (GPU_TOP), and runs every PMIC transaction
from one service task per image. This check fails when:

- a source other than the owner and its native adapter names a domain-1 or
  domain-5 PPU register, unless it is a reader listed for that domain and
  the line has no write hint (write, store, update or "= value"). The
  register may be spelled as a 0x0706[15]xxx literal, with or without
  leading zeros, an upper-case X and a U or L suffix, as
  A733_DE_SYS_PCK_BASE, BB8_DOMAIN1_STATUS (bundle 8's name for the
  domain-1 status) or A733_PCK_DOMAIN_BASE(1) or (5). An address
  computed from another base, or a write on a line after the one that names
  the register, is not seen; for bundle 8, checks/board-bundles-apps-model.c
  asserts that the gate phase writes nothing;
- the set of sources that write the pinned delay row (value 0x1f1f1f) differs
  from the owner plus the writers of domains the owner does not hold, five
  sources in all, or a native allowlist other than the GPU native adapter's
  admits the delay row. The GPU controller writes the row of domain 6
  (GPU_CORE) through that adapter, so the adapter must name no PCK address
  other than the CORE6 page 0x07066000 and must compare delay-row offsets
  and values only under its CORE bank (bank == A733_GPU_CORE). Two seeded
  mutants of the adapter, a computed 0x07060000 + 0x5000 address and a row
  comparison under the CCU bank, must both fail that test;
- a source outside checks/ registers a domain-4 row while the NPU platform
  writes domain 4 itself: the owner and the NPU would both write it;
- an image in the service table polls the PMIC provider itself, or does not
  create its service task at the listed priority below its USB0 task, on A55
  core 0;
- the owner's own source (drivers/power/a733_power_domains.c) enters a
  critical section, suspends the scheduler, masks interrupts, delays, or
  takes a semaphore, queue or notification. This covers the owner only: the
  PMIC poll that the step calls may block in one-tick waits on the I2C mutex
  and the register lock when it activates a request, until that request's
  deadline, and a higher-priority task still preempts it there.

It reads source text only and runs no build. It prints the delay-row writer
count that docs/designs/power-domains.md budgets. With --root naming a tree
before the owner, such as the merge base, it prints only the delay-row writer
and allowlist counts, the baseline that the design's measurements record.
"""
import argparse
import re
from pathlib import Path
if not __debug__:
    raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['drivers', 'platform', 'apps', 'runtime', 'include']
OWNER = {'drivers/power/a733_power_domains.c', 'drivers/power/a733_power_domains_native.c',
         'include/a733_power_domains.h', 'include/a733_power_domains_native.h'}
# Readers of domain 1 or 5 that write nothing there: the domain each reads,
# with the reason.
READERS = {
    'apps/deinterlace/native.c': ('1', 'requires domain 1 already ON before its clocks'),
    'apps/gpu-finite/return.c': ('5', 'confirms domain 5 OFF at the finite return'),
    'drivers/gpu/a733_gpu_resources_native.c': ('5', 'reads domain-5 status at the LOCAL lease release'),
    'apps/g2d/admission.c': ('1', 'records the domain-1 base in the admission profile'),
    # Bundle 8 (docs/designs/board-bundles-next.md) reads PPU_PWSR as a gate
    # before its NSI access, and its mapping check requires the page to be
    # mapped readable. checks/board-bundles-apps-model.c:105 asserts that the
    # gate phase writes nothing; the NSI phase writes only NSI ports 2, 14
    # and 15.
    'apps/board-bundles-dram/sequence.h': ('1', 'defines the domain-1 status that the bundle 8 gate reads'),
    'apps/board-bundles-dram/sequence.c': ('1', 'reads the domain-1 status as the bundle 8 gate'),
    'apps/board-bundles-dram/main.c': ('1', 'maps the domain-1 PPU page read-only for the bundle 8 gate'),
    'apps/gpu-power-id/main.c': ('5', 'reads domain-5 words in its read-only CORE6 prerequisite snapshot'),
}
SUFFIX = r'(?:[uU][lL]{0,2}|[lL]{1,2}[uU]?)?'
DOMAIN_REGISTER = re.compile(r'\b0[xX]0*706(?P<literal>[15])[0-9a-fA-F]{3}' + SUFFIX + r'\b|'
                             r'\bA733_DE_SYS_PCK_BASE\b|\bBB8_DOMAIN1_STATUS\b|'
                             r'\bA733_PCK_DOMAIN_BASE\(\s*(?P<base>[15])' + SUFFIX + r'\s*\)')
WRITE_HINT = re.compile(r'write|store|update|=\s*value', re.I)
# Writers of the delay row for domains the owner does not hold: video (2, 3),
# GPU_CORE (6), HDMI and DisplayPort (10). The NPU requests domain 4 through
# its row.
DELAY_WRITERS = {'drivers/power/a733_power_domains.c', 'drivers/video/a733_video_platform.c',
                 'drivers/gpu/a733_gpu_power.c', 'platform/a733/hdmi_native.c', 'apps/displayport/native.c'}
# The GPU native adapter admits the pinned row only for its CORE bank,
# PCK row 6 at 0x07066000.
DELAY_ALLOWLISTS = {'drivers/gpu/a733_gpu_platform_native.c'}
# Image: (source, service creation, USB0 creation or priority fact, core-0 placement).
IMAGES = {
    'GPU': ('apps/gpu-render/main.c',
            'xTaskCreateStaticAffinitySet(functions[i],names[i],4096,NULL,i?2:1,',
            'xTaskCreateStatic(functions[i],names[i],4096,NULL,i?2:1,', None),
    'accelerator': ('apps/npu-convolution/native.c',
                    'xTaskCreateStaticAffinitySet(pmic_task,"npu-pmic",4096,&app,3,app.stack[0],&app.task[0],1)',
                    None, None),
    'GPU display': ('apps/gpu-display/main.c',
                    'xTaskCreateStatic(functions[i],names[i],4096,NULL,!i||i==2?2:i==3?3:4,',
                    None, 'if(!i)vTaskCoreAffinitySet(app.handles[i],1);'),
    'HDMI': ('apps/hdmi/main.c', 'xTaskCreateStatic(functions[i],names[i],2048,NULL,i?4:3,',
             None, 'if(!i)vTaskCoreAffinitySet(app.handles[i],1);'),
    'DisplayPort and G2D': ('apps/board/main.c',
                            'xTaskCreateStatic(functions[i],names[i],2048,NULL,!i?3:i==2||i==3?5:4,',
                            None, 'if(!i)vTaskCoreAffinitySet(board.handles[i],1);'),
}
# A domain-4 row registration: a733_power_domain_row_init(row, owner, 4, ...).
DOMAIN4_ROW = re.compile(r'a733_power_domain_row_init\s*\([^;]*?,[^;,]*?,\s*4\s*,', re.S)
BLOCKING = re.compile(r'\b(taskENTER_CRITICAL|portENTER_CRITICAL|vTaskSuspendAll|portDISABLE_INTERRUPTS|'
                      r'xSemaphoreTake|vTaskDelay|ulTaskNotifyTake|xTaskNotifyWait|xQueueReceive)\b|daifset')


GPU_NATIVE = 'drivers/gpu/a733_gpu_platform_native.c'
HEX = re.compile(r'\b0[xX]([0-9a-fA-F]+)' + SUFFIX + r'\b')
# Delay-row offsets and the row's first value as the adapter compares them.
ROW_COMPARISON = re.compile(r'\b(?:off\s*==\s*0x(?:170|174|c00|c04|c10)|set\s*==\s*0x1f1f1f)\b', re.I)
CORE_BANK = re.compile(r'\s*bank\s*==\s*A733_GPU_CORE\b')


def gpu_native_problems(text):
    """The GPU native adapter's PCK reach: CORE6's page only, and every
    delay-row comparison inside a parenthesized condition that starts with
    bank == A733_GPU_CORE."""
    code = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    problems = []
    for m in HEX.finditer(code):
        value = int(m[1], 16)
        if 0x07060000 <= value < 0x07079000 and value != 0x07066000:
            problems.append(f'{GPU_NATIVE}: names a PCK address other than 0x07066000: {m[0]}')
    for m in ROW_COMPARISON.finditer(code):
        depth, inside = 0, False
        for i in range(m.start() - 1, -1, -1):
            if code[i] == ')':
                depth += 1
            elif code[i] == '(':
                if depth:
                    depth -= 1
                elif CORE_BANK.match(code, i + 1):
                    inside = True
                    break
        if not inside:
            problems.append(f'{GPU_NATIVE}: compares a delay-row word outside the CORE bank: {m[0]}')
    return problems


def files(root):
    for d in SOURCES:
        base = root / d
        if base.is_dir():
            yield from (p for p in base.rglob('*') if p.suffix in {'.c', '.h', '.S'})


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=ROOT)
    root = ap.parse_args().root.resolve()
    problems = []
    writers, allowlists, domain4_rows, scanned = set(), set(), set(), 0
    for path in files(root):
        rel = str(path.relative_to(root))
        text = path.read_text(errors='replace')
        scanned += 1
        uses = re.findall(r'(==\s*|expected\s*=\s*)?0x1f1f1f\b', text, re.I)
        if uses:
            # A native allowlist only compares a written value with the delay row.
            (allowlists if all(uses) else writers).add(rel)
        if rel in OWNER:
            continue
        if DOMAIN4_ROW.search(re.sub(r'/\*.*?\*/', '', text, flags=re.S)):
            domain4_rows.add(rel)
        for n, line in enumerate(text.splitlines(), 1):
            domains = {m['literal'] or m['base'] or '1' for m in DOMAIN_REGISTER.finditer(line)}
            if not domains:
                continue
            if rel not in READERS or domains != {READERS[rel][0]}:
                problems.append(f'{rel}:{n}: names a domain-1 or domain-5 PPU register: {line.strip()}')
            elif WRITE_HINT.search(line):
                problems.append(f'{rel}:{n}: may write a domain the owner holds: {line.strip()}')
    assert scanned > 100, f'only {scanned} source files under {root}'
    if not (root / 'drivers/power/a733_power_domains.c').exists():
        # A tree before the owner, such as the merge base (--root): report the
        # counts that the design records as its baseline.
        print(f'{len(writers)} delay-row writers and {len(allowlists)} native allowlists: '
              f'{sorted(writers)}; {sorted(allowlists)}')
        return
    if writers != DELAY_WRITERS:
        problems.append(f'delay-row writers {sorted(writers)} differ from {sorted(DELAY_WRITERS)}')
    if allowlists != DELAY_ALLOWLISTS:
        problems.append(f'delay-row native allowlists {sorted(allowlists)} differ from {sorted(DELAY_ALLOWLISTS)}')
    native = (root / GPU_NATIVE).read_text()
    if not ROW_COMPARISON.search(native):
        problems.append(f'{GPU_NATIVE}: no delay-row comparison found to check')
    problems += gpu_native_problems(native)
    # Seeded mutants: a computed TOP5 address and a row comparison under the
    # CCU bank must each be reported.
    for name, old, new in [
            ('computed TOP5 address', '0x07066000u + off', '0x07060000u + 0x5000 + off'),
            ('row value under the CCU bank', '(bank == A733_GPU_CCU && off == 0xb20 &&',
             '(bank == A733_GPU_CCU && set == 0x1f1f1f && off == 0xb20 &&')]:
        if native.count(old) != 1:
            problems.append(f'{GPU_NATIVE}: the {name} mutant has no unique site')
        elif not gpu_native_problems(native.replace(old, new)):
            problems.append(f'{GPU_NATIVE}: the {name} mutant was not detected')
    if domain4_rows and 'drivers/npu/a733_npu_platform.c' in writers:
        problems.append(f'domain-4 rows {sorted(domain4_rows)} while drivers/npu/a733_npu_platform.c still '
                        'writes domain 4; move the NPU onto its row first (docs/power-domains.md)')
    for image, (source, service, single, placement) in IMAGES.items():
        text = (root / source).read_text()
        if 'a733_pmic_poll(' in text:
            problems.append(f'{source}: the {image} image polls the PMIC provider outside its service task')
        if 'a733_power_service_step(' not in text:
            problems.append(f'{source}: the {image} image has no power service step')
        for fact in (service, single, placement):
            if fact and fact not in text:
                problems.append(f'{source}: the {image} service task is not created as listed: {fact}')
    owner = (root / 'drivers/power/a733_power_domains.c').read_text()
    step = re.sub(r'/\*.*?\*/', '', owner, flags=re.S)
    for m in BLOCKING.finditer(step):
        problems.append(f'drivers/power/a733_power_domains.c: the service step uses {m[0]}')
    for p in problems:
        print(p)
    assert not problems, f'{len(problems)} problem(s)'
    print(f'PASS power-domain tree facts in {scanned} sources: domains 1 and 5 written only by the owner; '
          f'{len(writers)} delay-row writers and {len(allowlists)} native allowlists, the GPU one reaching only '
          f'the CORE6 page under its CORE bank (two seeded mutants refused); {len(domain4_rows)} '
          'domain-4 row sources and no NPU write to domain 4; five images run their PMIC work from one service '
          'task below USB0 on core 0; '
          'no critical section, scheduler suspension or wait in the owner')


if __name__ == '__main__':
    main()
