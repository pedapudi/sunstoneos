#!/usr/bin/env python3
"""Compare thermal conversion and startup trace with pinned A733 sources.

Usage: a733-thermal.py SOURCE_CACHE_ROOT MODEL_LOG
No downloads or hardware access.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

cache,log=map(Path,sys.argv[1:])
files={
    "thermal/sunxi_thermal.c":"e0b67fa2bbda72d9f226dd910560d9a31169f671b07383c95831edfbe2be2c02",
    "thermal/sunxi-sid.c":"79190843de4dc734f56dd0c198ca75b557c83b2ee485b9667467e816f846a7f2",
    "thermal/sunxi-sid.h":"5c46a704a00f9533c57ea9487d5a624e3859aa3a426bd675e8436d96936181fe",
    "board-kernel/ccu-sun60iw2.c":"1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77",
    "board-kernel/sun60iw2p1.dtsi":"1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa",
    "u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a/arch/arm/include/asm/arch-sunxi/sid_sun60iw2.h":"31474ab5a8d0cb99502c0700779f35887ebb3cba4d6892ef3a19d78d4da96cca",
    "camera-research/A733_User_Manual_V1.00.txt":"219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2",
}
s={}
for name,digest in files.items():
    raw=(cache/name).read_bytes()
    assert hashlib.sha256(raw).hexdigest()==digest,name
    s[name]=raw.decode()
source=s['thermal/sunxi_thermal.c'];manual=s['camera-research/A733_User_Manual_V1.00.txt']
record,=[json.loads(line) for line in log.read_text().splitlines() if line.startswith('{')]
constants={name:int(number) for name,number in re.findall(r'#define SUN60IW2_(\w+)\s+\((-?\d+)\)',source)}

def temperature(code):
    branch='BELOW' if code>constants['SENSOR_DATA_CODE'] else 'ABOVE'
    return (code+constants['OFFSET_'+branch])*constants['SCALE_'+branch]

digest=0
for code in range(1,4096):
    digest=((digest*16777619)^(temperature(code)&0xffffffff))&0xffffffff
assert digest==record['temperature_digest']
w0,w1,w2=record['words'];ft=w0&0xfff
codes=[(w0>>12)&4095,((w0>>24)|(w1<<8))&4095,(w1>>4)&4095,(w1>>16)&4095,((w1>>28)|(w2<<4))&4095]
trunc=lambda a,b: (abs(a)//abs(b))*(-1 if (a<0)!=(b<0) else 1)
expected=[2048-trunc(ft*100-temperature(code),constants['SCALE_BELOW']) for code in codes]
assert expected==record['correction']
chip=re.search(r'sun60iw2p1_ths = \{(.*?)\n\};',source,re.S)[1]
assert re.search(r'\.sensor_num\s*=\s*5',chip)
for i,off in enumerate((0x44,0x48,0x4c)):
    assert int(re.search(r'#define SUN60IW2_THS_EFUSE_OFF'+str(i)+r'\s+\((0x\w+)\)',source)[1],16)==off
assert re.search(r'#define SUNXI_EFUSE_RAM_OFFSET\s+0x200',s['thermal/sunxi-sid.h'])
assert 'baseaddr += SUNXI_EFUSE_RAM_OFFSET;' in s['thermal/sunxi-sid.c']
assert re.search(r'#define SID_EFUSE\s+\(SUNXI_SID_BASE \+ 0x200\)',s['u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a/arch/arm/include/asm/arch-sunxi/sid_sun60iw2.h'])
dts=s['board-kernel/sun60iw2p1.dtsi']
assert re.search(r'sid: sid@3006000.*?reg = <0x0 0x03006000',dts,re.S)
assert re.search(r'dvfs_ori.*?offset = <0x24C>.*?shift = <8>',dts,re.S)
assert re.search(r'71\s+THS\s+0x011C',manual)
assert re.search(r'uart0:.*?interrupts = <GIC_SPI 2 ',dts,re.S)
assert re.search(r'34\s+UART0',manual) # Confirms the manual uses absolute GIC IDs.
ccu=re.sub(r'/\*.*?\*/','',s['board-kernel/ccu-sun60iw2.c'],flags=re.S)
assert re.search(r'gpadc0_24m_parents\[\] = \{ "sys24M", "dcxo" \}',ccu)
assert re.search(r'gpadc0_24m_clk.*?0x0FC0,\s*0, 5,\s*24, 3,\s*BIT\(31\)',ccu,re.S)
assert re.search(r'\[RST_BUS_THS0\].*?0x0fe4, BIT\(16\)',ccu)
trace=record['writes'];last=dict(trace);base=0x02522000;ccubase=0x02002000
assert last[ccubase+0xfc0]&0x8700001f==0x81000000
assert last[ccubase+0xfe4]&0x10001==0x10001
assert next(i for i,(a,v) in enumerate(trace) if a==ccubase+0xfc0 and v>>31)<next(i for i,(a,v) in enumerate(trace) if a==ccubase+0xfe4)
init=re.search(r'static int sun60iw2_thermal_init.*?\n\}',source,re.S)[0]
acq=int(re.search(r'T_ACQ\((\d+)\)',init)[1]);divider=int(re.search(r'FS_DIV\((\d+)\)',init)[1]);period=int(re.search(r'PC_TEMP_PERIOD\((\d+)\)',init)[1])
assert last[base]==acq|(divider<<16)
assert last[base+8]>>12==period
assert last[base+0x30]&7==5
assert last[base+4]&31==31
assert trace[-1][0]==base+4
for i,c in enumerate(expected):
    assert (last[base+0xa0+4*(i//2)]>>(16*(i%2)))&4095==c
for offset in (0x20,0x24,0x28,0x2c):
    assert last[base+offset]==31
    block=re.search(r'7\.1\.8\.\d+\s+0x'+f'{offset:04X}'+r'.*?(?=7\.1\.8\.\d+)',manual,re.S)[0]
    assert 'R/W1C' in block
assert all(a<0x03006000 or a>=0x03007000 for a,_ in trace)
print('PASS all 4095 temperature codes, packed five-sensor calibration, SID shadows, IRQ identity and native startup trace')
