#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare initial camera setup and PTEs against isolated pinned Linux code.

Inputs: cached Linux iommu directory, fresh output directory, native register
image from checks/a733-iommu.c. Generated inputs and executables stay outside Git.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
p.add_argument("native_registers", type=pathlib.Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
root = pathlib.Path(__file__).resolve().parents[1]
pins = {
    "sunxi-iommu-v2.c": "2cfa66d29a1223d259486c29ef6722fc6eb3679990f39592c8c4d4b11442e31d",
    "sunxi-iommu-pgtable.c": "c48d2f4c80f3d15455e811eaddbea0ab485af217654f9823e927fe299f329b76",
    "sunxi-iommu-pgtable.h": "93ff578b4632306c9403f1edd9c5e1841558d465ecc112caf07346d47373eba3",
}
sources = {}
for name, digest in pins.items():
    data = (a.source / name).read_bytes()
    assert hashlib.sha256(data).hexdigest() == digest, name
    (a.output / name).write_bytes(data)
    sources[name] = data.decode()

def function(text, name):
    start = re.search(r"^(?:static )?(?:inline )?(?:int|void|u32) " + name + r"\(", text, re.M).start()
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

def macro(text, name):
    return re.search(r"^#define " + name + r"\b[^\n]*(?:\\\n[^\n]*)*", text, re.M).group()

v2 = sources["sunxi-iommu-v2.c"]
pg = sources["sunxi-iommu-pgtable.c"]
header = sources["sunxi-iommu-pgtable.h"]
# Preserve upstream legal notices in the isolated source compilation.
legal = v2[:v2.index("#include")] + pg[:pg.index("#include")]
prelude = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
typedef uint32_t u32;
typedef uint64_t phys_addr_t;
#define BIT(n) (1u<<(n))
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define AW_IOMMU_PGTABLE_V2 1
struct iommu_domain { int dummy; };
struct sunxi_iommu_plat_data { unsigned version,tlb_prefetch,tlb_invalid_mode,ptw_invalid_mode; };
struct sunxi_iommu_domain { unsigned int *pgtable;struct iommu_domain domain; };
struct sunxi_iommu_dev { unsigned bypass,iommu_lock; void *dev;const struct sunxi_iommu_plat_data *plat_data; };
static struct sunxi_iommu_dev *global_iommu_dev;
static bool iommu_hw_init_flag;
static uint32_t regs[0x20000/4];
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define __pa(p) ((uintptr_t)(p))
#define spin_lock_irqsave(l,f) ((f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(f))
#define sunxi_iommu_distribute_mater_get(mask) ((void)(mask))
#define sunxi_iommu_distribute_mater_put(mask) ((void)(mask))
#define dev_err(...) ((void)0)
#define ENODEV 19
static void udelay(unsigned n) { if(n!=10)abort(); }
static u32 sunxi_iommu_read(struct sunxi_iommu_dev *d,u32 off) { (void)d;return regs[off/4]; }
static void sunxi_iommu_write(struct sunxi_iommu_dev *d,u32 off,u32 v) {
    (void)d;
    if(off>=0xc00 && off<0x1000)return; /* DTS skip_mask IDs4/5. */
    if(off>=sizeof(regs) || (off&3))abort();
    regs[off/4]=v;
}
'''
defs = v2[v2.index("#define IOMMU_VERSION_REG"):v2.index("#define IOMMU_DUMP(addr)")]
# Include exact vendor equal-bank macros, preserving every continuation line.
start = v2.index("#define WRITE_EQUAL_REG")
end = v2.index("/*\n * The format", start)
defs += v2[start:end]
defs += v2[v2.index("enum sunxi_iommu_version {"):v2.index("struct sunxi_iommu_plat_data {")]
defs += "\n#define IOMMU_PT_MASK (~((1ULL << 12) - 1))\n"
defs += pg[pg.index("#if defined(AW_IOMMU_PGTABLE_V2)\n#define SUNXI_PTE"):pg.index("#define IS_VALID")]
functions = "\n".join(function(v2, name) for name in [
    "__sunxi_enable_device_iommu_unlocked", "sunxi_tlb_flush",
    "__sunxi_iommu_enable_interrupt_unlocked", "sunxi_iommu_hw_init"])
functions += "\n" + function(pg, "sunxi_mk_pte")
main = r'''
int main(void) {
    struct sunxi_iommu_plat_data plat={0x16,0x3007f,1,1};
    struct sunxi_iommu_dev dev={.bypass=0xfcf&~2,.plat_data=&plat};
    struct sunxi_iommu_domain domain={.pgtable=(void *)UINT64_C(0x140000000)};
    global_iommu_dev=&dev;
    if(sunxi_iommu_hw_init(&domain.domain) || !iommu_hw_init_flag)return 1;
    if(fwrite(regs,1,sizeof(regs),stdout)!=sizeof(regs))return 2;
    for(unsigned i=0;i<60;++i)for(unsigned prot=1;prot<=3;++prot) {
        u32 pe=sunxi_mk_pte(UINT64_C(0x40000000)+UINT64_C(0x10000000)*i+0xabc000,prot);
        if(fwrite(&pe,4,1,stdout)!=1)return 3;
    }
    return 0;
}
'''
reference = a.output / "isolated-linux.c"
reference.write_text(legal + prelude + defs + functions + main)
reference_exe = a.output / "isolated-linux"
subprocess.run(["cc", "-std=gnu11", "-O1", str(reference), "-o", str(reference_exe)], check=True)
reference_bytes = subprocess.check_output([str(reference_exe)])
(a.output / "reference.bin").write_bytes(reference_bytes)
native = a.native_registers.read_bytes()
assert len(native) == 0x20000
ref_regs = reference_bytes[:0x20000]
# The production core polls faults and leaves IRQ enables zero. This is the
# only intentional register difference from source initialization.
irq = {0x80: 3, 0x10080: 3}
for master in (0, 1, 2, 3, 6, 7, 8, 9, 10, 11):
    irq[(master // 6)*0x10000 + 0x400 + (master % 6)*0x200 + 0x30] = 1
for off in range(0, len(native), 4):
    n = int.from_bytes(native[off:off+4], "little")
    r = int.from_bytes(ref_regs[off:off+4], "little")
    assert (n, r) == (0, irq[off]) if off in irq else n == r, (hex(off), hex(n), hex(r))
native_c = a.output / "native-pte.c"
native_c.write_text(r'''
#include "a733_iommu.h"
#include <stdio.h>
static _Alignas(16384) uint32_t words[A733_IOMMU_TABLE_BYTES/4];
int main(void) {
    struct a733_iommu_tables t;
    for(unsigned i=0;i<60;++i)for(unsigned prot=1;prot<=3;++prot) {
        struct a733_iommu_span s={4096,UINT64_C(0x40000000)+UINT64_C(0x10000000)*i+0xabc000,4096,prot};
        if(a733_iommu_build(&t,words,0x44000000,&s,1)!=1)return 1;
        if(fwrite(words+4097,4,1,stdout)!=1)return 2;
    }
}
''')
native_exe = a.output / "native-pte"
subprocess.run(["cc", "-std=c11", "-O1", "-I" + str(root / "include"),
                str(native_c), str(root / "drivers/iommu/a733_iommu.c"),
                "-o", str(native_exe)], check=True)
native_ptes = subprocess.check_output([str(native_exe)])
(a.output / "native-pte.bin").write_bytes(native_ptes)
assert native_ptes == reference_bytes[0x20000:]
print("A733 IOMMU: initial registers match isolated Linux except explicit polled IRQ masks; 180 PTEs match")
