#!/usr/bin/env python3
"""Exercise selected provider clock admissions on modeled register reads."""
import argparse, hashlib, json, re, shlex, subprocess
from pathlib import Path
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
ROOT=Path(__file__).resolve().parents[1]
def block(text,name):
    start=re.search(r'(?:static )?(?:int|enum a733_pins_result) '+name+r'\(',text).start()
    opening=text.index('{',start);end=opening+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def table(text,name):
    return re.search(r'static const [^;]*\b'+name+r'\[[^;]+;',text).group()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def input_identity(source):
    raw=subprocess.check_output(['/usr/bin/clang-21','-MM','-I'+str(ROOT/'include'),str(source)],text=True)
    return {str(Path(v).resolve()):digest(Path(v)) for v in shlex.split(raw.replace('\\\n',' ').split(':',1)[1])}
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--resume',action='store_true');ap.add_argument('--providers',nargs='+',choices=['twi','usb2','radio','ce','i2s','pwm']);a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume)
    common='''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "a733_clock_reference.h"
#define CCU UINT32_C(0x02002000)
#define BIT(n) (UINT32_C(1)<<(n))
#define B(n) BIT(n)
static struct {uintptr_t address;uint32_t value;} registers[96];
static unsigned count,writes;
static uint32_t *word(uintptr_t address){for(unsigned i=0;i<count;i++)if(registers[i].address==address)return &registers[i].value;assert(count<96);registers[count].address=address;return &registers[count++].value;}
static void initial(unsigned osc,unsigned n,unsigned m){memset(registers,0,sizeof registers);count=writes=0;*word(0x07090160)=0x10000003|(osc<<14);*word(CCU)=0xf8000000|((n-1)<<8)|((m-1)<<16);*word(CCU+0xa0)=0xfe000000|(99<<8)|(1<<20)|(2<<16)|(4<<2);*word(CCU+0x1908)=0x0fff0fff;*word(CCU+0x1988)=0x0fff0000;}
'''
    cases=[('twi','drivers/i2c/a733_twi_native.c','a733_twi_native_prepare'),('usb2','drivers/usb/a733_usb2_platform.c','parents'),('radio','drivers/net/a733_radio_platform.c','profile'),('ce','drivers/crypto/a733_ce.c','clocks'),('i2s','drivers/audio/a733_i2s_platform.c','reference'),('pwm','drivers/pwm/a733_pwm.c','profile')]
    results=json.loads((a.output/'completed.json').read_text()) if a.resume else []
    for name,path,fn in cases:
        if a.providers and name not in a.providers:continue
        prior=next((v for v in results if v['provider']==name),None)
        if prior:
            assert prior['sha256']==digest(ROOT/path) and prior['helper_sha256']==digest(ROOT/'include/a733_clock_reference.h'),'Refuse stale successful evidence'
            assert prior['inputs']==input_identity(a.output/(name+'.c')),'Refuse changed successful check inputs'
            continue
        text=(ROOT/path).read_text();actual=block(text,fn)
        if name in ('i2s','pwm'):
            typ,header=('a733_audio_parent','a733_i2s_platform.h') if name=='i2s' else ('a733_pwm','a733_pwm.h')
            extra='uint64_t deadline,' if name=='i2s' else ''
            ignored='(void)deadline;' if name=='i2s' else ''
            setup=f'#include "{header}"\n#define OSC UINT32_C(0x07090160)\nstatic struct {typ} p;\nstatic int rd(struct {typ} *p,{extra}uintptr_t address,uint32_t *value){{(void)p;{ignored}*value=*word(address);return 1;}}\nstatic void prepare(void){{memset(&p,0,sizeof p);}}\nstatic int test(void);\n'
        elif name=='twi':
            setup='''#include "a733_twi.h"
#define A733_PLATFORM_REGISTER_LOCK 0
static int current(struct a733_twi_native *p){(void)p;return 1;}
static int healthy(struct a733_twi_native *p){(void)p;return 1;}
static int now(struct a733_twi_native *p,uint64_t *v){(void)p;*v=1;return 1;}
static int before(struct a733_twi_native *p,uint64_t d){(void)p;return d>1;}
static int a733_platform_lock_enter(struct a733_platform_locks *p,unsigned n,uint64_t d){(void)p;(void)n;(void)d;return 1;}
static void a733_platform_lock_leave(struct a733_platform_locks *p,unsigned n){(void)p;(void)n;}
static uint32_t load(uintptr_t a){return *word(a);}
static int observed(struct a733_twi_native *p,uintptr_t a,uint32_t m,uint32_t v,uint64_t d){(void)p;(void)d;return (*word(a)&m)==v;}
static int update(struct a733_twi_native *p,uintptr_t a,uint32_t m,uint32_t v,uint64_t d){(void)p;(void)d;*word(a)=(*word(a)&~m)|(v&m);writes++;return 1;}
static void retain(struct a733_twi_native *p){p->fault=1;}
enum a733_pins_result a733_pins_held_banked(struct a733_pins *p,const struct a733_pin_lease_banked *t,int *v,uint64_t d){(void)p;(void)t;(void)d;*v=1;return A733_PINS_OK;}
int a733_pin_fields_banked(unsigned b,unsigned p,struct a733_pin_fields *f){(void)b;memset(f,0,sizeof(*f));f->mux_address=0x200+p;return 1;}
static struct a733_twi_native p;
static void prepare(void){memset(&p,0,sizeof p);p.self=&p;*word(A733_TWI11_BASE+0x10)=0xf8;*word(A733_TWI11_BASE+0x20)=0x30;*word(0x207)=8;*word(0x208)=8;}
static int test(void){return a733_twi_native_prepare(&p,100)==1;}
'''
        else:
            typ={'usb2':'a733_usb2_platform','radio':'a733_radio_platform','ce':'a733_ce'}[name]
            setup=f'#include "{typ}.h"\nstatic struct {typ} p;\nstatic int rd(struct {typ} *p,uintptr_t a,uint32_t *v){{(void)p;*v=*word(a);return 1;}}\n'
            if name=='usb2':
                setup+=''.join(table(text,t)+'\n' for t in ['fabric_off','fabric_mask','parent_mhz'])
                setup+='static int ck(struct a733_usb2_platform *p,uintptr_t a,uint32_t m,uint32_t v){uint32_t r;return rd(p,a,&r)&&(r&m)==v;}\n'
                body='*word(CCU+0x13c0)=0x81000005;*word(CCU+0x13c4)=BIT(16);*word(CCU+0x5c0)=BIT(8);*word(CCU+0x5e0)=BIT(28);*word(CCU+0x1b28)=0x20000;*word(0x070101ac)=1;*word(0x07067000)=*word(0x07067008)=*word(0x07068000)=*word(0x07068008)=8;'
            elif name=='radio':
                setup+=''.join(table(text,t)+'\n' for t in ['fabric_off','fabric_mask'])
                body='*word(CCU+0x5c0)=BIT(24);*word(CCU+0x5e0)=BIT(30);*word(CCU+0x59c)=0x30001;'
            else:
                setup+=''.join(table(text,t)+'\n' for t in ['fabric_offset','fabric_mask','parent_mhz'])
                setup+='static int bits(struct a733_ce *p,uintptr_t a,uint32_t m,uint32_t v){uint32_t r;return rd(p,a,&r)&&(r&m)==v;}\n'
                body='p.config.profile=A733_CE_TRNG_ONLY;*word(CCU+0xac0)=0x81000000;*word(CCU+0xac4)=0x30003;*word(CCU+0x5e4)=4;'
            setup+=f'static void prepare(void){{memset(&p,0,sizeof p);{body}}}\nstatic int test(void);\n'
        tail=''
        if name!='twi':tail=f'static int test(void){{return {fn}(&p);}}\n'
        main='''int main(void){unsigned osc[]={0,3,2,1},nn[]={96,96,96,100},mm[]={96,96,104,80};
for(unsigned i=0;i<4;i++){initial(osc[i],nn[i],mm[i]);prepare();assert(test());}
for(unsigned bad=0;bad<5;bad++){initial(2,96,104);prepare();switch(bad){case 0:*word(CCU)=0xf8000000|(95<<8)|(95<<16);break;case 1:*word(0x07090160)&=~BIT(28);break;case 2:*word(0x07090160)&=~BIT(1);break;case 3:*word(CCU)&=~BIT(28);break;case 4:*word(CCU)=0xf8000000|(78<<8)|(78<<16);break;}assert(!test());assert(!writes);}
'''
        if name=='ce':main+='initial(2,96,104);prepare();*word(0x07090160)&=~1u;assert(!test());p.config.profile=A733_CE_SHA256_ONLY;assert(test());\n'
        if name=='pwm':
            main+='initial(2,96,104);prepare();p.shared=A733_CLOCK_PERI0;p.config.apb0=0x03000000;*word(CCU+0x510)=p.config.apb0;assert(test());*word(CCU+0xa0)^=1u<<8;assert(!test());\n'
            main+='for(unsigned i=0;i<4;i++){initial(osc[i],nn[i],mm[i]);prepare();p.config.led_enabled=1;assert(test()==(i<2));p.config.apbs0=0x04000000;*word(0x0701000c)=p.config.apbs0;assert(test());*word(0x0701000c)^=1;assert(!test());}\n'
        if name in ('twi','radio'):
            # Exercise the PERI0 route as well as the direct SYS24M route.
            if name=='twi':setroute='p.config.apb1=0x03000000;*word(CCU+0x518)=p.config.apb1;'
            else:setroute='p.shared=A733_CLOCK_PERI0;p.selected_mhz[0]=600;p.selected_mhz[1]=p.selected_mhz[2]=24;'
            main+='initial(2,96,104);prepare();'+setroute+'assert(test());\n'
        main+='puts("PASS '+name+' normalized reference admission");return 0;}\n'
        generated=a.output/(name+'.c');generated.write_text(common+setup+actual+'\n'+tail+main)
        cmd=['/usr/bin/clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'include'),str(generated),'-o',str(a.output/name)]
        for phase,command in [('build',cmd),('run',[str(a.output/name)])]:
            v=subprocess.run(command,cwd=ROOT,capture_output=True,text=True);(a.output/(name+'-'+phase+'.log')).write_text(v.stdout+v.stderr)
            if v.returncode:raise RuntimeError(name+' '+phase+' failed; see preserved log')
        results.append({'provider':name,'passed':True,'source':path,'sha256':digest(ROOT/path),'helper_sha256':digest(ROOT/'include/a733_clock_reference.h'),'function':fn,'extracted_function_sha256':hashlib.sha256(actual.encode()).hexdigest(),'inputs':input_identity(generated),'command':cmd})
        (a.output/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
    (a.output/'result.json').write_text(json.dumps({'physical_access':False,'cases':results,'helper_sha256':digest(ROOT/'include/a733_clock_reference.h'),'scope':'Actual function bodies extracted from the selected providers and compiled against actual public structs. Register reads and preexisting ownership/lock/time/pin dependencies are modeled; no native startup, whole-provider qualification, hardware or physical frequency measurement.'},indent=2)+'\n')
if __name__=='__main__':main()
