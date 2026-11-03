#!/usr/bin/env python3
"""The pinned sources that the board and OS images need beyond FreeRTOS and TinyUSB, in one place.

A native board image (cmake/a733-board.cmake) refuses to configure without the FreeRTOS+TCP and wpa_supplicant sources it was pinned to
(sources.lock.json) and the freestanding toolchain's C headers. Checks that build such an image from a clean tree, and the gate's image recipes,
take the paths and the CMake arguments from here instead of each assembling their own.

    python3 checks/image_dependencies.py            print the -D arguments, one per line
    from image_dependencies import cmake_arguments  the same, as a list
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'


def paths(cache=CACHE):
    """The dependency directories, each checked to exist; raises FileNotFoundError naming the missing one."""
    cache = Path(cache)
    stacks = cache / 'sources/radio/stacks'
    found = {
        'freertos': next(iter(sorted((cache / 'sources').glob('FreeRTOS-Kernel-*'))), None),
        'tinyusb': next(iter(sorted((cache / 'sources').glob('tinyusb-*'))), None),
        'tcp': next(iter(sorted(d for d in stacks.glob('FreeRTOS-Plus-TCP-*') if d.is_dir())), None),
        'rsn': next(iter(sorted(d for d in stacks.glob('wpa_supplicant-*') if d.is_dir())), None),
        'headers': next(iter(sorted((cache / 'host').glob('arm-gnu-toolchain-*/aarch64-none-elf/include'))), None),
    }
    for name, path in found.items():
        if path is None or not path.is_dir():
            raise FileNotFoundError('image dependency %s is not under %s' % (name, cache))
    return found


def cmake_arguments(cache=CACHE, supervised=True):
    """-D arguments for any image: FREERTOS_DIR, TINYUSB_DIR, the network sources and headers, and the supervised board switch the board images need."""
    p = paths(cache)
    arguments = ['-DFREERTOS_DIR=%s' % p['freertos'], '-DTINYUSB_DIR=%s' % p['tinyusb'], '-DSUNSTONE_TCP_DIR=%s' % p['tcp'],
                 '-DSUNSTONE_RSN_DIR=%s' % p['rsn'], '-DSUNSTONE_NETWORK_C_HEADERS=%s' % p['headers']]
    if supervised:
        arguments.append('-DSUNSTONE_BOARD_SUPERVISED=ON')
    return arguments


if __name__ == '__main__':
    print('\n'.join(cmake_arguments()))
