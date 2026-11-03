#!/usr/bin/env python3
"""Check that board and storage build identities follow contents and flags, not paths.

The check copies the repository into a scratch directory and only configures it;
nothing is compiled. Pass the usual central-build cache options after "--".
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [
    ['--work', '{output}', '--', '-DFREERTOS_DIR={cache}/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2', '-DTINYUSB_DIR={cache}/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973', '-DSUNSTONE_LLD={cache}/host/usr/lib/llvm-21/bin/ld.lld', '-DSUNSTONE_NETWORK_C_HEADERS={cache}/host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include', '-DSUNSTONE_RSN_DIR={cache}/sources/radio/stacks/wpa_supplicant-2.12', '-DSUNSTONE_TCP_DIR={cache}/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7', '-DSUNSTONE_BOARD_SUPERVISED=ON', '-DSUNSTONE_GPU_FIRMWARE={cache}/evidence/gpu-render-20261017/native1/fixtures/gpu.fw', '-DNEWLIB_ROOT={cache}/host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf', '-DNEWLIB_SMP_ROOT={cache}/host/newlib-smp-bd24e79bebf17c01', '-DSUNSTONE_BT_SECURITY_DIR={cache}/sources/bluetooth-security-zephyr-v4.2.1'],
]
IDENTITIES = ("board-application/build-id.txt", "storage-maintenance/build-id.txt")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="scratch directory for source copies and builds")
    parser.add_argument("cmake_args", nargs="*", help="cache options such as -DFREERTOS_DIR=...")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="build-identity-", dir=args.work))
    options = [x for x in args.cmake_args if not x.startswith("-DCMAKE_TOOLCHAIN_FILE=")]

    def copy(name):
        tree = work / name
        shutil.copytree(ROOT, tree, ignore=shutil.ignore_patterns(".git", "__pycache__"))
        return tree

    def configure(tree, build, *extra):
        result = subprocess.run(["cmake", "-S", str(tree), "-B", str(work / build),
                                 f"-DCMAKE_TOOLCHAIN_FILE={tree}/cmake/aarch64-clang.cmake",
                                 *options, *extra], capture_output=True, text=True)
        return result

    def identities(tree, build, *extra):
        result = configure(tree, build, *extra)
        assert result.returncode == 0, result.stdout + result.stderr
        return tuple((work / build / name).read_text().strip() for name in IDENTITIES)

    first, second = copy("checkout-a"), copy("other/checkout-b")
    base = identities(first, "build-a")
    assert identities(second, "elsewhere/build-b") == base, "identity depends on checkout or build path"

    # A private network header and a command-line definition change only the board.
    header = second / "network/include/FreeRTOSIPConfig.h"
    header.write_text(header.read_text() + "\n/* identity check */\n")
    changed = identities(second, "elsewhere/build-b")
    assert changed[0] != base[0] and changed[1] == base[1], (base, changed)
    flagged = identities(first, "build-flags", "-DCMAKE_C_FLAGS=-DSUNSTONE_IDENTITY_CHECK=1")
    assert flagged[0] != base[0] and flagged[1] != base[1], (base, flagged)

    # Option-selected code is part of the board identity.
    irq = identities(first, "build-irq", "-DSUNSTONE_BOARD_USB2_IRQ=ON")
    assert irq[0] != base[0], (base, irq)

    # A FreeRTOS tree that differs from the lock is refused at configure time.
    freertos = next(x.split("=", 1)[1] for x in options if x.startswith("-DFREERTOS_DIR="))
    modified = work / "freertos"
    shutil.copytree(freertos, modified)
    tasks = modified / "tasks.c"
    tasks.write_bytes(tasks.read_bytes() + b"\n")
    result = configure(first, "build-freertos", f"-DFREERTOS_DIR={modified}")
    assert result.returncode != 0 and "does not match the freertos files" in result.stderr, result.stderr

    # The central build states that TinyUSB is required.
    result = configure(first, "build-no-tinyusb", "-DTINYUSB_DIR=")
    assert result.returncode != 0 and "Set TINYUSB_DIR" in result.stderr, result.stderr
    print(f"build identity checks passed in {work}")


if __name__ == "__main__":
    main()
