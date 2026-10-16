#!/usr/bin/env python3
"""One source-backed capacity/protocol batch and AArch64 compile; no hardware."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--sources', type=Path, required=True, help='External SunstoneOS sources cache')
p.add_argument('--evidence', type=Path, required=True, help='Fresh external directory')
p.add_argument('--cc', default='/usr/bin/clang-21')
p.add_argument('--auto-stop-argument-only', action='store_true')
a = p.parse_args()
a.evidence.mkdir(parents=True, exist_ok=False)
root = Path(__file__).resolve().parents[1]
records = []
inputs = {}


def pinned(relative, digest):
    path = a.sources / relative
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == digest, path
    inputs[str(path)] = digest
    return data.decode()


def function(text, name):
    start = text.index('static int ' + name + '(')
    cursor = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[start:cursor]


def run(command):
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    records.append(dict(command=command, returncode=result.returncode,
                        stdout=result.stdout, stderr=result.stderr))
    (a.evidence / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
    assert result.returncode == 0, records[-1]
    return result.stdout


sd = pinned('mmc-read/linux-sd.c', '2d522377ea4ea46fa01f23fdd9e5e04d7756dd51cbb2b6031aa8fe7e13796c07')
mmc = pinned('mmc-read/linux-mmc.c', '4bdca067049f6813b07034cd83e8d9880992a0a01a1292fe499dcd34efcaf2e2')
h = pinned('radio-sdio/include/linux/mmc/mmc.h', '9762e69cbbadb447abe1ba191ad2fc883d645c808024dfd810c12436ad4a695b')
controller = pinned('radio-sdio/bsp/drivers/mmc/sunxi-mmc-v5p3x.c',
                    '348a6893ae256b2df712813e998ab2b6a13341dd8c57f6fcfb31656fe735d94c')
main_controller = pinned('radio-sdio/bsp/drivers/mmc/sunxi-mmc.c',
                         '52c7f92db77290437b7818f3c3a92a2956c1ab637214794e56220d0dde99ce16')
assert 'mmc_writel(host, REG_A12A, 0);' in main_controller
u = 'u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a/'
transfer = pinned(u + 'drivers/mmc/sunxi_mmc.c', '890c3c48de3592cb214d4f32195c5e55fe2a03cfcd0d16d13cb2e36f4ca354c5')
timing = pinned(u + 'drivers/mmc/host/sunxi_mmc_host_tm4.c',
                'b3d849ca1cb049753d9bcb0d38d90f86c8e9179dd5916a91d91955f8104bc082')
t = pinned('camera-research/A733_User_Manual_V1.00.txt',
           '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2')
manual = a.sources / 'camera-research/A733_User_Manual_V1.00.pdf'
assert hashlib.sha256(manual.read_bytes()).hexdigest() == 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d'
inputs[str(manual)] = 'cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d'
for name, offset in [('CMDQ_MODE_EN', 15), ('DATA_SECTOR_SIZE', 61), ('PART_CONFIG', 179),
                     ('BUS_WIDTH', 183), ('HS_TIMING', 185), ('REV', 192), ('STRUCTURE', 194), ('SEC_CNT', 212)]:
    assert re.search(r'#define\s+EXT_CSD_' + name + r'\s+' + str(offset) + r'\b', h), name
# Manual sections distinguish data completion, automatic stop and its response.
flat = ' '.join(t.split())
assert '0x80003352' in flat
assert 'If set, the SMHC_RESP1 will record the response of auto CMD12.' in flat
assert 'Check whether SMHC_RINTSTS[ACD] and SMHC_RINTSTS[DTC] are both 1.' in flat
assert 'mmc_clk_400k].sam_ph_dat = 0' in controller
assert 'mmc_clk_400k].sam_ph_cmd = 0' in controller
assert '((odly&0x1)<<16) | ((odly&0x1)<<17)' in timing
assert 'SDXC_EnableDly' in timing
assert 'cmd->response[0] = readl(&priv->reg->resp3)' in transfer
# Execute unchanged Linux decoder bodies with a small type/platform shim. Keep
# original notices in this external generated artifact. No Linux code is linked
# into the native runtime. The SDXC 2^32-sector boundary is checked separately
# because this pinned Linux version stores capacity in a 32-bit field.
shim = r'''
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
struct mmc_csd {
 unsigned structure,mmca_vsn,taac_ns,taac_clks,max_dtr,cmdclass,capacity;
 unsigned read_blkbits,read_partial,write_misalign,read_misalign,dsr_imp;
 unsigned r2w_factor,write_blkbits,write_partial,erase_size,c_size;
};
struct mmc_card {struct mmc_csd csd;u32 raw_csd[4];void *host;unsigned erase_size;};
#define pr_err(...) ((void)0)
#define mmc_hostname(x) "reference"
#define mmc_card_set_readonly(x) ((void)(x))
#define mmc_card_set_blockaddr(x) ((void)(x))
#define mmc_card_set_ext_capacity(x) ((void)(x))
'''
arrays = mmc[mmc.index('static const unsigned int tran_exp[]'):mmc.index('/*\n * Given the decoded CSD')]
reference = (mmc[:mmc.index('#include')] + sd[:sd.index('#include')] + shim + arrays +
             function(sd, 'mmc_decode_csd').replace('mmc_decode_csd(', 'sd_reference(', 1) + '\n' +
             function(mmc, 'mmc_decode_csd').replace('mmc_decode_csd(', 'mmc_reference(', 1) + '\n')
(a.evidence / 'mmc-reference.h').write_text(reference)
common = [a.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Iinclude']
host = a.evidence / 'mmc-model'
run(common + ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
              '-I' + str(a.evidence), 'drivers/storage/sunxi_mmc.c', 'checks/sunxi-mmc.c', '-o', str(host)])
output = run([str(host)] + (['--auto-stop-argument'] if a.auto_stop_argument_only else []))
print(output, end='')
obj = a.evidence / 'sunxi-mmc.o'
run(common + ['--target=aarch64-none-elf', '-mcpu=cortex-a55', '-ffreestanding',
              '-fno-builtin', '-O2', '-fstack-usage', '-c', 'drivers/storage/sunxi_mmc.c', '-o', str(obj)])
print(run(['/usr/lib/llvm-21/bin/llvm-size', str(obj)]), end='')
undefined = run(['/usr/lib/llvm-21/bin/llvm-nm', '--undefined-only', str(obj)])
# Aggregate zero/copy operations may lower to runtime memory helpers. There are
# no OS, allocation or Linux dependencies.
assert all(line.split()[-1] in {'memset', 'memcpy'} for line in undefined.splitlines()), undefined
for relative in ['include/sunxi_mmc.h', 'drivers/storage/sunxi_mmc.c', 'checks/sunxi-mmc.c', 'checks/sunxi-mmc.py']:
    inputs[relative] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
(a.evidence / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
print('PASS: native AArch64 compile; evidence retained at', a.evidence)
