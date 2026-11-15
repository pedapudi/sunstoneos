#!/usr/bin/env python3
"""Host check of the stage 3a AXI2HIF cap (apps/latency/axi-cap.c and its decoder in apps/latency/host.py).

No board is used. The cap's bridge code is compiled against an emulated register file and run through every
operation. The check fails unless:
- each operation writes exactly the vendor sequence (sunxi-nsi.c lines 401-488) with the stage 3a words;
- the three gates read open before every write, and no bridge word is read or written once a gate reads closed;
- only +0x30, +0x48 and +0x4c are written, except that the entry check zeroes whichever word it found set;
- a word that reads back other than written fails the operation;
- the C section's size equals the decoder's, and the decoder accepts a full U B B U section and refuses one with a
  failed operation, a word set at entry, operations out of order or a sub-phase outside its set and clear.
"""
import argparse
import importlib.util
import struct
import subprocess
from pathlib import Path

GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('latency_host', ROOT / 'apps/latency/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)

HARNESS = r'''
#include "axi-cap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define B0 0x0a030000u
#define B1 0x0a040000u
static uint32_t regs[2][0x60 / 4];
static uint32_t gate[3] = {0x80000003u, 0x00010001u, 0x1u};
static int gate_reads, close_after = -1, drop_offset = -1;
static uint64_t clock_ticks = 1000;
uint64_t axi_host_now(void) { return clock_ticks += 3; }
static uint32_t *word(uintptr_t a)
{
    if ((a & ~0xffffu) == B0 && (a & 0xffffu) < 0x60) return &regs[0][(a & 0xffffu) / 4];
    if ((a & ~0xffffu) == B1 && (a & 0xffffu) < 0x60) return &regs[1][(a & 0xffffu) / 4];
    printf("BAD %lx\n", (unsigned long)a); exit(3);
}
uint32_t axi_host_read(uintptr_t a)
{
    if (a == 0x02002c00u || a == 0x02002c0cu || a == 0x08000200u) {
        unsigned i = a == 0x02002c00u ? 0 : a == 0x02002c0cu ? 1 : 2;
        printf("G %u %d\n", i, gate_reads);
        uint32_t v = gate[i];
        if (close_after >= 0 && gate_reads >= close_after && i == 0) v &= 0x7fffffffu;
        ++gate_reads;
        return v;
    }
    printf("R %lx\n", (unsigned long)a);
    return *word(a);
}
void axi_host_write(uintptr_t a, uint32_t v)
{
    printf("W %lx %x\n", (unsigned long)a, v);
    if ((int)(a & 0xffffu) == drop_offset && (a & ~0xffffu) == B0) return;
    *word(a) = v;
}
static void window(struct latency_axi_window *w, unsigned sweeps)
{
    w->start = axi_host_now();
    for (unsigned c = 4; c < 8; ++c) { w->sweeps_start[c] = 100u * c; w->sweeps_end[c] = 100u * c + sweeps; }
    clock_ticks += 1200000;  /* 50 ms at 24 MHz */
    w->end = axi_host_now();
}
int main(int argc, char **argv)
{
    const char *mode = argv[1];
    latency_axi.magic = LATENCY_AXI_MAGIC; latency_axi.version = LATENCY_AXI_VERSION;
    latency_axi.bytes = sizeof(latency_axi); latency_axi.bw_b = LATENCY_AXI_BW_B; latency_axi.sat_b = LATENCY_AXI_SAT_B;
    latency_axi.bw_nonbinding = LATENCY_AXI_BW_NONBINDING; latency_axi.sat_nonbinding = LATENCY_AXI_SAT_NONBINDING;
    latency_axi.subphase_wakes = LATENCY_AXI_SUBPHASE_WAKES; latency_axi.capped_mask = LATENCY_AXI_CAPPED_MASK;
    if (!strcmp(mode, "size")) { printf("SIZE %zu\n", sizeof(struct latency_axi_report)); return 0; }
    if (!strcmp(mode, "closed")) gate[2] = 0;
    if (!strcmp(mode, "set-entry")) { regs[1][0x30 / 4] = 0x10001u; regs[1][0x48 / 4] = 753u; }
    if (!strcmp(mode, "drop48")) drop_offset = 0x48;
    if (!strncmp(mode, "close", 5) && mode[5]) close_after = atoi(mode + 5);
    if (!strcmp(mode, "full")) {
        unsigned s[8];
        unsigned n = 0;
        s[n++] = latency_axi_run(LATENCY_AXI_ENTRY);
        window(&latency_axi.warm[0], 40);
        s[n++] = latency_axi_run(LATENCY_AXI_SET_NONBINDING);
        window(&latency_axi.warm[1], 40);
        s[n++] = latency_axi_run(LATENCY_AXI_CLEAR);
        window(&latency_axi.sub[0], 30);
        s[n++] = latency_axi_run(LATENCY_AXI_SET_B);
        window(&latency_axi.sub[1], 12);
        s[n++] = latency_axi_run(LATENCY_AXI_CLEAR);
        s[n++] = latency_axi_run(LATENCY_AXI_SET_B);
        window(&latency_axi.sub[2], 12);
        s[n++] = latency_axi_run(LATENCY_AXI_CLEAR);
        window(&latency_axi.sub[3], 30);
        s[n++] = latency_axi_run(LATENCY_AXI_RESTORE);
        latency_axi.complete = 1;
        for (unsigned i = 0; i < n; ++i) printf("S %u\n", s[i]);
        const unsigned char *bytes = (const unsigned char *)&latency_axi;
        printf("HEX ");
        for (size_t i = 0; i < sizeof(latency_axi); ++i) printf("%02x", bytes[i]);
        printf("\n");
        return 0;
    }
    for (int i = 2; i < argc; ++i) printf("S %u\n", latency_axi_run((enum latency_axi_op)atoi(argv[i])));
    return 0;
}
'''

B = (0x0a030000, 0x0a040000)
OPS = {'entry': 1, 'set_nonbinding': 2, 'set_b': 3, 'clear': 4, 'restore': 5}


def run(binary, mode, *ops):
    out = subprocess.run([str(binary), mode] + [str(OPS[o]) for o in ops], capture_output=True, text=True, check=True).stdout
    events = [line.split() for line in out.splitlines()]
    return events


def writes(events):
    return [(int(e[1], 16), int(e[2], 16)) for e in events if e[0] == 'W']


def statuses(events):
    return [int(e[1]) for e in events if e[0] == 'S']


def gated(events):
    """Every write immediately follows the three gate reads, which read open in these runs."""
    return all([e[:2] for e in events[i - 3:i]] == [['G', '0'], ['G', '1'], ['G', '2']]
               for i, e in enumerate(events) if e[0] == 'W')


def vendor_set(bw, sat, mode=0):
    seq = []
    for base in B:
        seq += [(base + 0x30, mode & ~0x10000), (base + 0x48, bw), (base + 0x4c, sat), (base + 0x30, mode | 0x10001)]
    return seq


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True)
    (args.output / 'harness.c').write_text(HARNESS)
    binary = args.output / 'harness'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DLATENCY_AXI_HOST_TEST', '-I', str(ROOT / 'apps/latency'),
                    str(args.output / 'harness.c'), str(ROOT / 'apps/latency/axi-cap.c'), '-o', str(binary)], check=True)

    size = int(run(binary, 'size')[0][1])
    assert size == host.AXI_BYTES, (size, host.AXI_BYTES)

    clean = run(binary, 'clean', 'entry')
    assert statuses(clean) == [0] and not writes(clean)
    assert [e[1] for e in clean if e[0] == 'R'] == ['%x' % (b + w) for b in B for w in host.AXI_WORDS]

    closed = run(binary, 'closed', 'entry', 'set_b', 'clear', 'restore')
    assert statuses(closed) == [1, 1, 1, 1] and not writes(closed) and not any(e[0] == 'R' for e in closed), \
        'a closed gate must stop every bridge access'

    found = run(binary, 'set-entry', 'entry')
    assert statuses(found) == [3] and writes(found) == [(B[1] + 0x30, 0), (B[1] + 0x48, 0)] and gated(found)

    sequence = run(binary, 'clean', 'set_nonbinding', 'clear', 'set_b', 'clear', 'restore')
    assert statuses(sequence) == [0] * 5 and gated(sequence)
    expected = (vendor_set(4095, 1023) + [(b + 0x30, 0) for b in B] + vendor_set(753, 483) + [(b + 0x30, 0) for b in B] +
                [(b + w, 0) for b in B for w in (0x30, 0x48, 0x4c)])
    assert writes(sequence) == expected, writes(sequence)
    assert {a & 0xffff for a, _ in writes(sequence)} == {0x30, 0x48, 0x4c}

    dropped = run(binary, 'drop48', 'set_b')
    assert statuses(dropped) == [2], 'a word that reads back other than written must fail the set'

    for close_after in range(3, 16):
        partial = run(binary, 'close%d' % close_after, 'set_b')
        assert statuses(partial) == [1], close_after
        reads = [int(e[2]) for e in partial if e[0] == 'G']
        closing = next(i for i, e in enumerate(partial) if e[0] == 'G' and e[1] == '0' and int(e[2]) >= close_after)
        assert not any(e[0] in 'RW' for e in partial[closing:]), 'a bridge access followed a closed gate'
        assert reads

    full = run(binary, 'full')
    assert statuses(full) == [0] * 8
    section = bytes.fromhex(next(e[1] for e in full if e[0] == 'HEX'))
    decoded = host.decode_axi_cap(section, 24000000, 0xf0)
    assert [s['op'] for s in decoded['switches']] == ['entry', 'set_nonbinding', 'clear', 'set_b', 'clear', 'set_b', 'clear', 'restore']
    assert decoded['capped'] == [False, True, True, False] and abs(decoded['warm_ratio'] - 1.0) < 1e-9
    rate = 4 * 30 * host.AXI_SWEEP_BYTES / (1200003 / 24e6) / 1e9
    assert abs(decoded['subphases'][0]['gb_per_second'] - rate) < 1e-6, decoded['subphases'][0]['gb_per_second']

    header = struct.calcsize(host.AXI_HEADER) + 6 * struct.calcsize(host.AXI_WINDOW)
    switch = struct.calcsize(host.AXI_SWITCH)

    def refused(change):
        data = bytearray(section)
        change(data)
        try:
            host.decode_axi_cap(bytes(data), 24000000, 0xf0)
        except ValueError:
            return True
        return False
    assert refused(lambda d: struct.pack_into('<I', d, header + 3 * switch + 28, 2)), 'a failed set passed'
    assert refused(lambda d: struct.pack_into('<I', d, 40, 0x10001)), 'a word set at entry passed'
    assert refused(lambda d: struct.pack_into('<I', d, header + 3 * switch + 24, 4)), 'operations out of order passed'
    sub1 = struct.calcsize(host.AXI_HEADER) + 3 * struct.calcsize(host.AXI_WINDOW)
    assert refused(lambda d: struct.pack_into('<Q', d, sub1, 1)), 'a sub-phase before its set passed'
    assert refused(lambda d: struct.pack_into('<I', d, 108, 0)), 'an incomplete run passed'

    padded = section + bytes(-len(section) % 64)
    footer = struct.pack('<8Q', host.AXI_FOOTER_MAGIC, 1, 64 + len(padded) + 64, 64, len(section), 0, 0, 0)
    rest, tail = host.peel_axi_cap(bytes(64) + padded + footer)
    assert rest == bytes(64) and tail == section
    print('PASS latency-axi-cap: vendor write order, gates before every write, entry zeroing, read-back, decoder')


if __name__ == '__main__':
    main()
