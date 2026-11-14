#!/usr/bin/env python3
"""Host cases for the reschedule bands (kernel/scheduler/bands.h, docs/designs/scheduler-partitioned.md).

Compiles bands.h with the A733 and QEMU priority configurations and checks four rules on the values it yields:
a task takes every publication of its own band and above and holds only lower bands'; the API mask holds every band
SGI; SGI 0 and the tick sit at the API level; and every device interrupt the source routes is above every band mask,
so no band mask holds it. The A733 device priorities come from every route table in the source and the PPI writes of
the platform. Three old designs must fail: a strict mask (the priority of the band's own SGI), which holds same-band
publications; SGIs one level each below the API level, which on QEMU puts the band masks below the device routes;
and the deadline PPI written as 0xd8 at Non-secure EL1, which reads 0xd0, the band-3 mask."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import hw_facts as hw  # noqa: E402

# The priority configuration of each platform: include/FreeRTOSConfig.h and the port's portmacro.h.
PLATFORMS = {
    'a733': dict(levels=16, api=11, shift=4),
    'qemu': dict(levels=32, api=19, shift=3),
}
ROUTE = re.compile(r'\{\s*[A-Za-z0-9_]+\s*,\s*[A-Za-z0-9_]+\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*[A-Za-z_][A-Za-z0-9_]*\s*\}')
PROGRAM = r'''
#include <stdio.h>
#include "bands.h"
int main(void)
{
    printf("api %u\n", (unsigned)portMAX_API_PRIORITY_MASK);
    printf("command %u\n", (unsigned)SS_BAND_COMMAND_PRIORITY);
    for (unsigned b = 1; b <= SS_BANDS; ++b) printf("sgi %u %u\n", b, (unsigned)SS_BAND_SGI_PRIORITY(b));
    for (unsigned p = 0; p < configMAX_PRIORITIES; ++p) printf("task %u %u %u\n", p, ss_band(p), ss_band_mask(p));
    return 0;
}
'''


def definitions(platform):
    c = PLATFORMS[platform]
    lowest_usable = c['levels'] - 2
    return ['-DconfigSS_BAND_SGIS=1', '-DconfigMAX_PRIORITIES=8',
            '-DconfigMAX_API_CALL_INTERRUPT_PRIORITY=%dULL' % c['api'], '-DportPRIORITY_SHIFT=%d' % c['shift'],
            '-DportLOWEST_USABLE_INTERRUPT_PRIORITY=%dUL' % lowest_usable,
            '-DportMAX_API_PRIORITY_MASK=(%d<<%d)' % (c['api'], c['shift'])]


def values(header, platform, work):
    (work / 'bands.h').write_text(header)
    (work / 'p.c').write_text(PROGRAM)
    binary = work / ('bands-' + platform)
    subprocess.run(['clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror', *definitions(platform), '-I', str(work),
                    str(work / 'p.c'), '-o', str(binary)], check=True, env=dict(os.environ, TMPDIR=str(work)))
    out = {'sgi': {}, 'task': {}}
    for line in subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.splitlines():
        kind, *numbers = line.split()
        numbers = list(map(int, numbers))
        if kind in ('api', 'command'):
            out[kind] = numbers[0]
        elif kind == 'sgi':
            out['sgi'][numbers[0]] = numbers[1]
        else:
            out['task'][numbers[0]] = (numbers[1], numbers[2])
    return out


def device_priorities(platform, platform_c=None):
    """The priority each device interrupt has in the view the band masks compare against (ICC_PMR_EL1's)."""
    stored = set()
    sources = [ROOT / 'tools/sunstone-manifest.py', *sorted(ROOT.glob('apps/*/irq.c')), *sorted(ROOT.glob('apps/*/irq.h'))]
    for path in sources:
        text = path.read_text()
        if 'a733_irq_route' not in text:
            continue
        for value in ROUTE.findall(text):
            stored.add(int(value, 16))
    assert stored, 'no route table found'
    if platform == 'a733':
        # Routes are stored (Secure) bytes written at EL3; the PPIs are written at Non-secure EL1
        # (platform/a733-smp-cached/platform.c), so their stored byte is the Non-secure write rule's.
        platform_c = platform_c or (ROOT / 'platform/a733-smp-cached/platform.c').read_text()
        written = set()
        for ppi in (27, 30):
            write = re.search(r'\(g\+0x400\+%d\)=([^;]+);' % ppi, platform_c)
            assert write, 'the PPI %d write moved' % ppi
            value = write.group(1).strip()
            api = PLATFORMS['a733']['api'] << PLATFORMS['a733']['shift']
            written.add(api if value == 'portMAX_API_PRIORITY_MASK' else hw.ns_view(hw.stored_from_ns(int(value, 16))))
        return {hw.ns_view(s) for s in stored} | written
    # QEMU (platform/qemu-os) writes the same route bytes to a GIC without the Non-secure shift, and the deadline PPI at 0xd8.
    qemu_c = (ROOT / 'platform/qemu-os/platform.c').read_text()
    deadline = re.search(r'0x400 \+ DEADLINE_IRQ\) = (0x[0-9a-f]+);', qemu_c)
    assert deadline, 'the QEMU deadline write moved'
    return stored | {int(deadline.group(1), 16)}


def judge(v, platform, platform_c=None):
    """Return the first broken rule, or None."""
    api, sgi, task = v['api'], v['sgi'], v['task']
    if v['command'] != api:
        return 'SGI 0 and the tick are not at the API level'
    for b, priority in sgi.items():
        if priority <= api:
            return 'SGI %d at 0x%x is not held by the API mask 0x%x' % (b, priority, api)
    for p, (band, mask) in task.items():
        for q, (other, _) in task.items():
            taken = sgi[other] < mask
            if taken != (other >= band):
                return 'a task of priority %d (band %d, mask 0x%x) %s a publication for priority %d (band %d, SGI 0x%x)' % (
                    p, band, mask, 'takes' if taken else 'holds', q, other, sgi[other])
        if not api < mask:
            return 'the mask of priority %d is not above the API mask' % p
        for device in device_priorities(platform, platform_c):
            if not (api <= device < mask):
                return 'a device interrupt at 0x%x is held by the mask 0x%x of priority %d' % (device, mask, p)
        if v['command'] >= mask:
            return 'SGI 0 and the tick are held by the mask of priority %d' % p
    return None


def main():
    header = (ROOT / 'kernel/scheduler/bands.h').read_text()
    strict = ('#define SS_BAND_MASK(b) ((b) <= 1u ? (uint8_t)0xffu : SS_BAND_SGI_PRIORITY((b) - 1u))',
              '#define SS_BAND_MASK(b) SS_BAND_SGI_PRIORITY(b)')
    below_api = ('((unsigned)portLOWEST_USABLE_INTERRUPT_PRIORITY + 1u - (unsigned)(b)) << portPRIORITY_SHIFT',
                 '((unsigned)configMAX_API_CALL_INTERRUPT_PRIORITY + 4u - (unsigned)(b)) << portPRIORITY_SHIFT')
    with tempfile.TemporaryDirectory(prefix='scheduler-bands-', dir=os.environ.get('TMPDIR')) as name:
        work = Path(name)
        for platform in PLATFORMS:
            v = values(header, platform, work)
            broken = judge(v, platform)
            assert broken is None, '%s: %s' % (platform, broken)
            print('PASS %s bands: SGIs 1-3 at %s, masks %s, SGI 0 and the tick at 0x%x, devices at %s' % (
                platform, ', '.join('0x%x' % v['sgi'][b] for b in (1, 2, 3)),
                ', '.join('0x%x' % m for m in sorted({m for _, m in v['task'].values()})), v['command'],
                ', '.join('0x%x' % d for d in sorted(device_priorities(platform)))))
        for label, (before, after), platform in (('strict mask', strict, 'a733'), ('SGIs below the API level', below_api, 'qemu')):
            assert header.count(before) == 1, label
            broken = judge(values(header.replace(before, after), platform, work), platform)
            assert broken, 'the old design (%s) passed' % label
            print('PASS old design refused (%s, %s): %s' % (label, platform, broken))
        # The deadline PPI written as 0xd8 at Non-secure EL1 reads 0xd0 (measured), the band-3 mask.
        platform_c = (ROOT / 'platform/a733-smp-cached/platform.c').read_text()
        before = '(g+0x400+27)=portMAX_API_PRIORITY_MASK;'
        assert platform_c.count(before) == 1
        broken = judge(values(header, 'a733', work), 'a733', platform_c.replace(before, '(g+0x400+27)=0xd8;'))
        assert broken, 'the old deadline PPI write passed'
        print('PASS old design refused (deadline PPI written 0xd8, a733): %s' % broken)


if __name__ == '__main__':
    main()
