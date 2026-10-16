#!/usr/bin/env python3
"""Run the focused bank expansion checks and preserve exact evidence inputs."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sources',type=pathlib.Path,default=pathlib.Path.home()/'.cache/sunstoneos/sources')
parser.add_argument('--evidence',type=pathlib.Path,required=True)
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
args.evidence.mkdir(parents=True,exist_ok=False)
commands=[]
def run(command):
    commands.append(command)
    result=subprocess.run(command,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    with (args.evidence/'run.log').open('a') as f: f.write(result.stdout)
    (args.evidence/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    print(result.stdout,end='')
    if result.returncode: raise SystemExit(result.returncode)

def braces(text,start):
    end=text.index('{',start)+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]

pins=[('pins/pinctrl-sunxi.c','3bc32a1fd6be151be68979a7c44fe400adb657f21cb66f4e043d31aea4273406'),
      ('pins/pinctrl-sunxi.h','cd345d552621857017e5a34c6c182c3fcc630d2394174c99edd9f2c135e9cab8'),
      ('board-kernel/pinctrl-sun60iw2.c','ab05ddccecd152edf3fafd238ade22299aebe3448646f02f0551edef0c6e1ec7'),
      ('board-kernel/pinctrl-sun60iw2-r.c','431fac7b767f5be39015cdf67fdd3dec230cbc1ebeb2fa0c0de08ca0f6e223a1'),
      ('camera-research/A733_User_Manual_V1.00.pdf','cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d')]
for name,digest in pins:
    assert hashlib.sha256((args.sources/name).read_bytes()).hexdigest()==digest,name
c=(args.sources/pins[0][0]).read_text(); h=(args.sources/pins[1][0]).read_text()
notice=c[:c.index('#include')]
structure=braces(h,h.index('struct sunxi_pinctrl_hw_info {'))+';'
profiles=[]
for kind in (1,4,10):
    start=c.index('{',c.index(f'/* SUNXI_PCTL_HW_TYPE_{kind} */'))
    profiles.append(f'[{kind}]='+braces(c,start))
functions=[]
for name in ['sunxi_pinctrl_recalc_offset','sunxi_mux_reg','sunxi_mux_offset',
             'sunxi_dlevel_reg','sunxi_dlevel_offset','sunxi_pull_reg','sunxi_pull_offset',
             'sunxi_data_reg','sunxi_irq_hw_bank_num','sunxi_irq_ctrl_reg_from_bank','sunxi_irq_ctrl_reg']:
    functions.append(braces(h,h.index('static inline u32 '+name+'(')))
expected=[]
for name,_ in pins[2:4]:
    soc=(args.sources/name).read_text().split('#else',1)[1].split('#endif',1)[0]
    starts=list(re.finditer(r'SUNXI_PIN\(SUNXI_PINCTRL_PIN\(([B-M]), (\d+)\),',soc))
    for i,entry in enumerate(starts):
        bank,pin=entry.groups(); body=soc[entry.end():starts[i+1].start() if i+1<len(starts) else len(soc)]
        codes={int(v,16) for v in re.findall(r'SUNXI_FUNCTION(?:_IRQ_BANK)?\(0x([0-9a-f]+),',body)}
        uart=[0]*16
        for f,n in re.findall(r'SUNXI_FUNCTION\(0x([0-9a-f]+), "((?:s_)?uart[0-6])"\)',body):
            uart[int(f,16)]=int(n[-1])+(8 if n.startswith('s_') else 1)
        expected.append('{'+f'{ord(bank)-65},{pin},{sum(1<<v for v in codes)}'+',{'+','.join(map(str,uart))+'}}')
assert len(expected)==201
stubs=r'''
#include "a733_pins_native.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
enum sunxi_pinctrl_hw_type {SUNXI_PCTL_HW_TYPE_1=1,SUNXI_PCTL_HW_TYPE_4=4,SUNXI_PCTL_HW_TYPE_10=10};
#define PINS_PER_BANK 32
#define MUX_PINS_PER_REG 8
#define MUX_PINS_BITS 4
#define PULL_PINS_PER_REG 16
#define PULL_PINS_BITS 2
#define DATA_PINS_PER_REG 32
#define IRQ_PER_BANK 32
#define round_down(x,a) ((x)&~((a)-1))
#define BUG_ON(x) assert(!(x))
struct sunxi_pinctrl_desc {enum sunxi_pinctrl_hw_type hw_type;unsigned irq_banks; const unsigned *irq_bank_map;};
'''
main=r'''
int main(void)
{
    unsigned counts[A733_PIN_BANKS]={0};
    for (unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        unsigned b=expected[i].bank,p=expected[i].pin; ++counts[b];
        const struct a733_pin_descriptor *d=a733_pin_descriptor(b,p);
        assert(d && d->functions==expected[i].functions);
        for (unsigned f=0;f<16;++f) assert(d->uart[f]==expected[i].uart[f]);
        for (unsigned variant=0;variant<2;++variant) {
            unsigned bank=b<A733_PIN_L ? b : b-A733_PIN_L;
            enum sunxi_pinctrl_hw_type type=b<A733_PIN_L ? (variant?10:4) : 1;
            unsigned pin=bank*32+p,map[]={bank};
            struct sunxi_pinctrl_desc desc={type,1,map};
            uintptr_t base=b<A733_PIN_L ? A733_PINS_BASE : A733_PINS_R_BASE;
            struct a733_pin_fields f; assert(a733_pin_fields_banked(b,p,&f));
            assert(f.mux_address==base+sunxi_mux_reg(pin,type));
            assert(f.mux_shift==sunxi_mux_offset(pin));
            assert(f.pull_address==base+sunxi_pull_reg(pin,type));
            assert(f.pull_shift==sunxi_pull_offset(pin));
            assert(f.drive_address==base+sunxi_dlevel_reg(pin,type));
            assert(f.drive_shift==sunxi_dlevel_offset(pin,type));
            assert(a733_pins_native_address(f.mux_address,1));
            assert(a733_pins_native_address(f.pull_address,1));
            assert(a733_pins_native_address(f.drive_address,1));
            assert(a733_pins_native_address(base+sunxi_irq_ctrl_reg(&desc,p),1));
            assert(a733_pins_native_address(base+sunxi_data_reg(pin,type),0));
            assert(!!a733_pins_native_address(base+sunxi_data_reg(pin,type),1)==(b>=A733_PIN_L));
        }
        for (unsigned fn=0;fn<=16;++fn) for (unsigned pull=0;pull<=3;++pull) for (unsigned drive=0;drive<=4;++drive) {
            struct a733_pin_config c={fn,pull,drive};
            int allowed=fn<16 && (d->functions&(1u<<fn)) && pull<=2 && drive<=3;
            assert(!!a733_pin_config_valid_banked(b,p,c)==!!allowed);
            if (b==A733_PIN_B && p>=4 && p<=8)
                assert(!!a733_pin_config_valid(p,c)==!!(allowed && fn!=1 && fn!=14));
        }
    }
    for (unsigned b=1;b<A733_PIN_BANKS;++b) {
        assert(a733_pin_bank_mask(b)==((1u<<counts[b])-1));
        assert(!a733_pin_descriptor(b,counts[b]));
    }
    assert(!a733_pin_descriptor(0,0) && !a733_pin_descriptor(13,0));
    puts("Pinned pin reference: 201 pins, all mux admissions, UART identities, main profiles 4/10 and always-on profile 1 passed");
}
'''
reference=args.evidence/'reference.c'
reference.write_text(notice+stubs+structure+'\nstruct sunxi_pinctrl_hw_info sunxi_pinctrl_hw_info[11]={'+','.join(profiles)+'};\n'+
    '\n'.join(functions)+'\nstatic const struct {unsigned bank,pin,functions;uint8_t uart[16];} expected[]={'+','.join(expected)+'};\n'+main)
files=['include/a733_pins.h','include/a733_pins_native.h','drivers/pinctrl/a733_pins.c',
       'drivers/pinctrl/a733_pin_tables.c','drivers/pinctrl/a733_pins_native.c',
       'checks/pins-expanded.c','checks/pins-expanded.py','checks/a733-pins.c','checks/a733-pins-deadline.c']
inputs=[]
for path in [root/f for f in files]+[args.sources/name for name,_ in pins]+[reference]:
    data=path.read_bytes(); inputs.append({'path':str(path),'sha256':hashlib.sha256(data).hexdigest()})
(args.evidence/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
common=['cc','-D_XOPEN_SOURCE=700','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-pthread','-I'+str(root/'include')]
implementation=[str(root/'drivers/pinctrl/a733_pins.c'),str(root/'drivers/pinctrl/a733_pin_tables.c')]
objects=[]
for source in implementation:
    obj=args.evidence/(pathlib.Path(source).stem+'-host.o')
    run(common+['-MD','-MF',str(obj.with_suffix('.d')),'-c',source,'-o',str(obj)])
    objects.append(str(obj))
for name,source in [('expanded',root/'checks/pins-expanded.c'),('reference',reference),
                    ('legacy',root/'checks/a733-pins.c'),('legacy-deadline',root/'checks/a733-pins-deadline.c')]:
    binary=args.evidence/name
    obj=args.evidence/(name+'.o')
    run(common+['-MD','-MF',str(obj.with_suffix('.d')),'-c',str(source),'-o',str(obj)])
    run(common+[str(obj)]+objects+['-o',str(binary)])
    run([str(binary)])
for source in implementation+[str(root/'drivers/pinctrl/a733_pins_native.c')]:
    obj=args.evidence/(pathlib.Path(source).stem+'-native.o')
    run(['clang','--target=aarch64-none-elf','-std=c11','-O2','-ffreestanding','-Wall','-Wextra','-Werror',
         '-I'+str(root/'include'),'-MD','-MF',str(obj.with_suffix('.d')),'-c',source,'-o',str(obj)])
# Preserve compiler-reported inputs once, including system headers, and retain
# the exact compiler identities. No test execution follows this snapshot.
paths={pathlib.Path(row['path']) for row in inputs}
for dep in args.evidence.glob('*.d'):
    names=dep.read_text().replace('\\\n',' ').split(':',1)[1].split()
    paths.update((root/name).resolve() for name in names)
paths.update(pathlib.Path(shutil.which(tool)).resolve() for tool in ('cc','clang'))
records=[]
for path in sorted(paths):
    data=path.read_bytes(); digest=hashlib.sha256(data).hexdigest()
    destination=args.evidence/'inputs'/digest
    destination.parent.mkdir(exist_ok=True)
    if not destination.exists(): destination.write_bytes(data)
    records.append({'path':str(path),'sha256':digest,'snapshot':str(destination.relative_to(args.evidence))})
(args.evidence/'inputs.json').write_text(json.dumps(records,indent=2)+'\n')
artifacts=[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
           for p in args.evidence.iterdir() if p.is_file() and p.name!='artifacts.json']
(args.evidence/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')
