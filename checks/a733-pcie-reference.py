#!/usr/bin/env python3
"""Run one composed PCIe batch and compare executed primary-source helpers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def block(text, pattern):
    start = re.search(pattern, text).start()
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=Path, help="Pinned cached kernel source root")
parser.add_argument("uboot", type=Path, help="Pinned cached U-Boot root")
parser.add_argument("evidence", type=Path, help="New immutable evidence directory")
parser.add_argument("--reuse-reference", type=Path, help="Reuse unchanged source-helper execution from a preserved batch")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.evidence.resolve()
out.mkdir(parents=True, exist_ok=False)
compiler = Path(shutil.which("clang-21")).resolve()
units = ["checks/a733-pcie.c", "drivers/pci/a733_pcie.c", "drivers/pci/a733_pcie_phy.c",
         "drivers/power/a733_pmic.c", "drivers/power/a733_rail_leases.c",
         "drivers/clock/a733_clock_leases.c", "drivers/pinctrl/a733_pins.c",
         "drivers/pinctrl/a733_pin_tables.c", "drivers/gpu/a733_gpu_resources.c"]
native_units = ["drivers/pci/a733_pcie.c", "drivers/pci/a733_pcie_phy.c",
                "drivers/pci/a733_pcie_native.c", "drivers/power/a733_pmic.c", "drivers/pinctrl/a733_pins.c"]
phy_path = args.kernel / "bsp/drivers/phy/sunxi-cadence-combophy.c"
rc_path = args.kernel / "bsp/drivers/pci/controller/allwinner/pcie-sunxi-rc.c"
header_path = args.kernel / "bsp/drivers/pci/controller/allwinner/pcie-sunxi.h"
pmic_path = args.uboot / "drivers/sunxi_power/pmu_axp8191.c"
pmic_header = args.uboot / "include/sunxi_power/pmu_axp8191.h"
cache = Path.home() / ".cache/sunstoneos"
fr = cache / "sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include"
port = cache / "evidence/clock-width-20261016/build/freertos-smp"
paths = ({root / u for u in units + native_units} | set((root / "include").glob("*.h")) |
         set((root / "runtime/include").glob("*.h")) | set((root / "platform/a733").glob("*.h")) |
         set(fr.rglob("*.h")) | set(port.glob("*.h")) |
         {Path(__file__).resolve(), root / "checks/a733-pmic.c", phy_path, rc_path,
          header_path, pmic_path, pmic_header, compiler})
inputs = {}
for path in sorted(paths):
    data = path.read_bytes()
    inputs[str(path)] = hashlib.sha256(data).hexdigest()
    target = out / "inputs" / str(path).lstrip("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
(out / "inputs.json").write_text(json.dumps(inputs, indent=2) + "\n")
commands = []


def run(command):
    result = subprocess.run([str(x) for x in command], text=True, capture_output=True)
    commands.append({"command": [str(x) for x in command], "exit": result.returncode,
                     "stdout": result.stdout, "stderr": result.stderr})
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


# Compile the exact pure aperture filter without the native task/MMIO binding.
native = (root / "drivers/pci/a733_pcie_native.c").read_text()
address = '#include "a733_pcie_native.h"\n' + block(native, r"int a733_pcie_native_address\(") + "\n"
(out / "address.c").write_text(address)
host = out / "composed"
run([compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
     "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
     "-I" + str(root / "include"), *[root / u for u in units], out / "address.c", "-o", host])
digest = hashlib.sha256(host.read_bytes()).hexdigest()
shutil.copy2(host, out / (digest + ".elf"))

# Execute the actual PHY and outbound-iATU functions from the pinned kernel.
phy = phy_path.read_text()
rc = rc_path.read_text()
header = header_path.read_text()
source = phy[:phy.index("#include")]
source += r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1u<<(n))
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((v)>>32))
struct cell {uintptr_t a;u32 value;} cells[256];
unsigned count;
static u32 *word(uintptr_t a){for(unsigned i=0;i<count;++i)if(cells[i].a==a)return &cells[i].value;
 assert(count<256);cells[count].a=a;return &cells[count++].value;}
static u32 readl(const void *p){uintptr_t a=(uintptr_t)p;if(a==0x06c02900)return 1;return *word(a);}
static u32 readw(const void *p){uintptr_t a=(uintptr_t)p;return (*word(a&~3ul)>>((a&3)*8))&65535;}
static void writel(u32 v,void *p){uintptr_t a=(uintptr_t)p;*word(a)=v;printf("%08lx 4 %08x\n",a,v);}
static void writew(u32 v,void *p){uintptr_t a=(uintptr_t)p;u32 *w=word(a&~3ul);unsigned s=(a&3)*8;
 *w=(*w&~(65535u<<s))|(v<<s);printf("%08lx 2 %08x\n",a,v);}
static void udelay(unsigned us){assert(us==100);}
struct sunxi_cadence_combophy {void *top_reg,*phy_reg;};
struct sunxi_cadence_phy {struct sunxi_cadence_combophy *combo1;};
struct sunxi_pcie {int unused;};
struct sunxi_pcie_port {void *dev;struct sunxi_pcie pci;};
#define to_sunxi_pcie_from_pp(p) (&(p)->pci)
#define LINK_WAIT_MAX_RETRIE 20
#define WAIT_ATU 1
#define mdelay(v) assert(0)
#define sunxi_warn(...) assert(0)
static void sunxi_pcie_writel_dbi(struct sunxi_pcie *p,u32 a,u32 v){(void)p;writel(v,(void *)(uintptr_t)(0x06000000+a));}
static u32 sunxi_pcie_readl_dbi(struct sunxi_pcie *p,u32 a){(void)p;return readl((void *)(uintptr_t)(0x06000000+a));}
'''
source += "\n".join(line for line in header.splitlines()
                    if line.startswith("#define PCIE_ATU_") and ("OUTBOUND" in line or "ENABLE" in line))
source += "\n" + block(phy, r"static void sunxi_cadence_phy_pcie_internal_100m_init\(")
source += "\n" + block(rc, r"static void sunxi_pcie_prog_outbound_atu\(")
source += r'''
int main(void){struct sunxi_cadence_combophy lane={(void *)0x06c02000,(void *)0x06ca0000};
 struct sunxi_cadence_phy phy={&lane};sunxi_cadence_phy_pcie_internal_100m_init(&phy);
 struct sunxi_pcie_port pp={0};sunxi_pcie_prog_outbound_atu(&pp,0,4,0x20000000,0x01000000,4096);return 0;}
'''
(out / "reference.c").write_text(source)
if args.reuse_reference:
    previous = args.reuse_reference.resolve()
    prior_inputs = json.loads((previous / "inputs.json").read_text())
    for path in [compiler, phy_path, rc_path, header_path]:
        assert prior_inputs[str(path)] == inputs[str(path)]
    assert (previous / "reference.c").read_bytes() == (out / "reference.c").read_bytes()
    prior = json.loads((previous / "commands.json").read_text())
    execution = [x for x in prior if x["command"] == [str(previous / "reference")]]
    assert len(execution) == 1 and execution[0]["exit"] == 0
    reference = execution[0]["stdout"]
    shutil.copy2(previous / "reference", out / "reference")
    (out / "reused-reference.json").write_text(json.dumps({"directory": str(previous), "execution": execution[0]}, indent=2) + "\n")
else:
    run([compiler, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", out / "reference.c", "-o", out / "reference"])
    reference = run([out / "reference"])
observed = run([host, "--trace"])
(out / "reference-trace.txt").write_text(reference)
(out / "native-trace.txt").write_text(observed)
# The native retarget path first disables its owned window and observes clear.
# Compare every source write in exact order, width and value after removing
# only that one additional native window-disable operation.
expected = reference.splitlines()
actual = [line for line in observed.splitlines() if re.fullmatch(r"[0-9a-f]{8} [24] [0-9a-f]{8}", line)]
assert actual.count("06300004 4 00000000") == 1
actual.remove("06300004 4 00000000")
assert actual == expected, "primary-source PHY/iATU trace differs"

# The three supported boundary selectors use the actual bootloader getter.
pmic = pmic_path.read_text()
decode = pmic[:pmic.index("#include")] + r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define CONFIG_SYS_SUNXI_R_I2C0_SLAVE 0x36
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define axp_err(...) assert(0)
static unsigned selector;
static int pmic_bus_read(unsigned a,unsigned r,u8 *out){assert(a==0x36);
 if(r==0x20)*out=128;else {assert(r==0x2b);*out=selector;}return 0;}
'''
decode += pmic_header.read_text() + "\n"
decode += block(pmic, r"typedef struct _axp_contrl_info") + " axp_contrl_info;\n"
decode += "axp_contrl_info pmu_axp8191_ctrl_tbl[]={" + re.search(r'\{ "bldo2",[^}]+\}', pmic)[0] + "};\n"
decode += block(pmic, r"static axp_contrl_info \*get_ctrl_info_from_tbl\(") + "\n"
decode += block(pmic, r"static int pmu_axp8191_get_voltage\(") + "\n"
decode += 'int main(void){unsigned codes[]={0,13,29};for(unsigned i=0;i<3;++i){selector=codes[i];assert(pmu_axp8191_get_voltage("bldo2")==500+100*(int)selector);}return 0;}\n'
(out / "bldo2-reference.c").write_text(decode)
run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", out / "bldo2-reference.c", "-o", out / "bldo2-reference"])
run([out / "bldo2-reference"])
for unit in native_units:
    target = out / (Path(unit).stem + ".o")
    run([compiler, "--target=aarch64-none-elf", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-ffreestanding", "-mstrict-align", "-DINCLUDE_xSemaphoreGetMutexHolder=1",
         "-DINCLUDE_xTaskGetCurrentTaskHandle=1", "-DINCLUDE_xTaskGetSchedulerState=1",
         *["-I" + str(p) for p in [root / "include", root / "runtime/include", root / "platform/a733", fr, port]],
         "-c", root / unit, "-o", target])
artifacts = {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in out.iterdir() if p.is_file() and p.name != "results.json"}
result = {"status": "PASS", "inputs": inputs, "artifacts": artifacts,
          "scope": "actual source PHY/iATU helpers; shared provider/rail/PCK/pins/clock host composition; native compilation",
          "limitations": ["hardware responses and lock scheduling are modeled", "no physical endpoint, bus deadline or electrical qualification",
                          "DMA/MSI activation and endpoint operations remain required follow-on work"]}
(out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
print("PASS PCIe source comparison, composed ownership checks, BLDO2 getter and native compilation")
