#!/usr/bin/env python3
"""Exercise terminal SD close with actual providers and modeled card/MMIO."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--failures-only', action='store_true', help='only idle, update-stall, gate and partial-release failures')
    p.add_argument('--admission', action='store_true', help='only close refusal boundaries')
    a = p.parse_args(); e = a.output.resolve(); e.mkdir(parents=True, exist_ok=False)
    raw = (ROOT / 'checks/sunxi-mmc.c').read_text()
    (e / 'mmc-card-model.h').write_text(raw[raw.index('struct model {'):raw.index('static int barrier(void *ctx)')])
    raw = (ROOT / 'checks/a733-mmc-platform.c').read_text()
    (e / 'mmc-platform-fixture.h').write_text(raw[:raw.index('int main(')])
    raw = (ROOT / 'checks/a733-mmc-uhs.c').read_text(); raw = raw[:raw.index('int main(')]
    assert raw.count('.sd_profile = SUNXI_MMC_SD_UHS_SDR104') == 1
    (e / 'mmc-high-speed-fixture.h').write_text(raw.replace('.sd_profile = SUNXI_MMC_SD_UHS_SDR104',
                                                        '.sd_profile = selected_profile'))
    sources = ['drivers/storage/a733_storage_update.c', 'drivers/storage/storage_digest.c',
               'drivers/storage/a733_mmc_platform.c', 'drivers/storage/sunxi_mmc.c',
               'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
               'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c',
               'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c', 'checks/sd-return-close.c']
    flags = [shutil.which('clang-21'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-fno-omit-frame-pointer', '-Iinclude', '-Ichecks', '-I'+str(e)]
    commands = []

    def run(command, name):
        commands.append(list(map(str, command)))
        (e / 'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
        r = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (e / (name+'.log')).write_text(r.stdout)
        if r.returncode: raise SystemExit(name+': '+r.stdout)
        return r.stdout

    paths = {Path(__file__), *(ROOT / s for s in sources),
             *(ROOT / ('checks/'+s) for s in ('sunxi-mmc.c', 'a733-mmc-platform.c', 'a733-mmc-uhs.c'))}
    for i, source in enumerate(sources):
        dep = e / f'{i}.d'; run(flags+['-M', '-MF', str(dep), source], f'dep-{i}')
        paths.update((ROOT / s).resolve() for s in dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split())
    inputs = []
    for source in sorted(paths):
        data = source.read_bytes(); sha = hashlib.sha256(data).hexdigest(); dest = e / 'inputs' / sha
        dest.parent.mkdir(exist_ok=True); dest.write_bytes(data)
        inputs.append(dict(path=str(source), sha256=sha, snapshot=str(dest.relative_to(e))))
    (e / 'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    run(flags+sources+['-o', str(e / 'close')], 'build')
    print(run([str(e / 'close')]+(['--admission'] if a.admission else ['--failures-only'] if a.failures_only else []), 'run'), end='')
    assert all(hashlib.sha256(Path(r['path']).read_bytes()).hexdigest()==r['sha256'] for r in inputs)
    (e / 'result.json').write_text(json.dumps(dict(result='pass', physical=False,
        scope='actual provider close under ASan/UBSan; existing card/PMIC model, no protocol suite replay'), indent=2)+'\n')


if __name__ == '__main__': main()
