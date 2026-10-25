#!/usr/bin/env python3
"""Check the UHS-I SDR104 sequence against pinned references and run its models.

The pinned references are the SD Physical Layer Simplified Specification 9.10,
Linux v6.12 and the board kernel's MMC core, the board kernel's sunxi-mmc,
pin-control and clock drivers, the board device tree, the A733 manual and
datasheet. The check asserts the facts the MMC core and storage platform rely
on, then builds and runs, under AddressSanitizer and UndefinedBehaviorSanitizer:
  checks/sunxi-mmc-uhs.c  the MMC core against a modeled UHS-I card;
  checks/a733-mmc-uhs.c   the actual storage platform and shared providers.
Finally it compiles the core and platform for AArch64 and records stack frames.
No board, USB or card access."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sources', type=Path, default=Path.home() / '.cache/sunstoneos/sources')
p.add_argument('--output', type=Path, required=True, help='new evidence directory')
p.add_argument('--cc', default='/usr/bin/clang-21')
p.add_argument('--read-reference', type=Path, default=Path.home() /
               '.cache/sunstoneos/evidence/mmc-read-20261016/auto-stop-argument/mmc-reference.h',
               help='generated Linux CSD reference header that the accepted card model includes')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
BOARD = 'sd-uhs/board-kernel-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/'
LINUX = 'sd-uhs/linux-adc218676eef25575469234709c2d87185ca223a/'
PINS = {
    'sd-uhs/sd-spec/spec.txt': None,
    'sd-uhs/sd-spec/Part1PhysicalLayerSimplifiedSpecificationVer9.10Fin_20231201.pdf':
        'ee86b20ec02b30797f14ceaf9f760feadb321de12cceb278b917aab7a65380e3',
    BOARD + 'drivers/mmc/core/core.c': 'e51652c15855d2ad7fef5d947b0d47378699fbca25de395f780d18f60952ed98',
    BOARD + 'drivers/mmc/core/sd.c': '2d522377ea4ea46fa01f23fdd9e5e04d7756dd51cbb2b6031aa8fe7e13796c07',
    BOARD + 'drivers/mmc/core/mmc_ops.c': '4e42bfe626bbad9ae01a4e32bdffe7543eadf6c23ac02c88191a614f8a62f0c4',
    BOARD + 'bsp/drivers/mmc/sunxi-mmc.c': '52c7f92db77290437b7818f3c3a92a2956c1ab637214794e56220d0dde99ce16',
    BOARD + 'bsp/drivers/mmc/sunxi-mmc.h': '0945ee0ea5e47082c2ff159e578380671f86f4cd797f4fb3a853bc6f8cde0da3',
    BOARD + 'bsp/drivers/mmc/sunxi-mmc-v5p3x.c': '348a6893ae256b2df712813e998ab2b6a13341dd8c57f6fcfb31656fe735d94c',
    LINUX + 'drivers/mmc/core/core.c': 'c17b20f781a8f630aaf0c9f3bcfeb106838e123bbbf509ad46657b1906e705d0',
    LINUX + 'drivers/mmc/host/sunxi-mmc.c': '1e2e6b5ffdbccbec7427d7a5c9648f2dcddbb4697b8b1c0d9d90193c350871ba',
    'mmc-sdc3/sunxi-mmc-v4p5x.c': 'f7b14c8dce66eefc222ac7d06b8aad041ea132c463a1f035a201dd940fb95f7f',
    'pins/pinctrl-sunxi.c': '3bc32a1fd6be151be68979a7c44fe400adb657f21cb66f4e043d31aea4273406',
    'board-kernel/pinctrl-sun60iw2.c': 'ab05ddccecd152edf3fafd238ade22299aebe3448646f02f0551edef0c6e1ec7',
    'board-kernel/ccu-sun60iw2.c': '1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77',
    'board-kernel/sun60i-a733-orangepi-zero3w.dts': '31145367f604fd756f2c6a67447e5134e4be0ad5fae87672cfaa9b7c6d055a39',
    'board-kernel/sun60iw2p1.dtsi': '1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa',
    'camera-research/A733_User_Manual_V1.00.txt': '219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2',
    'A733-datasheet.txt': '0c7c3c1984f49019f421b33089bc0f334cb084a489488ff10c69b7062a5accf0',
}
inputs, text = {}, {}
for rel, digest in PINS.items():
    data = (a.sources / rel).read_bytes(); h = hashlib.sha256(data).hexdigest()
    assert digest is None or h == digest, rel
    inputs[rel] = h; text[rel] = data.decode('latin-1')


def body(source, name):
    start = source.index(name)
    end = source.index('\n}\n', start)
    return source[start:end]


# Spec 9.10: CMD11 precedes CMD2, a CMD11 without response or a failed switch
# needs a power cycle, 1.8 V persists to power cycle, CMD19 bound and pattern.
spec = ' '.join(text['sd-uhs/sd-spec/spec.txt'].split())
for fact in ['3.9.4 UHS-I Bus Speed Modes Selection Sequence', '4.2.4.4 Voltage Switch Command',
             '4.2.4.5 Tuning Command', 'If the host detects no response, power cycle should be executed',
             'Power cycle resets the signal voltage to 3.3V', 'Switch Error', 'Power Cycle',
             '40 times CMD19 executions in no more than 150ms', 'FF0FFF00 FFCCC3CC C33CCCFF FEFFFEEF']:
    assert fact in spec, fact
# Linux MMC core: 1 ms, clock gated 10 ms, 1 ms; S18R repeats; SDR104 via group 1.
for rel in [BOARD + 'drivers/mmc/core/core.c', LINUX + 'drivers/mmc/core/core.c']:
    host, card = body(text[rel], 'int mmc_host_set_uhs_voltage'), body(text[rel], 'int mmc_set_uhs_voltage')
    assert 'host->ios.clock = 0;' in host and 'mmc_delay(10);' in host
    assert card.count('mmc_delay(1);') == 2 and 'cmd.opcode = SD_SWITCH_VOLTAGE;' in card
sd = text[BOARD + 'drivers/mmc/core/sd.c']
assert 'ocr |= SD_OCR_S18R;' in sd and '(*rocr & SD_ROCR_S18A)' in sd
assert 'err = mmc_sd_switch(card, 1, 0, card->sd_bus_speed, status);' in sd
assert 'if ((status[16] & 0xF) != card->sd_bus_speed)' in sd
ops = text[BOARD + 'drivers/mmc/core/mmc_ops.c']
start = ops.index('static const u8 tuning_blk_pattern_4bit[]')
linux_pattern = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', ops[start:ops.index('};', start)])]
core = (ROOT / 'drivers/storage/sunxi_mmc.c').read_text()
ours = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', core[core.index('tuning_pattern[64] = {'):core.index('};', core.index('tuning_pattern[64] = {'))])]
assert linux_pattern == ours and len(ours) == 64
# Board kernel sunxi-mmc: CMD11 voltage-switch flag, DAT3 and switch-done check,
# PF pin state for 1.8 V, sweep of 64 delays with 20 CMD19 and threshold 12.
vendor, vh = text[BOARD + 'bsp/drivers/mmc/sunxi-mmc.c'], text[BOARD + 'bsp/drivers/mmc/sunxi-mmc.h']
assert 'cmd_val |= SDXC_VOLTAGE_SWITCH;' in vendor and 'imask |= SDXC_VOLTAGE_CHANGE_DONE;' in vendor
busy = body(vendor, 'static int __sunxi_mmc_card_busy')
assert 'SDXC_CARD_PRESENT' in busy and 'SDXC_SWITCH_DDONE_BIT' in busy and 'msecs_to_jiffies(10)' in busy
assert 'host->pins_bias_1v8' in body(vendor, 'static int sunxi_mmc_signal_voltage_switch')
tune = body(vendor, 'static int sunxi_mmc_execute_tuning')
assert 'u32 sample_win_th = 12;' in tune and 'u32 sdly_cnt = 64;' in tune and 'sunxi_mmc_init_priv_v4p6x' in tune
assert re.search(r'#define\s+MMC_TUNING_RETRY_TIMES\s+20\b', vh) and re.search(r'#define\s+SDXC_VOLTAGE_SWITCH\s+BIT\(28\)', vh)
assert re.search(r'#define\s+SDXC_MASK_DATA0\s+BIT\(31\)', vh) and re.search(r'#define\s+SDXC_VOLTAGE_CHANGE_DONE\s+BIT\(10\)', vh)
assert 'best = s[j] + (w[j] >> 1);' in vendor
assert 'SDXC_START | SDXC_UPCLK_ONLY | SDXC_WAIT_PRE_OVER | SDXC_VOLTAGE_SWITCH' in text[BOARD + 'bsp/drivers/mmc/sunxi-mmc-v5p3x.c']
v4 = text['mmc-sdc3/sunxi-mmc-v4p5x.c']
# DRV_DL bit 7 is set only in FPGA builds; production clears the divider only.
for source, name in [(text[BOARD + 'bsp/drivers/mmc/sunxi-mmc-v5p3x.c'], 'static int sunxi_mmc_clk_set_rate_for_sdmmc_v5p3x'),
                     (v4, 'static int sunxi_mmc_clk_set_rate_for_sdmmc_v4p5x')]:
    rate = body(source, name)
    assert rate.index('#ifdef MMC_FPGA') < rate.index('rval |= (1 << 7);') < rate.index('#else\n\t/* clear internal divider */')
assert 'case MMC_TIMING_UHS_SDR104:\n\t\tdat_drv_ph = 1;' in v4 and 'rval &= ~(SDXC_2X_TIMING_MODE);' in v4
# Upstream sunxi-mmc has no tuning and no bank switch; it masks UHS without delay chains.
upstream = text[LINUX + 'drivers/mmc/host/sunxi-mmc.c']
assert 'execute_tuning' not in upstream and 'mmc_regulator_set_vqmmc' in upstream
# Pin control: A733 descriptors use the automatic PF switch through
# GPIO_POW_VAL_SET_CTL 0x70, observed in GPIO_POW_VAL bit 10, 100 us/10 ms.
pins, sun60 = text['pins/pinctrl-sunxi.c'], text['board-kernel/pinctrl-sun60iw2.c']
assert sun60.count('.auto_power_source_switch = true,') == 2 and '.hw_type = SUNXI_PCTL_HW_TYPE_10,' in sun60
auto = body(pins, 'static void sunxi_power_auto_switch_pf')
assert 'val = 0;\n\t\twritel(val, pctl->membase + pio_pow_ctrl_reg);' in auto
assert 'readl_relaxed_poll_timeout(pow_val_addr, reg, reg & BIT(power_val_pf_bit), 100, 10000)' in auto
assert 'udelay(10);' in auto
# Clocks: SMHC0 mux 2 is PLL_PERI0_300M = PERI0_2X / 4, auto gates 11, 9, 5, 4.
ccu = text['board-kernel/ccu-sun60iw2.c']
assert 'smhc_parents[] = { "sys24M", "pll-peri0-400m", "pll-peri0-300m"' in ccu
assert 'pll_peri0_300m_clk, "pll-peri0-300m", "pll-peri0-600m", 2, 1' in ccu
assert 'pll_peri0_600m_clk, "pll-peri0-600m", "pll-peri0-2x", 2, 1' in ccu
for name, bit in [('pll_peri0_2x_auto_clk', 11), ('pll_peri0_600m_auto_clk', 9),
                  ('pll_peri0_300m_all_auto_clk', 5), ('pll_peri0_300m_auto_clk', 4)]:
    assert re.search(name + r', "[^"]+",\s*"dcxo", 0x1908,\s*BIT\(' + str(bit) + r'\)', ccu), name
# Device trees: SDR104 with a 150 MHz limit, no vqmmc regulator, both PF
# supplies always on, 1.8 V pin state on PF0-PF5.
dts = text['board-kernel/sun60i-a733-orangepi-zero3w.dts']
sdc0 = dts[dts.index('&sdc0 {'):dts.index('};', dts.index('&sdc0 {'))]
for fact in ['sd-uhs-sdr104;', 'max-frequency = <150000000>;', 'execute_tuning_in_kernel;',
             'vmmc-supply = <&reg_dc1sw2>;', 'vqmmc18sw-supply = <&reg_cldo5>;', 'vqmmc33sw-supply = <&reg_dc1sw2>;']:
    assert fact in sdc0, fact
assert not re.search(r'^\s*vqmmc-supply', sdc0, re.M)
cldo5 = dts[dts.index('reg_cldo5: cldo5 {'):dts.index('};', dts.index('reg_cldo5: cldo5 {'))]
assert 'regulator-always-on;' in cldo5
dtsi = text['board-kernel/sun60iw2p1.dtsi']
assert 'pinctrl-names = "default","mmc_1v8","sleep","uart_jtag";' in dtsi
block = dtsi[dtsi.index('sdc0_pins_b: sdc0@1 {'):dtsi.index('};', dtsi.index('sdc0_pins_b: sdc0@1 {'))]
assert 'power-source = <1800>;' in block and '"PF0", "PF1"' in block
# Manual and datasheet: the PF bank selector, its status and the SMHC fields.
manual = ' '.join(text['camera-research/A733_User_Manual_V1.00.txt'].split())
for fact in ['VCC_PF_PWR_VOL_SEL VCC_PF Power Voltage Select Control. 0 R/W 0x1 0: 1.8V 1: 3.3V',
             'PF_PWR_VAL PF_Port Power Value. 00: Invalid 11:10 R 0x1 01: Invalid 10: 3.3V 11: 1.8V',
             'VOL_SW Voltage Switch 28 R/W 0x0 0: Normal command 1: Voltage switch command, set for CMD11 only',
             'Data Starvation Timeout/V1.8 Switch Done', 'MASK_DAT0 Mask Data0',
             'The SoC only supports setting group power voltage in PF ports']:
    assert fact in manual, fact
sheet = text['A733-datasheet.txt']
assert re.search(r'1R3\s+PF6\s+I/O\s+Z\s+PU/PD\s+4 mA\s+VCC18-PF/VCC-IO', sheet)
assert re.search(r'VCC18-PF\s+1\.8 V Power Supply for PF', sheet)

sources = ['drivers/storage/a733_storage_update.c', 'drivers/storage/storage_digest.c', 'drivers/storage/a733_mmc_platform.c',
           'drivers/storage/sunxi_mmc.c', 'drivers/clock/a733_clock_leases.c', 'drivers/power/a733_rail_leases.c',
           'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c',
           'drivers/pinctrl/a733_pin_tables.c']
raw = (ROOT / 'checks/sunxi-mmc.c').read_text()
(a.output / 'mmc-card-model.h').write_text(raw[raw.index('struct model {'):raw.index('static int barrier(void *ctx)')])
platform = (ROOT / 'checks/a733-mmc-platform.c').read_text()
(a.output / 'mmc-platform-fixture.h').write_text(platform[:platform.index('int main(')])
reference = a.read_reference.read_bytes()
assert hashlib.sha256(reference).hexdigest() == '846797588f8bd2470d885027b13078762f4a966826fc71be822c0346e6bf44af'
(a.output / 'mmc-reference.h').write_bytes(reference); inputs[str(a.read_reference)] = hashlib.sha256(reference).hexdigest()
sanitize = [a.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
            '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-Iinclude', '-Ichecks', '-I' + str(a.output)]
native = [a.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Iinclude', '--target=aarch64-none-elf',
          '-mcpu=cortex-a55', '-ffreestanding', '-fno-builtin', '-O2', '-fstack-usage', '-c']
steps = [('core-build', sanitize + ['drivers/storage/sunxi_mmc.c', 'checks/sunxi-mmc-uhs.c', '-o', str(a.output / 'core')]),
         ('core-run', [str(a.output / 'core')]),
         # The shared fixture also defines helpers that only other checks call.
         ('platform-build', sanitize + ['-Wno-unused-function'] + sources + ['checks/a733-mmc-uhs.c', '-o', str(a.output / 'platform')]),
         ('platform-run', [str(a.output / 'platform')]),
         ('native-core', native + ['drivers/storage/sunxi_mmc.c', '-o', str(a.output / 'sunxi_mmc.o')]),
         ('native-platform', native + ['drivers/storage/a733_mmc_platform.c', '-o', str(a.output / 'a733_mmc_platform.o')]),
         # The stage-2 loader's build: no UHS-I code may remain.
         ('native-core-no-uhs', native + ['-DSUNXI_MMC_NO_UHS=1', '-ffunction-sections', 'drivers/storage/sunxi_mmc.c',
                                          '-o', str(a.output / 'sunxi_mmc-no-uhs.o')])]
for name, command in steps:
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (a.output / (name + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode == 0, (name, result.stdout, result.stderr)
    if name.endswith('-run'): print(result.stdout, end='')
    if name == 'native-core-no-uhs':
        sizes = subprocess.run(['/usr/lib/llvm-21/bin/llvm-size', str(a.output / 'sunxi_mmc.o'), str(a.output / 'sunxi_mmc-no-uhs.o')],
                               capture_output=True, text=True, check=True).stdout
        (a.output / 'sizes.txt').write_text(sizes)
nm = subprocess.run(['/usr/lib/llvm-21/bin/llvm-nm', str(a.output / 'sunxi_mmc-no-uhs.o')], capture_output=True, text=True, check=True).stdout
assert not any(name in nm.split() for name in ('sd_signal', 'tune_recover', 'tune_advance', 'uhs_registers', 'tuning_pattern', 'set_delay'))
frames = {}
for su in ['sunxi_mmc.su', 'a733_mmc_platform.su']:
    for line in (a.output / su).read_text().splitlines():
        where, size, kind = line.split('\t'); frames[where.split(':')[-1]] = int(size)
for relative in ['include/sunxi_mmc.h', 'include/sunxi_smhc.h', 'include/a733_mmc_platform.h', 'include/a733_mmc_platform_native.h',
                 'checks/sunxi-mmc-uhs.c', 'checks/sunxi-mmc-uhs-card.h', 'checks/a733-mmc-uhs.c', 'checks/sunxi-mmc-uhs.py',
                 'checks/sunxi-mmc.c', 'checks/a733-mmc-platform.c', *sources]:
    inputs[relative] = hashlib.sha256((ROOT / relative).read_bytes()).hexdigest()
(a.output / 'result.json').write_text(json.dumps(dict(
    passed=True, physical_access=False, inputs=inputs, stack_frames=frames,
    scope='Reference facts from pinned sources; MMC core and actual storage platform with modeled card, MMIO and '
          'time under ASan/UBSan; AArch64 compilation. No native image, board, USB or card access.'), indent=2) + '\n')
print('PASS UHS-I references, core and platform models; frames: sunxi_mmc_poll %d, command_step %d bytes'
      % (frames.get('sunxi_mmc_poll', 0), frames.get('command_step', 0)))
