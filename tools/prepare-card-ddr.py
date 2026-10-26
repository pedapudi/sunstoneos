#!/usr/bin/env python3
"""Build an unqualified optional boot0 restore image from pinned ordinary files."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PIN = '829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3'
KIND = 'a733-card-dram-restore'
BASE = 0x47000
PATCHES = {0x5e216: 'ebf7e1fc', 0x56142: 'f5f7b1fd', 0x48d6c: '53ec0e2f'}


def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),ROOT/'tools'/f'{name}.py')
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result)
    return result


def sha(data): return hashlib.sha256(data).hexdigest()


def checksum(data):
    return (sum(struct.unpack('<61440I',data))-struct.unpack_from('<I',data,12)[0]+0x5f0a6c39)&0xffffffff


def validate_variant(manifest_path, original):
    package=module('prepare-microsd-boot')
    raw=package.read_regular(manifest_path,65536)
    m=json.loads(raw,object_pairs_hook=package.unique_object)
    if (m.get('schema')!=1 or m.get('kind')!=KIND or m.get('original_sha256')!=PIN
            or sha(original)!=PIN or m.get('qualification')!={'physical_boot':False,'thermal':False,'memory':False}):
        raise ValueError('Unrecognized card restore variant contract')
    data=package.read_regular(Path(manifest_path).parent/'boot0-card-restore.bin',len(original))
    if len(data)!=len(original) or m['files']['boot0-card-restore.bin']!={'bytes':len(data),'sha256':sha(data)}:
        raise ValueError('Derived boot0 identity differs from its contract')
    expected=bytearray(original)
    for address in PATCHES:
        offset=address-BASE;expected[offset:offset+4]=data[offset:offset+4]
    expected[0x6f4:0xaf4]=data[0x6f4:0xaf4]
    expected[12:16]=data[12:16]
    if bytes(expected)!=data or struct.unpack_from('<I',data,12)[0]!=checksum(data):
        raise ValueError('Derived boot0 modifies unapproved bytes or has an invalid checksum')
    if any(data[0x6f4+i*128:0x6f8+i*128]!=bytes(4) for i in range(8)):
        raise ValueError('Derived boot0 changes a parameter-slot presence word')
    units=m.get('units',{})
    if type(m.get('experimental_unknown_temperature')) is not bool or set(units)!={'A','B'}:
        raise ValueError('Variant lacks an explicit temperature policy or paired units')
    config=struct.unpack_from('<8I',data,0xac8)
    if config!=(0x44524343,1,int(m['experimental_unknown_temperature']),units['A']['first'],units['B']['first'],
                m['files']['card-ddr-helper.bin']['bytes'],m['helper_crc32'],240000000):
        raise ValueError('Variant configuration differs from its loaded bytes')
    for address,target in ((0x5e216,0x476f8),(0x56142,0x47978),(0x48d6c,0x477f8)):
        first,second=struct.unpack_from('<HH',data,address-BASE)
        if first&0xf800!=0xf000 or second&0xd000!=(0x9000 if address==0x48d6c else 0xd000):
            raise ValueError('Derived hook is not the required Thumb branch')
        sign=(first>>10)&1
        delta=(sign<<24)|((1^((second>>13)&1)^sign)<<23)|((1^((second>>11)&1)^sign)<<22)|((first&1023)<<12)|((second&2047)<<1)
        if sign: delta-=1<<25
        if address+4+delta!=target:
            raise ValueError('Derived hook branches outside its fixed entry')
    helper=package.read_regular(Path(manifest_path).parent/'card-ddr-helper.bin',32768)
    if (m['files']['card-ddr-helper.bin']!={'bytes':len(helper),'sha256':sha(helper)}
            or module('training-record').crc(helper)!=m['helper_crc32']):
        raise ValueError('Helper identity differs from its contract')
    return data,{'manifest_sha256':sha(raw),'sha256':sha(data),'original_sha256':PIN,
                 'experimental_unknown_temperature':m['experimental_unknown_temperature'],
                 'qualification':m['qualification'],'allocation_sha256':m['allocation_sha256']}


def prepare(args):
    package=module('prepare-microsd-boot');slots=module('boot-slots');records=module('training-record')
    original=package.pinned(args.boot0,0x3c000,PIN)
    raw,units,fd,sectors=records.paired_image(slots,args.allocation,args.backup)
    try:
        if any(not 40960 <= v['first'] <= 0xffffffff-8192 for v in units.values()):
            raise ValueError('Training unit leaves the 32-bit reader LBA range')
        # Only a preserved ordinary-file snapshot is inspected; this is no proof
        # that a physical card has these reservations or this identity.
        backup=hashlib.sha256()
        for offset in range(0,sectors,8192): backup.update(slots.read_at(fd,offset,min(8192,sectors-offset)))
        allocation=json.loads(raw)
        if allocation.get('backup')!={'bytes':sectors*512,'sha256':backup.hexdigest()}:
            raise ValueError('Allocation backup identity does not match the supplied snapshot')
    finally: os.close(fd)
    out=slots.new_directory(args.output)
    profile=records.profile(records.restore_set(args.restore_set))
    generated=('#define BOOT0_CHECKSUM 0x2fce7bb3u\n#define STRAP_CRC 0xf874a6a5u\n'
               f'#define PROFILE_COUNT {len(profile)}u\n'
               'static const struct { uint32_t address; uint16_t mask; } restore_profile[] = {\n')
    generated+=''.join(f'{{{a:#x}u,{m:#x}u}},\n' for a,m in profile)+'};\n'
    (out/'restore-profile.h').write_text(generated)
    ld=shutil.which('ld.lld') or str(Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld');objcopy=shutil.which('llvm-objcopy') or '/usr/lib/llvm-21/bin/llvm-objcopy'
    nm=shutil.which('llvm-nm') or '/usr/lib/llvm-21/bin/llvm-nm'
    def run(*cmd): subprocess.run([str(v) for v in cmd],check=True)
    def compile(source,obj,*flags):
        run('clang','--target=arm-none-eabi','-march=armv8-a','-marm','-Os','-ffreestanding',
            '-fno-builtin','-fno-stack-protector','-mno-unaligned-access','-Wall','-Wextra','-Werror',
            '-I',out,*flags,'-c',source,'-o',obj)
    # Bootstrap read/stop locations are fixed section entries; helper links them
    # directly without loading pointers from mutable card data.
    objects=[]
    for source in ('ddr-restore.c','card-ddr-main.c','card-ddr-helper.S'):
        obj=out/(source+'.o');compile(ROOT/'boot/a733'/source,obj,'-DCARD_RESTORE');objects.append(obj)
    script=out/'helper.ld'
    script.write_text('SECTIONS { . = 0x92000; .text : { *(.entry) *(.text*) *(.rodata*) } '
                      '/DISCARD/ : { *(.ARM.exidx*) *(.comment*) } }\n')
    # Stop is placed in slot14, read begins at slot11+4.
    run(ld,'-T',script,'-e','card_helper',*objects,'-o',out/'card-ddr-helper.elf')
    run(objcopy,'-O','binary',out/'card-ddr-helper.elf',out/'card-ddr-helper.bin')
    helper=(out/'card-ddr-helper.bin').read_bytes()
    linked=(out/'card-ddr-helper.elf').read_bytes();so=struct.unpack_from('<I',linked,32)[0]
    ss,sn=struct.unpack_from('<HH',linked,46)
    allocated=[]
    for i in range(sn):
        _,kind,flags,address,_,size,*_=struct.unpack_from('<10I',linked,so+i*ss)
        if flags&2 and size:
            if kind!=1 or not 0x92000<=address<address+size<=0x9a000:
                raise ValueError('Helper has uninitialized or out-of-SRAM sections')
            allocated.append({'address':address,'bytes':size})
    if not 0<len(helper)<=32768: raise ValueError('Helper exceeds SRAM/card reservation')
    defines={'UNIT_A':units['A']['first'],'UNIT_B':units['B']['first'],'HELPER_BYTES':len(helper),
             'HELPER_CRC':records.crc(helper),
             'EXPERIMENTAL':int(args.experimental_unknown_temperature)}
    compile(ROOT/'boot/a733/card-ddr-entry.S',out/'entry.o',
            *(f'-Wa,--defsym,{k}={v}' for k,v in defines.items()))
    script=out/'entry.ld'
    script.write_text('SECTIONS {\n'+''.join(
        f'.slot{i} {0x476f8+(i-8)*128:#x} : {{ *(.slot{i}) }}\n'
        f'ASSERT(SIZEOF(.slot{i}) <= 124, "parameter slot {i} overflow")\n' for i in range(8,16))+
        '/DISCARD/ : { *(.ARM.exidx*) *(.comment*) } }\n')
    symbols={'vendor_init':0x49bdd,'vendor_timer_tail':0x48d71,'vendor_read':0x5db09,
             'vendor_selector':0x5da65,'vendor_timing':0x4bca9}
    run(ld,'-T',script,'-e','card_boot',*(f'--defsym={k}={v}' for k,v in symbols.items()),
        out/'entry.o','-o',out/'entry.elf')
    names={line.split()[-1]:int(line.split()[0],16) for line in subprocess.check_output([nm,str(out/'entry.elf')],text=True).splitlines()
           if len(line.split())==3 and line.split()[1].upper()!='U'}
    if names['card_read']!=0x47878 or names['card_stop']!=0x479f8:
        raise ValueError('Helper entry symbols moved from their fixed ABI')
    image=bytearray(original);spans=[]
    for i in range(8,16):
        target=out/f'slot{i}.bin'
        run(objcopy,'-O','binary',f'--only-section=.slot{i}',out/'entry.elf',target)
        data=target.read_bytes();offset=0x6f8+(i-8)*128
        image[offset:offset+len(data)]=data;spans.append({'slot':i,'bytes':len(data)})
    for address,target in ((0x5e216,'card_boot'),(0x56142,'card_timing'),(0x48d6c,'card_timer')):
        offset=address-BASE
        if image[offset:offset+4].hex()!=PATCHES[address]:raise ValueError('Pinned instruction mismatch')
        padding=address%4
        assembly='.syntax unified\n.arch armv8-a\n.thumb\n.text\n'+('nop\n' if padding else '')
        assembly+=('b.w' if address==0x48d6c else 'bl')+' target\n'
        source=out/f'patch-{address:x}.S';source.write_text(assembly)
        compile(source,out/'patch.o')
        run(ld,f'-Ttext={address-padding:#x}','-e',str(address),f'--defsym=target={names[target]|1}',out/'patch.o','-o',out/'patch.elf')
        run(objcopy,'-O','binary',out/'patch.elf',out/'patch.bin')
        patch=(out/'patch.bin').read_bytes()[padding:]
        if len(patch)!=4:raise ValueError('Patch is not one Thumb branch')
        image[offset:offset+4]=patch
    struct.pack_into('<I',image,12,checksum(image))
    (out/'boot0-card-restore.bin').write_bytes(image)
    fd,_=slots.open_image(args.backup)
    writes=[(f'helper-{copy}',unit['first']+64,helper) for copy,unit in units.items()]
    records.write_plan(slots,out/'helper-plan',raw,sectors,writes,fd,
                       {'helper_bytes':len(helper),'helper_sha256':sha(helper)})
    manifest={'schema':1,'kind':KIND,'original_sha256':PIN,'allocation_sha256':sha(raw),
              'backup_sha256':backup.hexdigest(),'units':units,'experimental_unknown_temperature':args.experimental_unknown_temperature,
              'qualification':{'physical_boot':False,'thermal':False,'memory':False},
              'helper_crc32':records.crc(helper),'helper_sections':allocated,'bootstrap_sections':spans,
              'symbols':names,'counter_hz':24000000,'io_budget_ticks':240000000,
              'files':{name:{'bytes':len(data),'sha256':sha(data)} for name,data in
                       [('boot0-card-restore.bin',image),('card-ddr-helper.bin',helper)]},
              'sources':{name:sha((ROOT/name).read_bytes()) for name in
                         ('tools/prepare-card-ddr.py','tools/training-record.py','boot/a733/ddr-restore.c',
                          'boot/a733/card-ddr-entry.S','boot/a733/card-ddr-main.c','boot/a733/card-ddr-helper.S')}}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    validate_variant(out/'manifest.json',original)
    return manifest


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('boot0','restore-set','allocation','backup','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--experimental-unknown-temperature',action='store_true')
    a=p.parse_args()
    try: prepare(a)
    except (ValueError,OSError,subprocess.SubprocessError) as e:p.exit(1,str(e)+'\n')
    print(a.output)

if __name__=='__main__':main()
