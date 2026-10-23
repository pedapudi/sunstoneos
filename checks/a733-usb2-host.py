#!/usr/bin/env python3
"""Focused USB2 platform/xHCI/TinyUSB composition; no board access."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def block(text, name):
    start = re.search(r'(?:static )?(?:int|void) '+name+r'\(', text).start()
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--typec', type=Path, required=True)
    p.add_argument('--twi', type=Path, required=True)
    p.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
    p.add_argument('--port', type=Path, required=True)
    p.add_argument('--cases',nargs='+',help='Only selected affected composition cases')
    p.add_argument('--compile-only',action='store_true',help='Compile selected host/native units without executing composition models')
    p.add_argument('--resume', action='store_true', help='Only missing steps; refuses changed frozen inputs')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=a.resume)
    compiler = Path(shutil.which('clang-21')).resolve()
    interpreter = Path(sys.executable).resolve()
    resource = Path(subprocess.check_output([str(compiler), '-print-resource-dir'], text=True).strip())
    identities = {'compiler': {'path': str(compiler), 'sha256': hashlib.sha256(compiler.read_bytes()).hexdigest(),
        'version': subprocess.check_output([str(compiler), '--version'], text=True)},
        'python': {'path': str(interpreter), 'sha256': hashlib.sha256(interpreter.read_bytes()).hexdigest(), 'version': sys.version},
        'sanitizer_archives': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (resource/'lib/linux').glob('libclang_rt.*x86_64.a') if 'asan' in p.name or 'ubsan' in p.name}}
    if a.resume:
        assert json.loads((out/'tools.json').read_text()) == identities
    else:
        (out/'tools.json').write_text(json.dumps(identities, indent=2)+'\n')
    tiny = a.sources/'tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src'
    freertos = a.sources/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include'
    core_source = a.sources/'usb2-research/drivers/usb/dwc3/core.c'
    core_header = a.sources/'usb2-native/core.h'
    units = [root/u for u in ['checks/a733-usb2-host.c','drivers/usb/a733_usb2_platform.c',
        'drivers/usb/a733_xhci.c','drivers/usb/a733_xhci_tinyusb.c','drivers/usb/a733_usb3_phy.c','drivers/power/a733_pmic.c',
        'drivers/power/a733_rail_leases.c','drivers/clock/a733_clock_leases.c',
        'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/gpu/a733_gpu_resources.c',
        'runtime/device_memory_pool.c']]
    units += [a.typec/'drivers/usb/a733_typec.c', a.typec/'drivers/usb/a733_typec_pd.c', a.twi/'drivers/i2c/a733_twi.c']
    units += [tiny/u for u in ['host/hub.c','tusb.c','host/usbh.c','common/tusb_fifo.c','class/msc/msc_host.c','class/hid/hid_host.c']]
    native = [root/u for u in ['drivers/usb/a733_usb2_platform.c','drivers/usb/a733_usb2_native.c',
        'drivers/usb/a733_xhci.c','drivers/usb/a733_xhci_tinyusb.c','drivers/usb/a733_usb3_phy.c']]
    cadence = a.sources/'pcie/kernel/bsp/drivers/phy/sunxi-cadence-combophy.c'
    paths = set(units+native+[cadence,root/'drivers/usb/a733_usb3_phy.h',root/'drivers/usb/host/cadence-source.py',root/'drivers/usb/host/superspeed.py',Path(__file__).resolve(),root/'checks/a733-pmic.c',core_source,core_header,
        a.typec/'include/a733_typec.h',a.twi/'include/a733_twi.h',root/'drivers/usb/host/tusb_config.h',root/'drivers/usb/host/hub.patch'])
    for directory in [root/'include',root/'runtime/include',root/'platform/a733',freertos,a.port]:
        paths.update(directory.glob('*.h'))
    paths.update(tiny.rglob('*.h'))
    def saved(path):
        return out/'inputs'/str(path.resolve()).lstrip('/')
    hashes = {}
    for path in sorted(paths):
        data = path.read_bytes()
        hashes[str(path.resolve())] = hashlib.sha256(data).hexdigest()
        target = saved(path)
        if a.resume:
            assert target.read_bytes() == data, f'Changed frozen input: {path}'
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    if a.resume:
        assert json.loads((out/'inputs.json').read_text()) == hashes
    else:
        (out/'inputs.json').write_text(json.dumps(hashes, indent=2)+'\n')
    # Compile the actual pinned hub class with the reviewed narrow adaptation.
    hub_source=saved(tiny/'host/hub.c');hub_native=out/'hub-native.c'
    subprocess.run(['patch','--silent','--output',str(hub_native),str(hub_source),str(saved(root/'drivers/usb/host/hub.patch'))],check=True)
    report_path = out/'steps.json'
    steps = json.loads(report_path.read_text()) if a.resume and report_path.exists() else {}
    def run(label, command):
        if steps.get(label, {}).get('exit') == 0:
            return
        command = [str(x) for x in command]
        r = subprocess.run(command, capture_output=True, text=True)
        (out/(label+'.log')).write_text(r.stdout+r.stderr)
        steps[label] = {'command': command, 'exit': r.returncode}
        report_path.write_text(json.dumps(steps, indent=2)+'\n')
        if r.returncode:
            raise RuntimeError(f'{label} failed; preserved {out/(label+".log")}')
    run('ss-source',[interpreter,saved(root/'drivers/usb/host/cadence-source.py'),'--source',saved(cadence),'--output',out/'ss-cadence.inc'])
    run('ss-stack',[interpreter,saved(root/'drivers/usb/host/superspeed.py'),'--source',saved(tiny),'--output',out/'tinyusb-ss'])
    # Exact native mapping loop, compiled on host with a translation model.
    mapping = '#include <assert.h>\n#include "a733_mapping_check.h"\n'
    mapping += '''static unsigned attributes=0x44,shareability=2,bad_last,shift;
int aarch64_device_translate(uintptr_t a,int w,struct aarch64_translation *t) {
assert(w);*t=(struct aarch64_translation){a+shift,attributes,shareability};
return !(bad_last&&a==0x79001000);}
'''
    mapping += block((root/'drivers/usb/a733_usb2_native.c').read_text(), 'mappings')
    mapping += '''
void usb2_native_mapping_check(void) {
assert(mappings(0x79000000,8192,0x44));shareability=3;assert(mappings(0x79000000,8192,0x44));
shareability=1;assert(!mappings(0x79000000,8192,0x44));shareability=2;
attributes=0xff;assert(!mappings(0x79000000,8192,0x44));attributes=0x44;
shift=4096;assert(!mappings(0x79000000,8192,0x44));shift=0;
bad_last=1;assert(!mappings(0x79000000,8192,0x44));bad_last=0;
assert(!mappings(UINTPTR_MAX-4095,8192,0x44));}
'''
    (out/'mapping.c').write_text(mapping)
    # Published reference function and original notices; no recreated formula.
    source = core_source.read_text()
    ref = source[:source.index('#include')]+'''\n#include <stdint.h>
typedef uint32_t u32;typedef uint64_t u64;
#define NSEC_PER_SEC 1000000000UL
#define DWC3_GUCTL 0xc12c
#define DWC3_GFLADJ 0xc630
#define DWC3_GUCTL_REFCLKPER_MASK 0xffc00000u
#define DWC3_GFLADJ_REFCLK_FLADJ_MASK 0x003fff00u
#define DWC3_GFLADJ_240MHZDECR 0x7f000000u
#define DWC3_GFLADJ_240MHZDECR_PLS1 0x80000000u
#define DWC3_GFLADJ_REFCLK_LPM_SEL 0x00800000u
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m))&(m))
#define DWC3_VER_IS_PRIOR(...) 0
#define div64_u64(a,b) ((a)/(b))
struct dwc3 {void *ref_clk;unsigned long ref_clk_per;unsigned gfladj_refclk_lpm_sel;u32 *regs;};
static unsigned long clk_get_rate(void *p){(void)p;return 24000000;}
static u32 dwc3_readl(u32 *p,unsigned off){return p[off==DWC3_GUCTL?0:1];}
static void dwc3_writel(u32 *p,unsigned off,u32 v){p[off==DWC3_GUCTL?0:1]=v;}
'''+block(source,'dwc3_ref_clk_period')+'''
void usb2_source_reference(u32 *out){out[0]=out[1]=0;struct dwc3 d={.ref_clk=out,.regs=out};dwc3_ref_clk_period(&d);}
'''
    (out/'reference.c').write_text(ref)
    includes = [saved(root/'drivers/usb/host'),saved(root/'include'),saved(a.typec/'include'),
        saved(a.twi/'include'),saved(root/'checks'),saved(tiny),saved(tiny/'host')]
    includes.insert(1,out/'tinyusb-ss');includes.append(out)
    flags = [compiler,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer']+['-I'+str(i) for i in includes]
    run('host-build',flags+[hub_native if u==tiny/'host/hub.c' else out/'tinyusb-ss'/u.relative_to(tiny) if u.is_relative_to(tiny) else saved(u) for u in units]+[out/'mapping.c',out/'reference.c','-o',out/'composed'])
    loaded = subprocess.check_output(['ldd', str(out/'composed')], text=True)
    libraries = {}
    for line in loaded.splitlines():
        for word in line.split():
            if word.startswith('/') and Path(word).is_file():
                libraries[word] = hashlib.sha256(Path(word).read_bytes()).hexdigest()
    (out/'loaded-libraries.json').write_text(json.dumps({'ldd': loaded, 'sha256': libraries}, indent=2)+'\n')
    cases=['admission','read','busy-read','busy-stuck','busy-stuck-platform','busy-stuck-lock','mutex-loss','bad-config','short-descriptor','bad-status','short-data','cancel','detach','unplug','hid-hs-unplug','hub-unplug','unplug-stuck','hub-detach','hub-detach-enabled','ss-hub-detach']
    cases=[] if a.compile_only else a.cases or cases
    for case in cases:
        run(case,[out/'composed',case,*([out/(case+'-reports.bin')] if case.startswith('companion-') or case.startswith('ss-') or case.startswith('irq-ss-') or case.startswith('msc-stall') or case.startswith('irq-msc-stall') or case.startswith('dual-') or case.startswith('irq-dual-') or case in ('hid-fs','hid-ls','hid-hs','hub-hid-fs','hub-hid-ls','hub-hid-hs','hub-mtt-ls','hub-port7','irq-hub-hid-fs') else [])])
    native_flags=[compiler,'--target=aarch64-none-elf','-march=armv8-a','-mstrict-align','-mgeneral-regs-only',
        '-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin',
        '-DINCLUDE_xTaskGetSchedulerState=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1','-DINCLUDE_xSemaphoreGetMutexHolder=1']
    native_flags += ['-I'+str(i) for i in includes+[saved(root/'runtime/include'),saved(root/'platform/a733'),saved(freertos),saved(a.port)]]
    for i, unit in enumerate(native):
        run('native-'+str(i),native_flags+['-c',saved(unit),'-o',out/('native-'+str(i)+'.o')])
    result={'result':'PASS','cases':cases,'native_objects':len(native),'hardware_access':False,
        'typec_boundary':'Actual snapshot/revoke from a prepared observation checkpoint; reuse separate role-engine evidence.',
        'artifact_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [out/'composed',*out.glob('native-*.o')]}}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    main()
