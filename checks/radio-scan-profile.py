#!/usr/bin/env python3
"""Check the scan-only radio profile and its producer against the native C layout and initializations."""
import argparse
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root/'tools'))
import radio_scan_prepare as prep

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=pathlib.Path, required=True)
parser.add_argument('--freertos', type=pathlib.Path, required=True)
parser.add_argument('--tcp', type=pathlib.Path, required=True)
parser.add_argument('--firmware', type=pathlib.Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
sources = ['checks/radio-scan-profile.c', 'drivers/net/a733_radio_platform.c', 'drivers/storage/sunxi_sdio.c',
           'drivers/storage/sdio_card.c', 'drivers/storage/storage_digest.c', 'drivers/clock/a733_clock_leases.c',
           'drivers/power/a733_rail_leases.c', 'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c',
           'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c', 'drivers/net/aic8800_firmware.c',
           'drivers/net/aic8800_wifi.c', 'drivers/net/aic8800_runtime.c', 'drivers/net/aic8800_sdio_codec.c']
binary = args.output/'radio-scan-profile'
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-DSUNSTONE_AIC8800_RSN=1',
         '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-Inetwork/include', '-Iinclude', '-I'+str(args.freertos/'include'),
         '-I'+str(args.freertos/'portable/GCC/ARM_AARCH64_SRE'), '-I'+str(args.tcp/'source/include'),
         '-I'+str(args.tcp/'source/portable/Compiler/GCC')]
build = subprocess.run(['/usr/bin/clang-21', *flags, *sources, '-o', str(binary)], cwd=root, capture_output=True, text=True)
if build.returncode:
    sys.exit(build.stdout + build.stderr)
# The producer's constants are the compiled offsets.
native = {}
for line in subprocess.run([str(binary), 'offsets'], capture_output=True, text=True, check=True).stdout.splitlines():
    name, value = line.split()
    native[name] = int(value)
assert native['size'] == prep.PROFILE_BYTES
for name in ('platform', 'radio_identity', 'transport', 'bootstrap', 'wifi', 'firmware_address', 'firmware_bytes',
             'session_owner', 'runtime_lease_us', 'renew_margin_us', 'binding_timeout_us', 'command_timeout_us',
             'association_timeout_us', 'platform_authority', 'platform_evidence'):
    assert prep.PROFILE[name] == native[name], name
for name in ('owner', 'topology', 'fabric', 'fabric_hz', 'expected', 'desired', 'card'):
    assert prep.PLATFORM[name] == native['platform.' + name], name
# A profile for a prefix with the fields the producer reads.
prefix = bytearray(592)
prefix[prep.SESSION['topology']:prep.SESSION['topology'] + 8] = (101).to_bytes(8, 'little')
for k, v in enumerate((0x3000002, 0x83000000, 0x81000000)):
    prefix[prep.SESSION['fabric'] + 4*k:prep.SESSION['fabric'] + 4*k + 4] = v.to_bytes(4, 'little')
for k, v in enumerate((200000000, 600000000, 600000000)):
    prefix[prep.SESSION['fabric_hz'] + 4*k:prep.SESSION['fabric_hz'] + 4*k + 4] = v.to_bytes(4, 'little')
profile, uploads = prep.apply(prefix, bytes(range(1, 33)), args.firmware)
(args.output/'profile.bin').write_bytes(profile)
run = subprocess.run([str(binary), str(args.output/'profile.bin'), str(args.firmware)], capture_output=True, text=True)
sys.stdout.write(run.stdout + run.stderr)
assert run.returncode == 0 and 'PASS' in run.stdout
assert len(uploads) == 5 and uploads[0][0] == prep.PROFILE_ADDRESS
