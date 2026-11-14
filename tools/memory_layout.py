#!/usr/bin/env python3
"""The memory layout record: every address and bound that application memory depends on.

Every pair in the record is (low, high): the first byte and the first byte
after the range. extent() converts a pair to (address, bytes).

docs/designs/application-memory-layout.md describes the layout. This module
holds the values that the host tools used to repeat in their own constants:
the manifest tool, the CPU-return host, the board image preparation and the
boot-slot planner import the record from here. The linker scripts, the table
builders and `apps/os/identity.c` still hold the same numbers as text; the
check checks/memory-layout.py compares those files with this record, so a
change in one place without the other fails on the host.

The record reproduces the layout of the images built today. Later steps of the
design compute it from the linked section sizes and the manifest.

    memory_layout.py          print the record as JSON
    memory_layout.py --get K  print the integer record[K], for the build
"""
import json
import sys

KIB, MIB, GIB = 1 << 10, 1 << 20, 1 << 30
PAGE, BLOCK = 4 * KIB, 2 * MIB


def record():
    """The layout of the images built today, as a new dictionary on every call."""
    return {
        'schema': 1,
        # Installed DRAM (12 GiB) and the first 128 MiB that EL0 reaches in the cached profile.
        'dram': (0x40000000, 0x340000000),
        'el0': (0x40000000, 0x48000000),
        'image': {
            'entry': 0x41000000,
            # linker memory regions CODE and RAM of platform/a733-smp-cached/link.ld
            'code': (0x41000000, 0x41200000),
            'ram': (0x41200000, 0x42000000),
            # The OS image (apps/os/sealed-link.patch) moves read-only data, the manifest and the sealed session out of CODE
            # into this block, which EL1 and EL0 map read-only and execute-never, and starts its RAM after it.
            'os_rodata': (0x41200000, 0x41400000),
            # image RAM as the board runner backs it up and the CPU-return host reports it
            'image_ram': (0x41000000, 0x42200000),
            # the first byte after the cached writable range (ASSERT(__bss_end <= ...))
            'bss_limit': 0x42000000,
            # CPU-return images keep heap and device pool below the optional uncached block
            'writable_ceiling': 0x41c00000,
            'uncached_block': (0x41c00000, 0x41e00000),
            'session': 0x41f00000,
        },
        'monitor': {
            'range': (0x42000000, 0x42200000),
            'result': (0x42000000, 0x42004000),
            'contract': (0x42004000, 0x42005000),
            'bss_first': 0x42005000,
            'tables': 0x421f0000,
        },
        # Input, work and output regions of a manifest, GPU buffers and the NPU pool.
        'window': (0x44000000, 0x47000000),
        # A region that does not fit the window goes in level-1 slot 1 above the EL0 window, in 2 MiB blocks, below
        # the first byte of the A733 profile that the kernel table leaves unmapped; the OS maps it for its
        # application alone (docs/designs/application-memory-layout.md).
        'slot1_area': (0x48000000, 0x79000000),
        # The upper memory above the unmapped hole 0x79000000..0x80000000: level-1 slots 2 to 12, to the end of the installed
        # memory. A manifest region may lie here too, in 2 MiB blocks (docs/designs/application-memory-layout.md, step 6b).
        'upper_area': (0x80000000, 0x340000000),
        # The part of DRAM that the board runner backs up, restores and uploads into: all that FEL addresses, since its
        # read and write commands carry 32-bit addresses (docs/designs/application-memory-layout.md).
        'runner_dram': (0x40000000, 0x100000000),
        # What a preserving run costs: the FEL rate in bytes per second, measured in docs/board-runner.md. Each
        # preserved byte is read before the run and written after it.
        'fel_bytes_per_second': 550000,
        # The extent of a boot slot's runtime and asset segments.
        'boot_slot_load': (0x41000000, 0x47000000),
        # The C library's heap. The image links heap_base bytes for the OS and the manifest's heap_bytes when they
        # are at most heap_link_max (the largest heap any image links today); a larger heap is a region of the
        # application memory ledger instead (docs/designs/application-memory-layout.md, step 1).
        'heap_base': MIB,
        'heap_link_max': 4 * MIB,
    }


def extent(pair):
    """The (address, bytes) form of a (low, high) pair, the form the runner uses."""
    return (pair[0], pair[1] - pair[0])


def device_range_valid(address, size):
    """True for a range that the OS may publish as device-visible (apps/os/device-ranges.h): 64-byte lines, in the DRAM above
    the OS image. The ranges themselves are chosen at run time and read from the return record."""
    low, high = record()['dram'][0] + (0x42200000 - 0x40000000), record()['dram'][1]
    return size > 0 and address % 64 == 0 and size % 64 == 0 and low <= address and address + size <= high


def preserve_seconds(preserved_bytes):
    """The estimated transfer time of a preserving run that backs up and restores that many bytes."""
    return 2.0 * preserved_bytes / record()['fel_bytes_per_second']


def hexed(value):
    if isinstance(value, dict):
        return {k: hexed(v) for k, v in value.items()}
    if isinstance(value, (tuple, list)):
        return [hexed(v) for v in value]
    return '%#x' % value if isinstance(value, int) and not isinstance(value, bool) else value


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--get':
        sys.stdout.write('%d\n' % record()[sys.argv[2]])
        return
    json.dump(hexed(record()), sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write('\n')


if __name__ == '__main__':
    main()
