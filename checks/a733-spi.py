#!/usr/bin/env python3
"""One focused SPI source, host-transfer/native-admission, and object batch."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--steps', nargs='+', choices=['reference','host','native','compile'],
                        default=['reference','host','native','compile'])
    parser.add_argument('--sources', type=Path, default=Path.home()/'.cache/sunstoneos/sources')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = [root/p for p in ['include/a733_spi.h', 'include/a733_spi_native.h',
        'drivers/spi/a733_spi.c', 'drivers/spi/a733_spi_native.c',
        'checks/a733-spi.c', 'checks/a733-spi-native.c', 'checks/a733-spi.py']]
    refs = [args.sources/p for p in ['spi/ng/spi-sunxi.h', 'spi/ng/spi-sunxi.c',
        'board-kernel/ccu-sun60iw2.c', 'board-kernel/ccu-sun60iw2-r.c',
        'board-kernel/sun60iw2p1.dtsi', 'board-kernel/sun60i-a733-orangepi-zero3w.dts',
        'board-kernel/pinctrl-sun60iw2.c', 'spi/upstream-spi-sun6i.c', 'spi/upstream.json',
        'camera-research/A733_User_Manual_V1.00.txt']]
    snapshots = []
    for i,p in enumerate(inputs+refs):
        target=out/f'input-{i:02d}-{p.name}';shutil.copyfile(p,target)
        snapshots.append(dict(path=str(p),archive=target.name,sha256=digest(target)))
    (out/'inputs.json').write_text(json.dumps(snapshots,indent=2)+'\n')
    if 'reference' in args.steps:
        runtime=inputs[2].read_text();reference=refs[0].read_text()
        offsets={'GCR':'GC','TCR':'TC','IER':'INT_CTL','ISR':'INT_STA','FCR':'FIFO_CTL',
                 'FSR':'FIFO_STA','WCR':'WAIT_CNT','MBC':'MBC','MTC':'MTC','BCC':'BCC',
                 'TXD':'TXDATA','RXD':'RXDATA','BSR':'BUF_STA'}
        facts={}
        for local,external in offsets.items():
            a=int(re.search(r'#define '+local+r' (0x[0-9a-f]+)u',runtime)[1],16)
            b=int(re.search(r'#define SUNXI_SPI_'+external+r'_REG\s+\((0x[0-9A-Fa-f]+)\)',reference)[1],16)
            assert a==b,(local,a,b)
            facts[local]=a
        dtsi=refs[4].read_text()
        for port in range(6):
            node=f'spi{port}' if port<5 else 'r_spi'
            body=re.search(r'\b'+node+r': spi@[^}]+}',dtsi)[0]
            base=0x02540000+port*0x1000 if port<5 else 0x07092000
            irq=24+port if port<5 else 210
            assert f'0x{base:08x}' in body and f'GIC_SPI {irq} ' in body
        ccu=refs[2].read_text()
        for port,offset in enumerate([0xf00,0xf08,0xf10,0xf20,0xf28]):
            body=re.search(r'SUNXI_CCU_MP_WITH_MUX_GATE_NO_INDEX\(spi'+str(port)+r'_clk,.*?\);',ccu,re.S)[0]
            assert f'0x{offset:04X}' in body and re.search(r'0, 5,.*?8, 5,.*?24, 3,',body,re.S)
        manual=refs[-1].read_text()
        assert '0xc0:192bytes in RX FIFO & RX BUFFER' in manual
        assert 'SPI_CLK = Clock Source/M/N' in manual
        assert 'SUN6I_CLK_CTL_REG' in refs[-3].read_text()  # Older controller differs.
        (out/'reference.json').write_text(json.dumps(dict(offsets=facts,ports=6,
            rx_combined_max=192,clock='24 MHz / ((M+1)*(N+1))',result='pass'),indent=2)+'\n')
    clang=shutil.which('clang');assert clang
    def run(name,cmd):
        result=subprocess.run(cmd,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (out/(name+'.log')).write_text(result.stdout)
        if result.returncode: raise RuntimeError(f'{name} failed; see {out/name}.log')
    run('toolchain',[clang,'--version'])
    flags=[clang,'-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-I'+str(root/'include')]
    if 'host' in args.steps:
        run('host-build',flags+[str(inputs[2]),str(inputs[4]),'-o',str(out/'host')])
        run('host',[str(out/'host')])
    if 'native' in args.steps:
        original=inputs[3].read_text()
        assembly='__asm__ volatile("dsb sy" ::: "memory")'
        assert original.count(assembly)==4 and original.count('__asm__')==4
        transformed=original.replace(assembly,'__asm__ volatile("" ::: "memory")')
        (out/'native-host.c').write_text(transformed)
        run('native-host-build',flags+[str(inputs[2]),str(out/'native-host.c'),str(inputs[5]),'-o',str(out/'native-host')])
        run('native-host',[str(out/'native-host')])
    if 'compile' in args.steps:
        for name in ['a733_spi','a733_spi_native']:
            run(name+'-compile',[clang,'--target=aarch64-none-elf','-mcpu=cortex-a55','-ffreestanding',
                '-std=c11','-Wall','-Wextra','-Werror','-O2','-fstack-usage','-I'+str(root/'include'),
                '-c',str(root/'drivers/spi'/(name+'.c')),'-o',str(out/(name+'.o'))])
    results=dict(result='pass',steps=args.steps,scope='source comparison, host wire/FIFO/fault model, native C admission guards, AArch64 objects',
        physical=False,barrier_execution=False,native_host_transform='Only four dsb sy statements become compiler memory barriers.',
        native_original_sha256=digest(inputs[3]),native_host_sha256=digest(out/'native-host.c') if (out/'native-host.c').exists() else None,
        artifacts={p.name:digest(p) for p in out.iterdir() if p.is_file() and not p.name.startswith('input-')})
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(dict(result='pass',evidence=str(out))))

if __name__=='__main__':
    main()
