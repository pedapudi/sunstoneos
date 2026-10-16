#!/usr/bin/env python3
"""Check the modeled media clock trace against pinned primary source.

Usage: a733-media-clock.py SOURCE_CACHE_ROOT MODEL_LOG
No downloads, hardware access or legacy suites.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

cache, log = map(Path, sys.argv[1:])
expected = {
    "board-kernel/ccu-sun60iw2.c": "1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77",
    "board-kernel/sun60i-a733-orangepi-zero3w.dts": "31145367f604fd756f2c6a67447e5134e4be0ad5fae87672cfaa9b7c6d055a39",
    "camera-research/A733_User_Manual_V1.00.txt": "219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2",
    "camera/bsp/drivers/vin/vin.c": "b84ac10aa78a63aeaa98ed94a01c688dcb5ac49f063ac09c4ef6c874916af249",
    "gpu-power/drivers/pm_domain/pck600_domains.c": "2d9c2b60b3b264d1732538815d12570e45386d30b280a1d0b4f5b9780bdc49ee",
}
sources = {}
for name, digest in expected.items():
    raw = (cache/name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    sources[name] = raw.decode()
ccu = re.sub(r"/\*.*?\*/", "", sources["board-kernel/ccu-sun60iw2.c"], flags=re.S)
dts = sources["board-kernel/sun60i-a733-orangepi-zero3w.dts"]
manual = sources["camera-research/A733_User_Manual_V1.00.txt"]
record, = [json.loads(line) for line in log.read_text().splitlines() if line.startswith("{")]
trace = [(a-0x02002000,v) for a,v in record["writes"]]
final = dict(trace)
field = lambda word,start,width: (word>>start)&((1<<width)-1)
rates = {"sys24M":24000000}
for name,key in (("pll_ref","ref"),("pll_peri0","peri"),("pll_video0","video"),("pll_de","de")):
    body = re.search(r"static struct ccu_nm "+name+r"_clk = \{(.*?)\n\};",ccu,re.S)[1]
    shift,width,minimum,maximum = map(int,re.search(r"_SUNXI_CCU_MULT_MIN_MAX\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)",body).groups())
    n=field(record[key],shift,width)+1
    assert minimum<=n<=maximum
    if name=="pll_ref":
        shift,width=map(int,re.search(r"\.m\s*= _SUNXI_CCU_DIV\((\d+),\s*(\d+)\)",body).groups())
        rates["pll-ref"]=24000000*n//(field(record[key],shift,width)+1)
    else:
        assert '"pll-ref"' in body
        rates[name.replace("_","-")]=rates["pll-ref"]*n
for name in ("pll_ve0", "pll_ve1"):
    body = re.search(r"static struct ccu_nm " + name + r"_clk = \{(.*?)\n\};", ccu, re.S)[1]
    assert '"pll-ref"' in body
for name,key in (("pll_video0_4x","video"),("pll_de_3x","de"),("pll_peri0_2x","peri")):
    title,parent,offset,shift,width=re.search(r"SUNXI_CCU_M(?:_WITH_GATE)?\("+name+r'_clk,\s*"([^"]+)",\s*"([^"]+)",\s*(0x\w+),\s*(\d+),\s*(\d+)',ccu).groups()
    rates[title]=rates[parent]//(field(record[key],int(shift),int(width))+1)
for suffix in ("600m","300m","150m"):
    title,parent,div,mul=re.search(r'CLK_FIXED_FACTOR\(pll_peri0_'+suffix+r'_clk,\s*"([^"]+)",\s*"([^"]+)",\s*(\d+),\s*(\d+)',ccu).groups()
    rates[title]=rates[parent]*int(mul)//int(div)
for name,want in (("csi",int(re.search(r"csi_top = <(\d+)>",dts)[1])),("isp",int(re.search(r"csi_isp = <(\d+)>",dts)[1])),("de0",600000000),("dsi1",150000000)):
    parent_array,offset,shift,width,muxshift,muxwidth=re.search(r'SUNXI_CCU_M_WITH_MUX_GATE\('+name+r'_clk,\s*"[^"]+",\s*(\w+),\s*(0x\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)',ccu).groups()
    parents=re.findall(r'"([^"]+)"',re.search(parent_array+r'\[\] = \{([^}]+)\}',ccu)[1])
    word=final[int(offset,16)]
    assert word&(1<<31)
    rate=rates[parents[field(word,int(muxshift),int(muxwidth))]]//(field(word,int(shift),int(width))+1)
    assert rate==want,(name,rate,want)
    events=[v for a,v in trace if a==int(offset,16)]
    assert not events[0]&(1<<31) if len(events)>1 else True
# All bus/reset writes derive from registered reset words. Each deassertion
# precedes its first enabled bus gate, and no reset is asserted by the trace.
resets={int(a,16):int(b) for a,b in re.findall(r'\[RST_\w+\]\s*=\s*\{\s*(0x\w+),\s*BIT\((\d+)\)',ccu)}
for offset in (0x1884,0x1844,0xa74,0xa04,0x16e4,0x16c4,0x150c,0x158c,0x58c,0x5b4):
    assert resets[offset]==16
    events=[v for a,v in trace if a==offset]
    assert events and all(v&(1<<16) for v in events)
    assert not events[0]&7
for n in (1,2):
    offset=int(re.search(r'SUNXI_CCU_MP_WITH_MUX_GATE_NO_INDEX\(csi_master'+str(n)+r'_clk,\s*"[^"]+",\s*\w+,\s*(0x\w+)',ccu)[1],16)
    assert final[offset]&0x87001f1f==0
# The manual, not the source helper's name, defines gate polarity.
for offset,name in ((0x1908,"PERI0PLL"),(0x1910,"VIDEOPLL"),(0x1928,"DEPLL")):
    block=re.search(r"4\.1\.6\.\d+\s+0x"+f"{offset:04X}"+r" .*?(?=4\.1\.6\.\d+\s+0x)",manual,re.S)[0]
    assert re.search(r"0:\s*Auto\s+1:\s*No-Auto",block),name
    assert "1: Enable" in block
    assert f"0x{offset+0x80:04X}" in manual
vin=sources["camera/bsp/drivers/vin/vin.c"]
start=vin.index("static int vin_md_clk_enable")
assert vin.index("reset_control_deassert",start)<vin.index("clk_prepare_enable",start)
allowed={0x1884,0x1844,0xa74,0xa04,0x16e4,0x16c4,0x150c,0x158c,0x58c,0x5b4,0x1804,0x1808,0x1840,0x1860,0xa00,0x1588,0x5e4}
assert set(final)<=allowed
print("PASS pinned parent topology, board camera rates, display rates, reset order, disabled sensor clocks and gate polarity")
