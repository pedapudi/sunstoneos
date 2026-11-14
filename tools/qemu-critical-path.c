/* SPDX-License-Identifier: GPL-2.0-or-later */
/* QEMU TCG plugin: count the instructions each vCPU executes, so that a check can measure a wake by the work that it needs and not by
 * the elapsed time of a round-robin emulator.
 *
 * Under -accel tcg,thread=single -icount shift=0 the eight vCPUs take turns on one host thread and guest time is the sum of the
 * instructions they all execute. A core that spins while it waits for another (the model's priority-1 busy task, a lock or an IPI wait)
 * therefore adds its instructions to the elapsed time of every wake; on hardware the cores run in parallel and it adds nothing.
 * checks/scheduler-yield-qemu.c calls an empty function per core at the wake interrupt and at the served task. The plugin writes one line
 * per call with the cumulative instruction count of every vCPU and the cumulative count of the instructions that every vCPU executed
 * inside the spin function, so that the check can subtract the two snapshots and keep the owner's instructions and the victim's own
 * instructions outside the spin: the critical path.
 *
 * Arguments (hexadecimal addresses): lo=ADDR hi=ADDR  the marker functions [lo, hi), 8 bytes each, even slots wake, odd slots served;
 *                                    spin=LO-HI       the spin function; out=PATH
 * Output: "M OFFSET VCPU T0 .. T7 S0 .. S7\n" per marker call, OFFSET relative to lo.
 * Built by checks/scheduler-control-linked.py against the unmodified QEMU 10.2.1 qemu-plugin.h (SHA-256 checked there) and the GLib
 * type declarations in tools/qemu-plugin-glib. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

enum { VCPUS = 8 };
struct block { uint64_t pc; unsigned insns; };
static uint64_t lo_pc, hi_pc, spin_lo, spin_hi;
static uint64_t total[VCPUS], spin[VCPUS];
static FILE *out;

static void block_exec(unsigned vcpu, void *userdata)
{
    const struct block *block = userdata;
    if (vcpu >= VCPUS) return;
    if (block->pc >= lo_pc && block->pc < hi_pc) {
        fprintf(out, "M %" PRIx64 " %u", block->pc - lo_pc, vcpu);
        for (unsigned i = 0; i < VCPUS; ++i) fprintf(out, " %" PRIu64, total[i]);
        for (unsigned i = 0; i < VCPUS; ++i) fprintf(out, " %" PRIu64, spin[i]);
        fputc('\n', out);
    }
    total[vcpu] += block->insns;
    if (block->pc >= spin_lo && block->pc < spin_hi) spin[vcpu] += block->insns;
}
static void block_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;
    struct block *block = malloc(sizeof(*block));
    if (!block) abort();
    block->pc = qemu_plugin_tb_vaddr(tb);
    block->insns = (unsigned)qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, block_exec, QEMU_PLUGIN_CB_NO_REGS, block);
}
static void finish(qemu_plugin_id_t id, void *userdata) { (void)id; (void)userdata; fclose(out); }

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    (void)info;
    const char *path = NULL;
    for (int i = 0; i < argc; ++i) {
        if (!strncmp(argv[i], "lo=", 3)) lo_pc = strtoull(argv[i] + 3, NULL, 16);
        else if (!strncmp(argv[i], "hi=", 3)) hi_pc = strtoull(argv[i] + 3, NULL, 16);
        else if (!strncmp(argv[i], "spin=", 5)) {
            char *dash;
            spin_lo = strtoull(argv[i] + 5, &dash, 16);
            spin_hi = strtoull(dash + 1, NULL, 16);
        } else if (!strncmp(argv[i], "out=", 4)) path = argv[i] + 4;
    }
    if (!path || !(out = fopen(path, "w"))) return -1;
    qemu_plugin_register_vcpu_tb_trans_cb(id, block_trans);
    qemu_plugin_register_atexit_cb(id, finish, NULL);
    return 0;
}
