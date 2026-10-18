#!/usr/bin/env python3
"""Compose maintenance supervision with actual storage; no hardware access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence', required=True, type=Path)
    p.add_argument('--native-tree', required=True, type=Path)
    p.add_argument('--case', choices=['completion-and-final-drain','pending-deadline','loss','cutoff-guard'], action='append')
    p.add_argument('--core-only', action='store_true', help='Reuse unchanged native hardware-adapter evidence.')
    p.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = a.evidence.resolve()
    out.mkdir(parents=True, exist_ok=False)
    # Reuse the existing hardware model, without executing its prior scenarios.
    card = (root/'checks/sunxi-mmc.c').read_text()
    (out/'mmc-card-model.h').write_text(card[card.index('struct model {'):card.index('static int barrier(void *ctx)')])
    provider = (root/'checks/a733-mmc-platform.c').read_text()
    before = 'storage_leave,storage_retain,storage_read,storage_write,storage_barrier,storage_outside'
    assert provider.count(before) == 1
    (out/'storage-provider-model.h').write_text(provider.replace(before,
        'storage_leave,storage_retain,update_read,update_write,storage_barrier,storage_outside'))
    service = (root/'checks/a733-storage-update.c').read_text()
    assert service.count('int main(int argc,char **argv)') == 1
    (out/'storage-service-model.h').write_text(service.replace('int main(int argc,char **argv)',
        'int inherited_storage_cases_not_run(int argc,char **argv)'))
    cc = shutil.which('clang-21') or shutil.which('clang')
    assert cc
    kernel = a.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    common = ['-std=c11','-Wall','-Wextra','-Werror','-Iinclude','-Idrivers/storage','-Ichecks',
        '-I'+str(out),'-I'+str(a.native_tree.resolve()/'include'),'-I'+str(kernel/'include'),
        '-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),
        '-DINCLUDE_xSemaphoreGetMutexHolder=1','-DINCLUDE_xTaskGetSchedulerState=1',
        '-DINCLUDE_xTaskGetCurrentTaskHandle=1']
    sources = ['drivers/watchdog/a733_maintenance_watchdog.c','drivers/storage/a733_storage_update.c',
        'drivers/storage/storage_digest.c','drivers/storage/storage_update_usb.c',
        'drivers/storage/a733_mmc_platform.c','drivers/storage/sunxi_mmc.c',
        'drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c',
        'drivers/power/a733_pmic.c','drivers/gpu/a733_gpu_resources.c',
        'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','checks/maintenance-watchdog.c']
    native = ['drivers/watchdog/a733_maintenance_watchdog.c','platform/a733/maintenance_watchdog.c']
    if a.core_only:
        native = native[:1]
    host_flags = [cc,'-O1','-g','-fsanitize=address,undefined']+common
    native_flags = [cc,'--target=aarch64-none-elf','-O2','-ffreestanding','-fno-builtin','-Iruntime/include']+common
    commands = []
    def run(cmd, name):
        cmd = [str(x) for x in cmd]
        commands.append(cmd)
        (out/'commands.json').write_text(json.dumps(commands, indent=2))
        r = subprocess.run(cmd, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/name).write_text(r.stdout)
        if r.returncode:
            print(r.stdout)
            raise SystemExit(r.returncode)
    for i, src in enumerate(sources):
        run(host_flags+['-M','-MF',out/f'host-{i}.d',src],f'host-dependencies-{i}.log')
    for i, src in enumerate(native):
        run(native_flags+['-M','-MF',out/f'native-{i}.d',src],f'native-dependencies-{i}.log')
    paths = {root/x for x in sources+native+['checks/maintenance-watchdog.py',
        'checks/sunxi-mmc.c','checks/a733-mmc-platform.c','checks/a733-storage-update.c']}
    paths.add(Path(cc).resolve())
    for dep in out.glob('*.d'):
        paths.update((root/x).resolve() for x in dep.read_text().replace('\\\n',' ').split(':',1)[1].split())
    records = []
    for path in sorted(paths):
        data = path.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        target = out/'inputs'/sha
        target.parent.mkdir(exist_ok=True)
        if not target.exists():
            target.write_bytes(data)
        records.append(dict(path=str(path),sha256=sha,snapshot=str(target.relative_to(out))))
    (out/'inputs.json').write_text(json.dumps(records,indent=2))
    run(host_flags+sources+['-o',out/'maintenance'], 'host-build.log')
    cases = a.case or ['completion-and-final-drain','pending-deadline','loss','cutoff-guard']
    for case in cases:
        run([out/'maintenance',case],case+'.log')
    for i, src in enumerate(native):
        run(native_flags+['-c',src,'-o',out/f'native-{i}.o'],f'native-build-{i}.log')
    artifacts = []
    for path in [out/'maintenance']+sorted(out.glob('*.o'))+sorted(out.glob('*.log')):
        artifacts.append(dict(path=path.name,sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    (out/'results.json').write_text(json.dumps(dict(cases=cases,native_objects=len(native),artifacts=artifacts,
        physical_qualification=False,prior_suites_executed=False),indent=2))
    print('Maintenance composition and changed native objects passed.')


if __name__ == '__main__':
    main()
