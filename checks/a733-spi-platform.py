#!/usr/bin/env python3
"""Focused real-provider SPI composition and native platform compilation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--steps',nargs='+',choices=['reference','host','native'],default=['reference','host','native'])
    args=ap.parse_args();root=Path(__file__).resolve().parents[1];cache=Path.home()/'.cache/sunstoneos'
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    source=['drivers/spi/a733_spi_platform.c','drivers/spi/a733_spi_platform_native.c',
        'include/a733_spi_platform.h','include/a733_spi_platform_native.h','drivers/spi/a733_spi.c',
        'include/a733_spi.h','drivers/clock/a733_clock_leases.c','include/a733_clock_leases.h',
        'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','include/a733_pins.h',
        'checks/a733-spi-platform.c','checks/a733-spi-platform.py']
    refs=['board-kernel/pinctrl-sun60iw2.c','board-kernel/pinctrl-sun60iw2-r.c',
        'board-kernel/sun60iw2p1.dtsi','board-kernel/ccu-sun60iw2.c','board-kernel/ccu-sun60iw2-r.c',
        'gpu-power/drivers/clk/sunxi-ng/ccu_reset.c','camera-research/A733_User_Manual_V1.00.txt']
    records=[]
    def archive(p):
        p=p.resolve()
        if any(x['path']==str(p) for x in records):return
        dest=out/f'input-{len(records):03d}-{p.name}';shutil.copyfile(p,dest)
        records.append(dict(path=str(p),sha256=sha(p),archive=dest.name))
        (out/'inputs.json').write_text(json.dumps(records,indent=2)+'\n')
    for p in [root/n for n in source]+[cache/'sources'/n for n in refs]:archive(p)
    clang=shutil.which('clang');assert clang
    commands=[]
    def run(name,cmd):
        commands.append(dict(name=name,command=cmd));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        p=subprocess.run(cmd,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (out/(name+'.log')).write_text(p.stdout)
        if p.returncode:raise RuntimeError(f'{name}: see {out/name}.log')
    if 'reference' in args.steps:
        text=(root/source[0]).read_text();main=(cache/'sources'/refs[0]).read_text();r=(cache/'sources'/refs[1]).read_text()
        assert '#if IS_ENABLED(CONFIG_AW_FPGA_S4) || IS_ENABLED(CONFIG_AW_FPGA_V7)' in main
        main=main.split('#else',1)[1]  # Real silicon table, not FPGA pad remapping.
        routes=re.findall(r'\{(\d+),A733_PIN_([BCDELM]),(\d+),(\d+),(\d+),(\d+),\{([^}]+)\}\}',text)
        assert len(routes)==6
        for port,bank,mux,clk,mosi,miso,css in routes:
            pins=[int(clk),int(mosi),int(miso)]+[int(x) for x in css.split(',') if int(x)!=255]
            for pin in pins:
                src=r if bank in 'LM' else main
                start=src.index(f'SUNXI_PIN(SUNXI_PINCTRL_PIN({bank}, {pin}),')
                tail=src[start:];end=tail.find('\n\tSUNXI_PIN(',1);block=tail if end<0 else tail[:end]
                func='s_spi0' if port=='5' else 'spi'+port
                assert re.search(r'SUNXI_FUNCTION\(0x'+format(int(mux),'x')+r',\s*"'+func+r'"',block),(bank,pin,mux)
        dts=(cache/'sources'/refs[2]).read_text();assert 'ccu: ccu@2002000' in dts
        reset=(cache/'sources'/refs[5]).read_text();assert 'ccu_reset_assert(rcdev, id);\n\tudelay(10);\n\tccu_reset_deassert(rcdev, id);' in reset
        (out/'reference.json').write_text(json.dumps(dict(result='pass',routes=routes,reset_hold_us=10,
            spi4='Controller descriptor retained; no unproven package pin mapping admitted.'),indent=2)+'\n')
    run('toolchain',[clang,'--version'])
    if 'host' in args.steps:
        sources=[source[i] for i in [0,4,6,8,9,11]]
        cmd=[clang,'-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-I'+str(root/'include')]
        run('host-build',cmd+[str(root/p) for p in sources]+['-o',str(out/'host')])
        run('host',[str(out/'host')])
    if 'native' in args.steps:
        freertos=cache/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
        port=freertos/'portable/GCC/ARM_AARCH64_SRE'
        flags=[clang,'--target=aarch64-none-elf','-mcpu=cortex-a55','-mstrict-align','-ffreestanding','-fno-builtin',
            '-std=c11','-Wall','-Wextra','-Werror','-O2','-fstack-usage','-DINCLUDE_xTaskGetSchedulerState=1',
            '-DINCLUDE_xTaskGetCurrentTaskHandle=1','-DINCLUDE_xSemaphoreGetMutexHolder=1',
            '-I'+str(root/'include'),'-I'+str(root/'runtime/include'),'-I'+str(freertos/'include'),'-I'+str(port)]
        for unit in ['a733_spi_platform','a733_spi_platform_native']:
            sourcefile=root/'drivers/spi'/(unit+'.c')
            run(unit+'-deps',flags+['-M',str(sourcefile)])
            dep=(out/(unit+'-deps.log')).read_text().replace('\\\n',' ')
            for name in dep.split(':',1)[1].split():archive(Path(name))
            run(unit+'-compile',flags+['-c',str(sourcefile),'-o',str(out/(unit+'.o'))])
    (out/'results.json').write_text(json.dumps(dict(result='pass',steps=args.steps,physical=False,
        old_wire_checks_replayed=False,artifacts={p.name:sha(p) for p in out.iterdir() if p.is_file() and not p.name.startswith('input-')}),indent=2)+'\n')
    print(json.dumps(dict(result='pass',evidence=str(out))))

if __name__=='__main__':main()
