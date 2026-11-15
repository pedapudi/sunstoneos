#!/usr/bin/env python3
"""Run the decoder cold-start model on the actual power-domain owner, with PLL_VE0 at 624 MHz, and refuse its mutants.

checks/a733-video-cold.c composes the decoder platform binding, the JPEG executor, the real clock and PCK ownership
managers, and the actual PMIC provider, rail ledger and power-domain owner (checks/power-domains-model.h) with a
register model of the CCU, the published nonsecure boot ROM and the decoder. PCK domain 2 exists only in the owner
model's PPU words: the decoder requests it through its row and writes no PCK600 register. The model starts from the
port 3-3 FEL words (gates 0x5c0 0x110103ff, 0x5e0 0xf1055803, 0x5e4 0x4; PLL_VE0 0x41305a00) and from fifteen other
PLL entry and lock cases, the CCU words a hold-B fault left (refused at entry with no write), domain 2 OFF, ON or incoherent, critical gates clear after ON, two power lifetimes
joined by the quiesce (docs/designs/video-pll.md), the VE+0x804 pulse under two readbacks that no source documents,
and faults after the first write closed by a733_video_decoder_close. Every read the binding makes must pass the
native read allowlist. Each mutant changes one text in one driver source; the model must
fail with it.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from harness import CLANG, SANITIZE, environment

sys.dont_write_bytecode = True
GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]
ROM = Path.home() / '.cache/sunstoneos/sources/brom/ns-brom.bin'
ROM_SHA256 = '95be69c916846a8259578565f3193a6b9ab0625294d5374f3dc6c2f667fc6169'
SOURCES = ['drivers/video/a733_video_platform.c', 'drivers/video/a733_video_unit.c', 'drivers/video/a733_video_jpeg.c',
           'drivers/video/a733_video_jpeg_run.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/clock/a733_clock_leases.c',
           'drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c', 'drivers/pinctrl/a733_pins.c',
           'drivers/pinctrl/a733_pin_tables.c', 'drivers/power/a733_pmic_consumer.c', 'drivers/gpu/a733_gpu_power.c',
           'drivers/power/a733_power_domains.c']
CHECK_INPUTS = ['drivers/video/a733_video_platform.c', 'drivers/video/a733_video_unit.c', 'drivers/video/a733_video_jpeg.c',
                'drivers/video/a733_video_jpeg_run.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/clock/a733_clock_leases.c',
                'drivers/power/a733_pmic.c', 'drivers/power/a733_rail_leases.c', 'drivers/pinctrl/a733_pins.c',
                'drivers/pinctrl/a733_pin_tables.c', 'drivers/power/a733_pmic_consumer.c', 'drivers/gpu/a733_gpu_power.c',
                'drivers/power/a733_power_domains.c', 'checks/a733-video-cold.c', 'checks/a733-video-platform.c', 'checks/power-domains-model.h',
                'checks/a733-pmic.c', 'include/a733_pll_lock.h', 'include/a733_video_platform.h', 'include/a733_video_unit.h',
                'include/a733_power_domains.h', 'include/a733_power_domain_row.h', 'apps/jpeg/fixture.py',
                'apps/jpeg/fixtures.json']
MUTANTS = [
    ('drivers/video/a733_video_platform.c', 'check(u,CCU+0x1b10,0x30001,0x20000)', 'check(u,CCU+0x1b10,0x30001,0x10000)',
     'entry requires 0x1b10 at POWER_OFF'),
    ('drivers/video/a733_video_platform.c', 'p->checked=p->prepared=0;p->quiesced=1;u->failed=faulted;',
     'p->checked=p->prepared=0;u->failed=faulted;', 'reopen after the quiesce'),
    ('drivers/video/a733_video_unit.c', '    if(!u->failed)(void)a733_video_unit_unlock(u);\n    return a733_video_unit_fail(u);',
     '    return a733_video_unit_fail(u);', 'refusal inside a hold keeps the mutex'),
    ('drivers/video/a733_video_unit.c', '!(w&UINT32_C(0xb8000000))', '!(w&UINT32_C(0x38000000))', 'entry with enable set'),
    ('drivers/video/a733_video_unit.c', '!(w&UINT32_C(0xb8000000))', '!(w&UINT32_C(0x98000000))', 'entry with lock enable set'),
    ('drivers/video/a733_video_unit.c', '#define PLL_FIELDS UINT32_C(0xf870ff02)', '#define PLL_FIELDS UINT32_C(0xf870ffe2)',
     'lock levels admitted'),
    ('drivers/video/a733_video_unit.c', 'u->pll_inherited=pll&PLL_INHERITED;', 'u->pll_inherited=PLL_INHERITED;', 'bit 24 forced on'),
    ('drivers/video/a733_video_unit.c', '{{BIT(30),BIT(30)},{0x0070ff02u,A733_VIDEO_PLL_FACTORS},\n        {BIT(31),BIT(31)}',
     '{{BIT(30),BIT(30)},{BIT(31),BIT(31)},\n        {0x0070ff02u,A733_VIDEO_PLL_FACTORS}', 'enable before factors'),
    ('drivers/video/a733_video_unit.c', 'value=(*word&~(mask|PLL_LOCK))|(value&mask);', 'value=(*word&~mask)|(value&mask);',
     'LOCK written back'),
    ('include/a733_pll_lock.h', "if(read(cookie,&v)!=1 || (v&~A733_PLL_LOCK_BIT)!=word || now(cookie,&t)!=1)return 0;",
     "if(read(cookie,&v)!=1 || now(cookie,&t)!=1)return 0;", 'no whole-word check per LOCK read'),
    ('include/a733_pll_lock.h', 'if(!locked){l->locked=0;return 0;}', 'if(!locked)return 0;', 'LOCK drop ignored'),
    ('include/a733_pll_lock.h', '#define A733_PLL_LOCK_GAP_US 3u', '#define A733_PLL_LOCK_GAP_US 2u', 'a 2 us gap'),
    ('include/a733_pll_lock.h', 'next=t+A733_PLL_LOCK_GAP_US+1u;', 'next=t+A733_PLL_LOCK_GAP_US;', 'gap without the extra microsecond'),
    ('include/a733_pll_lock.h', '#define A733_PLL_LOCK_SETTLE_US 20u', '#define A733_PLL_LOCK_SETTLE_US 19u', 'a 19 us settle'),
    ('include/a733_pll_lock.h', '{return l->counted_at+A733_PLL_LOCK_SETTLE_US+1u;}', '{return l->counted_at+A733_PLL_LOCK_SETTLE_US;}',
     'settle without the extra microsecond'),
    ('include/a733_pll_lock.h', '#define A733_PLL_LOCK_MAX_READS 100u', '#define A733_PLL_LOCK_MAX_READS 50u', '50 reads'),
    ('drivers/video/a733_video_platform.c', '{{0xa80,0},{0xa88,1},{0xb00,4}}', '{{0xa88,1},{0xb00,4}}', 'encoder consumer ignored'),
    ('drivers/video/a733_video_platform.c', '{{0xa80,0},{0xa88,1},{0xb00,4}}', '{{0xa80,0},{0xa88,1}}', 'NPU consumer ignored'),
    ('drivers/video/a733_video_platform.c', '            if(!a733_video_unit_pll_wait(u))return fail(u);',
     '            if(lock(u)!=1||!a733_video_unit_pll_wait(u))return fail(u);', 'lock wait under the register mutex'),
    # Domain 2 through the owner's row, and the critical gates.
    ('drivers/video/a733_video_platform.c', '            r=held_phase(p,cold_entry,&p->cold.entered_at);break;',
     '            ++p->cold_step;break;', 'no read-only entry before the row request'),
    ('drivers/video/a733_video_platform.c', '            if(s==A733_POWER_ROW_PENDING)return 0;\n            if(s!=A733_POWER_ROW_ON)return fail(u);',
     '            if(s==A733_POWER_ROW_PENDING&&0)return 0;\n            if(s!=A733_POWER_ROW_ON&&s!=A733_POWER_ROW_PENDING)return fail(u);',
     'writes before the ON answer'),
    ('drivers/video/a733_video_platform.c', '    if(!open_mclk(p))return 0;\n', '', 'no 0x5e4 open: A1 refuses the FEL state'),
    ('drivers/video/a733_video_platform.c', '    p->cold.mclk_written=old|MCLK_KEY;', '    p->cold.mclk_written=old|BIT(18);',
     '0x5e4 opened without the key value'),
    ('drivers/video/a733_video_platform.c',
     '    for(unsigned i=0;i<3;++i)if(!(p->cold.gates[i]&bit[i]))return 0;\n    return update(u,CCU+0xa8c,BIT(18),BIT(18))&&',
     '    if(!update(u,CCU+0xa8c,BIT(18),BIT(18)))return 0;\n    for(unsigned i=0;i<3;++i)if(!(p->cold.gates[i]&bit[i]))return 0;\n    return ',
     'A1 after the reset release'),
    ('drivers/video/a733_video_platform.c', '    if(!open_mclk(p))return 0;\n', '    if(!open_mclk(p)||!update(u,CCU+0x5c0,1,1))return 0;\n',
     'a write to a critical gate'),
    ('drivers/video/a733_video_platform.c', '        if(r!=1)return r;\n    }\n}',
     '        if(r!=1)return r;\n        if(p->cold_step>COLD_HOLD_A)return 0;\n    }\n}', 'holds A and B split across calls'),
    ('drivers/video/a733_video_platform.c', '    return (actual&~(MCLK_KEY&~BIT(18)))==(p->cold.mclk_written&~(MCLK_KEY&~BIT(18)));',
     '    return actual==p->cold.mclk_written;', 'exact 0x5e4 readback'),
    ('drivers/video/a733_video_platform.c', '            if((m^value)!=BIT(31))return 0;\n', '', 'module word change with the gate open'),
    ('drivers/video/a733_video_platform.c',
     "    return update(u,CCU+0xa88,BIT(31),0)&&update(u,CCU+0xa8c,BIT(2),0)&&update(u,CCU+0xa8c,BIT(18),0)&&\n        a733_video_unit_pll_stop(u);",
     "    return a733_video_unit_pll_stop(u)&&update(u,CCU+0xa88,BIT(31),0)&&update(u,CCU+0xa8c,BIT(2),0)&&\n        update(u,CCU+0xa8c,BIT(18),0);",
     'PLL stopped before the module gate closes'),
    ('drivers/video/a733_video_unit.c', '    static const uint32_t disable[4]={BIT(27),BIT(29),BIT(31),BIT(30)};',
     '    static const uint32_t disable[4]={BIT(31),BIT(27),BIT(29),BIT(30)};', 'PLL disabled before its output gate'),
    ('drivers/video/a733_video_platform.c',
     '    return a733_video_unit_rmw(u,VE+0x804,0x11,0x11,vcu?&vcu[0]:NULL)&&\n'
     '        a733_video_unit_rmw(u,VE+0x804,0x11,0,vcu?&vcu[1]:NULL);',
     '    (void)vcu;return update(u,VE+0x804,0x11,0x11)&&update(u,VE+0x804,0x11,0);', 'VCU pulse read back exactly'),
    ('drivers/video/a733_video_platform.c', '(faulted?u->uncertain:u->failed||u->locked)',
     '(faulted?0:u->failed||u->locked)', 'fault close after an uncertain access'),
    ('drivers/video/a733_video_platform.c',
     '    return update(u,CCU+0xa88,BIT(31),0)&&update(u,CCU+0xa8c,BIT(2),0)&&update(u,CCU+0xa8c,BIT(18),0)&&',
     '    return update(u,CCU+0xa88,BIT(31),0)&&a733_video_unit_rmw(u,CCU+0xa8c,BIT(2),0,NULL)&&update(u,CCU+0xa8c,BIT(18),0)&&',
     'bus gate close not read back'),
    ('drivers/video/a733_video_platform.c', '    if(!close_steps(u))return a733_video_unit_refuse_held(u);',
     '    if(!faulted&&!close_steps(u))return a733_video_unit_refuse_held(u);', 'fault close writes nothing'),
    ('drivers/video/a733_video_unit.c', '    u->last_word=*v;\n    return 1;', '    return 1;', 'failing word not kept'),
    ('drivers/video/a733_video_platform.c',
     '            uint64_t n;if(a733_video_unit_now(u,&n)!=1)return -1;\n'
     '            p->cold.lock_reads=u->pll_reads;p->cold.locked_at=n;',
     '            p->cold.lock_reads=u->pll_reads;p->cold.locked_at=u->last_now;', 'lock wait stamped before the settle'),
    ('drivers/video/a733_video_platform.c', '    p->cold.closed_at=u->last_now;\n', '', 'close hold end not stamped'),
    ('drivers/video/a733_video_platform.c', '    for(unsigned i=0;i<3;++i)if(rd(u,CCU+gate[i],&p->cold.entry_gates[i])!=1)return 0;\n',
     '    for(unsigned i=0;i<sizeof(rom_identity)/sizeof(rom_identity[0]);++i)\n'
     '        if(!check(u,rom_identity[i].address,UINT32_MAX,rom_identity[i].word))return 0;\n'
     '    for(unsigned i=0;i<3;++i)if(rd(u,CCU+gate[i],&p->cold.entry_gates[i])!=1)return 0;\n', 'ROM identity read under the lock'),
]

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
assert hashlib.sha256(ROM.read_bytes()).hexdigest() == ROM_SHA256, 'published nonsecure ROM differs'
spec = importlib.util.spec_from_file_location('jpeg_fixture', ROOT / 'apps/jpeg/fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
jpeg = out / 'nonconstant.jpg'
jpeg.write_bytes(fixture.nonconstant())
flags = ['-std=c11', '-Wall', '-Wextra', '-O1', '-g', *SANITIZE]
runtime = {**environment(), 'UBSAN_OPTIONS': 'halt_on_error=1', 'ASAN_OPTIONS': 'detect_leaks=0', 'TMPDIR': str(out)}


def build(root, binary, log):
    include = ['-I', root / 'include'] if root != ROOT else []
    units = [root / s if (root / s).exists() else ROOT / s for s in SOURCES]
    strict = ['-Werror'] if root == ROOT else []  # a mutant may leave a variable unused
    result = subprocess.run(list(map(str, [CLANG, *flags, *strict, *include, '-I', ROOT / 'include', '-I', ROOT / 'checks', *units,
                                           ROOT / 'checks/a733-video-cold.c', '-o', binary])),
                            capture_output=True, text=True, env=runtime)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, log.read_text()[-2000:]


def model(binary, *extra):
    return subprocess.run(list(map(str, [binary, jpeg, ROM, *extra])), capture_output=True, text=True, env=runtime)


binary = out / 'cold'
build(ROOT, binary, out / 'build.log')
runs = {}
for extra in ([], ['--reset']):
    result = model(binary, *extra)
    (out / ('cold' + ''.join(extra) + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr[-2000:]
    runs[' '.join(['cold', *extra])] = result.stdout.splitlines()
assert any(line.startswith('PASS: decoder cold start on the actual owner') for line in runs['cold'])
assert any(line.startswith('lifetime2 (CCU at the FEL words): result1') for line in runs['cold'])
assert any(line.startswith('service: result1') and 'adopted result1' in line for line in runs['cold'])
assert sum(line.startswith('vcu ') for line in runs['cold']) == 2
assert sum(line.startswith('fault') and ' closed' in line for line in runs['cold']) == 4
holds = [line for line in runs['cold'] if line.startswith('holds ')]
refused = []
for i, (path, old, new, name) in enumerate(MUTANTS):
    text = (ROOT / path).read_text()
    assert text.count(old) == 1, (name, old)
    root = out / 'mutants' / str(i)
    (root / Path(path).parent).mkdir(parents=True)
    (root / path).write_text(text.replace(old, new))
    binary = root / 'cold'
    build(root, binary, root / 'build.log')
    result = model(binary)
    (root / 'run.log').write_text(result.stdout + result.stderr)
    assert result.returncode != 0, ('mutant survived', name)
    refused.append(dict(source=path, mutant=name))
record = dict(rom_sha256=ROM_SHA256, fixture_sha256=hashlib.sha256(jpeg.read_bytes()).hexdigest(), runs=runs, holds=holds,
              mutants_refused=refused,
              inputs={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in CHECK_INPUTS})
(out / 'result.json').write_text(json.dumps(record, indent=1) + '\n')
print(f'PASS video cold start on the power-domain owner: PLL_VE0, domain 2, critical-gate and two-lifetime cases, '
      f'{len(refused)} mutants refused')
