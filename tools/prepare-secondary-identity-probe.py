#!/usr/bin/env python3
"""Extend the demonstrated primary-return overlay with a sequential CPU survey."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

from elftools.elf.elffile import ELFFile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('primary_host',ROOT/'tools/primary-publication-host.py')
primary=importlib.util.module_from_spec(spec);spec.loader.exec_module(primary)
require,digest=primary.require,primary.digest
EXTENSION=0x41021000
REPORT=(0x42018100,0x900)


def branch(address,target):
    displacement=target-address
    require(displacement%4==0 and -(1<<27)<=displacement<(1<<27),'Branch out of range.')
    return 0x14000000|((displacement//4)&0x3ffffff)


def prepare(args):
    local=getattr(args,"local",False)
    report=(REPORT[0],0x1b00) if local else REPORT
    base=json.loads((args.primary/'manifest.json').read_text())
    frozen,reservations=primary.load(args.primary,args.image,base)
    with args.image.open('rb') as stream:
        elf=ELFFile(stream)
        entries=list(elf.get_section_by_name('.symtab').iter_symbols())
        def address(name):
            values=[s['st_value'] for s in entries if s.name==name and s['st_shndx']!='SHN_UNDEF']
            require(len(values)==1,'Missing or ambiguous native symbol '+name)
            return values[0]
        names=('a733_cpu_start_init','a733_cpu_entry_native_init','a733_cpu_entry_poll',
               'a733_cpu_secondary_entry','a733_cpu_secondary_continue','a733_cpu_index',
               'a733_smp_counter','a733_smp_power_address','memset','memcpy')
        if local:names+=('prepare_core','a733_cpu_continue_init','a733_cpu_continue_poll','a733_cached_el3_vectors')
        native={n:address(n) for n in names}
    require(all(report[0]+report[1]<=a or a+n<=report[0] for _,a,n in reservations),
            'Identity report overlaps an existing allocation.')
    require(len(frozen['overlay.bin'])<=EXTENSION-primary.probe.OVERLAY[0], 'Base overlay exceeds extension boundary.')
    args.output.mkdir(parents=True,exist_ok=False)
    link=args.output/'identity.ld'
    link.write_text('''ENTRY(identity_primary_gate)
SECTIONS {
 . = 0x41021000;
 .text : { *(.text .text.*) *(.rodata .rodata.*) }
 .data : { *(.data .data.*) }
 .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
 ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "Explicit report storage only")
 ASSERT(. <= 0x41024000, "Identity extension exceeds reservation")
 /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
''')
    source='boot/a733/secondary-identity-probe'
    commands=[]
    units=[(source+'.'+suffix,'identity-'+suffix+'.o') for suffix in ('c','S')]
    if local:units += [('boot/a733/secondary-local-probe.c','local.o'),('boot/a733/secondary-distributor-probe.c','distributor.o')]
    for unit,object_name in units:
        commands.append([args.clang,'--target=aarch64-none-elf','-march=armv8-a','-mgeneral-regs-only',
            '-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffunction-sections','-fdata-sections',
            '-Wall','-Wextra','-Werror',*(['-DSUNSTONE_SECONDARY_LOCAL=1'] if local else []),'-I',ROOT/'include','-I',ROOT/'platform/a733-smp-cached',
            '-I',ROOT/'boot/a733','-c',ROOT/unit,'-o',args.output/object_name])
    symbols=[f'--defsym={name}={value:#x}' for name,value in native.items()]
    return_address=base['hooks']['primary_publication_return']
    symbols.append(f'--defsym=identity_original_return_next={return_address+4:#x}')
    commands.extend([[args.lld,'-T',link,'--gc-sections',*symbols,*[args.output/name for _,name in units],
                      '-o',args.output/'identity.elf'],
                     [args.objcopy,'-O','binary',args.output/'identity.elf',args.output/'identity.bin']])
    for command in commands:
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (args.output/'build.log').open('a') as stream:stream.write(result.stdout+result.stderr)
        require(result.returncode==0,'Identity extension build failed; inspect build.log.')
    with (args.output/'identity.elf').open('rb') as stream:
        hooks={s.name:s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
               if s.name.startswith('identity_')}
    overlay=bytearray(frozen['overlay.bin'])
    offset=return_address-primary.probe.OVERLAY[0]
    word=struct.unpack_from('<I',overlay,offset)[0]
    require(word&0xff00001f==0x58000000,'Return checkpoint no longer loads x0 from a literal.')
    displacement=(word>>5)&0x7ffff
    if displacement&0x40000:displacement-=0x80000
    require(struct.unpack_from('<Q',overlay,offset+4*displacement)[0]==0x08001004,'Return destination differs.')
    replacement=branch(return_address,hooks['identity_primary_gate'])
    struct.pack_into('<I',overlay,offset,replacement)
    overlay+=bytes(EXTENSION-primary.probe.OVERLAY[0]-len(overlay))
    overlay+=(args.output/'identity.bin').read_bytes()
    require(len(overlay)<=primary.probe.OVERLAY[1],'Composite overlay too large.')
    (args.output/'overlay.bin').write_bytes(overlay)
    payloads=[];patches=[{'address':return_address,'original':word,'replacement':replacement,'space':'overlay'}]
    target=native['a733_cpu_secondary_continue'];patched=0
    for item in base['payloads']:
        data=bytearray(frozen[item['name']]);start=item['address']
        if start<=target<start+len(data):
            offset=target-start;word=struct.unpack_from('<I',data,offset)[0]
            require(word==0xd5384244,'Secondary continuation entry differs from reviewed MRS x4,CurrentEL.')
            replacement=branch(target,hooks['identity_target']);struct.pack_into('<I',data,offset,replacement)
            patches.append({'address':target,'original':word,'replacement':replacement,'space':'native'});patched+=1
        path=args.output/item['name'];path.write_bytes(data)
        payloads.append(dict(item,sha256=digest(path)))
    require(patched==1,'Secondary patch did not match exactly one segment.')
    names=(source+'.c',source+'.S',source+'.h','include/a733_cpu_entry.h','include/a733_cpu_start.h',
           'platform/a733-smp-cached/boot.h','tools/prepare-secondary-identity-probe.py')
    if local:names+=('boot/a733/secondary-local-probe.c','boot/a733/secondary-local-probe.h',
                     'boot/a733/secondary-distributor-probe.c','include/a733_cpu_continue.h')
    manifest={'schema':'a733-secondary-local-probe-v1' if local else 'a733-secondary-identity-probe-v1','physical_access':False,'native_sha256':primary.probe.NATIVE,
        'primary_manifest':{'path':str((args.primary/'manifest.json').resolve()),'sha256':digest(args.primary/'manifest.json')},
        'sources':{n:digest(ROOT/n) for n in names},'native_symbols':native,'hooks':hooks,'patches':patches,
        'overlay':dict(base['overlay'],sha256=digest(args.output/'overlay.bin')),'payloads':payloads,
        'identity_report':{'address':report[0],'bytes':report[1]},
        'reservations':reservations+[('identity-report',*report)],'commands':[[str(x) for x in c] for c in commands],
        'files':{p.name:digest(p) for p in args.output.iterdir() if p.is_file()}}
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Prepared sequential '+('local initialization for cores1..7' if local else 'identity survey for cores2..7')+'; no board access.')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','primary','output','clang','lld','objcopy'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument("--local",action="store_true",help="Include actual CPU preparation and local GIC setup/restoration")
    prepare(parser.parse_args())
