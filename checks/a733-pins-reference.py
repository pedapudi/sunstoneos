#!/usr/bin/env python3
"""Compare pin fields and admitted encodings with the pinned A733 sources."""
import argparse
import hashlib
import pathlib
import re
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-dir',type=pathlib.Path,required=True)
parser.add_argument('--soc-source',type=pathlib.Path,required=True)
parser.add_argument('--build-dir',type=pathlib.Path,required=True)
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
args.build_dir.mkdir(parents=True,exist_ok=True)
inputs=[(args.source_dir/'pinctrl-sunxi.c','3bc32a1fd6be151be68979a7c44fe400adb657f21cb66f4e043d31aea4273406'),
        (args.source_dir/'pinctrl-sunxi.h','cd345d552621857017e5a34c6c182c3fcc630d2394174c99edd9f2c135e9cab8'),
        (args.soc_source,'ab05ddccecd152edf3fafd238ade22299aebe3448646f02f0551edef0c6e1ec7')]
texts=[]
for path,digest in inputs:
    data=path.read_bytes()
    if hashlib.sha256(data).hexdigest()!=digest: raise SystemExit(f'Pinned input mismatch: {path}')
    texts.append(data.decode())
c,h,soc=texts

def braces(text,start):
    end=text.index('{',start)+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]

notice=c[:c.index('#include')]
structure=braces(h,h.index('struct sunxi_pinctrl_hw_info {'))+';'
entries=[]
for kind in (4,10):
    start=c.index('{',c.index(f'/* SUNXI_PCTL_HW_TYPE_{kind} */'))
    entries.append(f'[{kind}]='+braces(c,start))
functions=[]
for name in ['sunxi_pinctrl_recalc_offset','sunxi_mux_reg','sunxi_mux_offset',
             'sunxi_dlevel_reg','sunxi_dlevel_offset','sunxi_pull_reg','sunxi_pull_offset',
             'sunxi_data_reg','sunxi_irq_hw_bank_num','sunxi_irq_ctrl_reg_from_bank','sunxi_irq_ctrl_reg']:
    functions.append(braces(h,h.index('static inline u32 '+name+'(')))
# The second pin table is the real IC, following the FPGA branch.
ic=soc.split('#else',1)[1]
allowed=[]
for pin in range(4,9):
    start=ic.index(f'SUNXI_PIN(SUNXI_PINCTRL_PIN(B, {pin}),')
    end=ic.index('\n\tSUNXI_PIN(',start+1)
    values=[int(v,16) for v in re.findall(r'SUNXI_FUNCTION(?:_IRQ_BANK)?\(0x([0-9a-f]+),',ic[start:end])]
    allowed.append(sum(1<<v for v in values))
# Derive admitted drive codes from the reference setter's argument range.
setter=c[c.index('static int sunxi_pconf_set'):]
drive=re.search(r'case PIN_CONFIG_DRIVE_STRENGTH:\s*if \(arg < (\d+) \|\| arg > (\d+)\)',setter)
assert drive and re.search(r'val = arg / 10 - 1;',setter)
lo,hi=(int(v)//10-1 for v in drive.groups())
for name,val in [('DISABLE',0),('PULL_UP',1),('PULL_DOWN',2)]:
    assert re.search(r'case PIN_CONFIG_BIAS_'+name+r':(?:(?!case ).)*?val = '+str(val)+r';',setter,re.S)
stubs=r'''
#include "a733_pins.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
enum sunxi_pinctrl_hw_type {SUNXI_PCTL_HW_TYPE_4=4,SUNXI_PCTL_HW_TYPE_10=10};
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
    unsigned bank_map[]={1};
    for(unsigned variant=0;variant<2;++variant) {
        enum sunxi_pinctrl_hw_type type=variant?SUNXI_PCTL_HW_TYPE_10:SUNXI_PCTL_HW_TYPE_4;
        struct sunxi_pinctrl_desc desc={type,1,bank_map};
        assert(sunxi_irq_ctrl_reg(&desc,4)==0x150);
        assert(sunxi_data_reg(32+4,type)==0x110);
        for(unsigned pin=4;pin<=8;++pin) {
            struct a733_pin_fields f;
            assert(a733_pin_fields(pin,&f));
            assert(f.mux_address==A733_PINS_BASE+sunxi_mux_reg(pin+32,type));
            assert(f.mux_shift==sunxi_mux_offset(pin+32));
            assert(f.pull_address==A733_PINS_BASE+sunxi_pull_reg(pin+32,type));
            assert(f.pull_shift==sunxi_pull_offset(pin+32));
            assert(f.drive_address==A733_PINS_BASE+sunxi_dlevel_reg(pin+32,type));
            assert(f.drive_shift==sunxi_dlevel_offset(pin+32,type));
        }
    }
    for(unsigned pin=4;pin<=8;++pin)
      for(unsigned function=0;function<=16;++function)
       for(unsigned pull=0;pull<=3;++pull)
        for(unsigned drive=0;drive<=16;++drive) {
            struct a733_pin_config c={function,pull,drive};
            int expected=function<16 && (allowed[pin-4]&(1u<<function)) &&
                         function!=1 && function!=14 && pull<=2 && drive<=max_drive;
            assert(!!a733_pin_config_valid(pin,c)==!!expected);
        }
    struct a733_pin_fields unchanged={0};
    assert(!a733_pin_fields(3,&unchanged)&&!a733_pin_fields(9,&unchanged));
    puts("Pinned pin reference: both silicon profiles, five pin routes, and reserved mux/pull/drive refusals passed");
}
'''
assert lo==0
file=args.build_dir/'reference.c'
file.write_text(notice+stubs+structure+'\nstruct sunxi_pinctrl_hw_info sunxi_pinctrl_hw_info[11]={'+','.join(entries)+'};\n'+
                '\n'.join(functions)+'\nstatic const unsigned allowed[]={'+','.join(map(str,allowed))+'};\n'+
                f'static const unsigned max_drive={hi};\n'+main)
binary=args.build_dir/'reference'
subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-I'+str(root/'include'),str(file),str(root/'drivers/pinctrl/a733_pins.c'),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
