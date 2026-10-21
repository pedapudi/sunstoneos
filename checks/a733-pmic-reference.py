#!/usr/bin/env python3
"""Compare supported PMIC decodes with the pinned bootloader's actual getter."""
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
parser.add_argument("uboot", type=Path)
parser.add_argument("native", type=Path)
parser.add_argument("evidence", type=Path)
args = parser.parse_args()
args.evidence.mkdir(parents=True, exist_ok=False)
paths = ["drivers/sunxi_power/pmu_axp8191.c", "include/sunxi_power/pmu_axp8191.h",
         "drivers/i2c/sunxi_i2c.c", "include/sunxi_i2c.h", "include/linux/kernel.h"]
inputs = {}
texts = {}
for name in paths:
    data = (args.uboot / name).read_bytes()
    inputs[str(args.uboot / name)] = hashlib.sha256(data).hexdigest()
    target = args.evidence / "source" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    texts[name] = data.decode()
shutil.copy2(args.native, args.evidence / "native")
shutil.copy2(__file__, args.evidence / "runner.py")
inputs[str(args.native)] = hashlib.sha256(args.native.read_bytes()).hexdigest()
inputs[str(Path(__file__).resolve())] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
pmic = texts[paths[0]]
names = ["dcdc1", "dcdc2", "dcdc3", "dcdc4", "dcdc5", "dcdc8", "bldo5", "cldo1"]
code = pmic[:pmic.index("#include")]
code += r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define CONFIG_SYS_SUNXI_R_I2C0_SLAVE 0x36
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*(x)))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define axp_err(...) assert(0)
static unsigned selector;
static int pmic_bus_read(unsigned a,unsigned r,u8 *out) {
    assert(a==0x36);
    if(r==0x10 || r==0x21)*out=255;
    else {assert(r==0x12 || r==0x13 || r==0x14 || r==0x15 || r==0x16 || r==0x19 || r==0x2e || r==0x2f);*out=selector;}
    return 0;
}
'''
code += texts[paths[1]] + "\n"
code += block(pmic, r"typedef struct _axp_contrl_info") + " axp_contrl_info;\n"
code += "axp_contrl_info pmu_axp8191_ctrl_tbl[]={\n"
code += ",\n".join(re.search(r'\{ "' + name + r'",[^}]+\}', pmic)[0] for name in names)
code += "};\n"
code += block(pmic, r"static axp_contrl_info \*get_ctrl_info_from_tbl\(") + "\n"
code += block(pmic, r"static int pmu_axp8191_get_voltage\(") + "\n"
code += 'int main(void){char name[32];unsigned want;while(scanf("%31s %u %u",name,&selector,&want)==3)'
code += '{assert(selector<256);assert(pmu_axp8191_get_voltage(name)==(int)want);}return 0;}\n'
(args.evidence / "reference.c").write_text(code)
compiler = shutil.which("clang-21")
if not compiler:
    raise RuntimeError("clang-21 unavailable")
inputs[compiler] = hashlib.sha256(Path(compiler).read_bytes()).hexdigest()
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
           str(args.evidence / "reference.c"), "-o", str(args.evidence / "reference")]
subprocess.run(command, check=True)
output = subprocess.check_output([str(args.evidence / "native"), "--decode"], text=True)
(args.evidence / "native-decodes.txt").write_text(output)
subprocess.run([str(args.evidence / "reference")], input=output, text=True, check=True)
# Source definitions independently pin the native state machine's status values.
header = texts[paths[3]]
statuses = {"I2C_START_TRANSMIT": 8, "I2C_RESTART_TRANSMIT": 16,
            "I2C_ADDRWRITE_ACK": 24, "I2C_ADDRREAD_ACK": 64,
            "I2C_DATAWRITE_ACK": 40, "I2C_READY": 248, "I2C_DATAREAD_NACK": 88}
for name, value in statuses.items():
    found = re.search(r"#define\s+" + name + r"\s+(0x[0-9a-fA-F]+)", header)
    assert found and int(found[1], 16) == value
body = block(texts[paths[2]], r"static int sunxi_i2c_read\(")
ordered = ["twi_start(", "twi_send_slave_addr(", "twi_send_addr(", "twi_restart(",
           "twi_send_slave_addr(", "twi_get_data(", "twi_stop("]
cursor = 0
for symbol in ordered:
    cursor = body.index(symbol, cursor) + len(symbol)
assert "twi_send_data(" not in body
# The CPU-rail selector write follows the helper's write order: one data byte
# after the register index, acknowledged with I2C_DATAWRITE_ACK, then STOP.
body = block(texts[paths[2]], r"static int sunxi_i2c_write\(")
cursor = 0
for symbol in ["twi_start(", "twi_send_slave_addr(", "twi_send_addr(", "twi_send_data(", "twi_stop("]:
    cursor = body.index(symbol, cursor) + len(symbol)
assert "twi_restart(" not in body
body = block(texts[paths[2]], r"static int twi_send_data\(")
assert "twi_wait_status(bus_num, I2C_DATAWRITE_ACK, timeout)" in body
result = {"status": "PASS", "decoded_cases": len(output.splitlines()), "inputs": inputs,
          "command": command, "scope": "actual bootloader voltage getter on supported selectors; static I2C read and write helper order/status definitions",
          "native_sha256": inputs[str(args.native)],
          "reference_sha256": hashlib.sha256((args.evidence / "reference").read_bytes()).hexdigest(),
          "limitations": ["DCDC8 selectors >=102 remain undecoded", "no physical timing/electrical proof", "I2C MMIO helper instructions not executed"]}
(args.evidence / "results.json").write_text(json.dumps(result, indent=2) + "\n")
print("PASS actual source voltage getter and I2C source definitions")
