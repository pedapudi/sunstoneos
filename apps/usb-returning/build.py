#!/usr/bin/env python3
"""Build the finite USB client on the returning platform and the real providers.

Pass --platform-build with a central CMake build whose a733-power-probe target
supplies the platform; platform and providers then come from the current tree
and only the five accepted TinyUSB objects are reused. With --base-monitor,
the image links that monitor.c object in place of the central one: the
board-qualified base image's monitor, built with the monitor's counter option,
which --base-bundle's equivalence record names and whose image --base-run ran
on the board with status 0. The counter option only sets the EL1 event-counter
access registers before EL1 entry and adds no field to the result record. --baseline only
reproduces the 2026-10-18 evidence: its accepted provider objects predate the
current provider headers and are refused for current source."""
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode=True;sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from returning_build import CENTRAL_PLATFORM,PROVIDER_UNITS,ROOT,digest,image_object_closures,platform_arguments,platform_from,save

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
platform_arguments(p)
p.add_argument('--base-monitor',type=Path,help='monitor.c object of the board-qualified base image, linked in place of the central one')
p.add_argument('--base-bundle',type=Path,help='frozen base bundle whose equivalence.json names --base-monitor')
p.add_argument('--base-run',type=Path,help='session directory of that base image\'s passing board run')
p.add_argument('--transport',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-transport-only-20261018')
# Per-object compiler dependencies of the accepted TinyUSB objects.
p.add_argument('--transport-records',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-integration-20261018/frozen/image-objects.json')
a=p.parse_args();r=ROOT;e=a.evidence.resolve();transport=a.transport.resolve()
if not a.platform_build and not a.baseline:
    p.error('pass --platform-build; --baseline only reproduces the 2026-10-18 evidence')
# The accepted TinyUSB objects exist only in evidence.
central=a.platform_build is not None
platform=platform_from(a,e)
base_monitor=None
if a.base_monitor or a.base_bundle or a.base_run:
    if not (central and a.base_monitor and a.base_bundle and a.base_run):
        p.error('--base-monitor, --base-bundle and --base-run go together, with --platform-build')
    equivalence=json.loads((a.base_bundle/'equivalence.json').read_text())
    run=json.loads((a.base_run/'session.json').read_text())
    if digest(a.base_monitor)!=equivalence['objects']['platform/a733/monitor.c.obj']['sha256'] or \
       equivalence['image_sha256']!=digest(a.base_bundle/'image.elf'):
        p.error('--base-monitor is not the monitor object of the base bundle\'s image')
    if not (run.get('complete') is True and run['result']['status']==0 and run.get('image_sha256')==digest(a.base_bundle/'probe.bin')):
        p.error('--base-run is not a passing board run of the base bundle\'s image')
    platform.replace_platform_object('platform/a733/monitor.c',a.base_monitor.resolve())
    base_monitor={'object':str(a.base_monitor.resolve()),'sha256':digest(a.base_monitor),
                  'base_bundle':str(a.base_bundle.resolve()),'base_image_sha256':equivalence['image_sha256'],
                  'base_run':str(a.base_run.resolve()),'base_run_image_sha256':run['image_sha256']}
tiny=Path.home()/'.cache/sunstoneos/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src'
flags=['-DSUNSTONE_A733_USB=1',f'-I{r}/platform/a733-storage-update',f'-I{r}/drivers/usb',f'-I{tiny}']
config_pin=Path.home()/'.cache/sunstoneos/evidence/gpu-software-failure-20261018/snapshot/inputs/188-tusb_config.h'
assert digest(r/'platform/a733-storage-update/tusb_config.h')==digest(config_pin)
# Recompile the layout-dependent provider for this platform's FreeRTOS config.
platform.compile([r/'apps/usb-returning/main.c',r/'platform/a733/native_usb.c',r/'drivers/usb/a733_usb_platform.c',r/'drivers/usb/a733_usb_platform_native.c'],flags)
names=list(PROVIDER_UNITS) if central else [Path(pin['path']).stem for pin in platform.accepted['new_objects'] if Path(pin['path']).name!='00-pmic-provider-probe.obj']
# TinyUSB uses OPT_OS_NONE. Its objects consume no FreeRTOS layouts or APIs.
usb=[item for item in json.loads((transport/'reused-objects.json').read_text())
     if any(Path(item['path']).name.endswith(name) for name in ('dcd_sunxi_musb.c.obj','tusb.c.obj','tusb_fifo.c.obj','usbd.c.obj','vendor_device.c.obj'))]
recorded=image_object_closures(a.transport_records,'apps/gpu-render/main.c')
platform.reuse(names,usb,{x['sha256']:recorded[x['sha256']] for x in usb if 'accepted' in x and x['sha256'] in recorded})
assert len(platform.reused)==(len(CENTRAL_PLATFORM)+5 if central else 31),len(platform.reused)
platform.link();platform.binary();platform.run()
platform.record(__file__,{'transport_input_records_sha256':digest(transport/'input-identities.json'),'base_monitor':base_monitor})
for root,name in [(transport,'transport')]+([] if central else [(a.baseline.resolve(),'baseline')]):
 for source in ['input-identities.json','reused-objects.json']:
  shutil.copy2(root/source,e/(name+'-'+source))
platform.copy_helpers('usb-returning',('host-check.py','check.py','prepare.py','README.md'))
print(f'Built bounded USB returning image; {len(platform.reused)} reused objects and four new units.')
# Freeze the runnable host closure, including the updated kernel observer hook.
host_files=['tools/check-a733-usb-returning.py','tools/check-a733-kernel.py','tools/usb-access.py','tools/check-a733-smp.py']
host_inputs={}
for name in host_files:
 source=r/name;dest=e/'host'/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 host_inputs[name]=digest(source)
# The existing runner deliberately uses the configured external guard path.
# Record that runtime dependency; do not substitute another implementation.
external=Path.home()/'git/mimus/tools/usb_guard.py';host_inputs[str(external)]=digest(external)
save(e,'host-inputs.json',host_inputs)
# Preserve the reviewed ROM bytes/disassembly and the actual compiled PIO source.
return_sources={}
source_paths=[Path.home()/'.cache/sunstoneos/evidence/rom-copy-20261016-084055'/name for name in ('physical-rom.bin','physical-rom.dis','control-flow.json')]
source_paths += [Path.home()/'.cache/sunstoneos/sources/brom'/name for name in ('ns-brom.bin','ns-brom.dis')]
ancestor=Path.home()/'.cache/sunstoneos/evidence/gpu-software-failure-20261018/snapshot/manifest.json'
for item in json.loads(ancestor.read_text())['inputs']:
 if any(part in item['source']['path'] for part in ('dcd_sunxi_musb.c','musb_def.h','vendor_device.c','tusb_config.h')):
  source=Path(item['frozen']['path']);assert digest(source)==item['frozen']['sha256'];source_paths.append(source)
for i,source in enumerate(source_paths):
 dest=e/'return-sources'/f'{i:02d}-{source.name}';dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 return_sources[str(source)]={'snapshot':str(dest.relative_to(e)),'sha256':digest(source)}
save(e,'return-source-identities.json',return_sources)
