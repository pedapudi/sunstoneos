/* SPDX-License-Identifier: GPL-2.0-or-later */
/* QEMU TCG plugin: record CPU0's executed instructions and data accesses
 * between two code addresses (the latency diagnostic's tick hooks), keeping
 * only windows in which a load of the phase word returned the selected value.
 *
 * Arguments (all addresses hexadecimal):
 *   begin=ADDR end=ADDR   window entry and exit instructions on vCPU 0
 *   mark=ADDR             repeatable; logs a marker when the address executes
 *   phase=ADDR value=N    keep a window only after a load of N from phase
 *   skip=LO-HI            repeatable; no data callbacks for code in [LO, HI)
 *   windows=N out=PATH    stop after N kept windows; text output path
 *
 * Output lines: "W n" opens a kept window, "M addr x0" a marker with the
 * argument register at that instruction, "I pc" an
 * instruction, "R pc addr size" or "W pc addr size" a data access, "E" the end.
 * checks/tick-footprint-budget.py builds it against the unmodified QEMU 10.2.1
 * qemu-plugin.h (cached under ~/.cache/sunstoneos/host/qemu-plugin-10.2.1, SHA-256
 * aebc639d01685893a9d71b9ef3afa7ea67e0be6a8959781c4057bd4974d05d4d) and the GLib
 * type declarations in tools/qemu-plugin-glib. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

enum { MARKS = 32, SKIPS = 8 };
struct rec { char kind; uint64_t pc, addr; unsigned size; };
static struct qemu_plugin_register *x0;
static uint64_t begin_addr, end_addr, phase_addr, phase_value;
static uint64_t marks[MARKS]; static unsigned nmarks;
static uint64_t skip_lo[SKIPS], skip_hi[SKIPS]; static unsigned nskips;
static unsigned long want = 20, kept;
static FILE *out;
static struct qemu_plugin_scoreboard *board;
static qemu_plugin_u64 active;
static struct rec *buf; static size_t used, cap;
static bool matched, done;

static void put(char kind, uint64_t pc, uint64_t addr, unsigned size)
{
    if (used == cap) {
        cap = cap ? cap * 2 : 1u << 16;
        buf = realloc(buf, cap * sizeof(*buf));
        if (!buf) abort();
    }
    buf[used++] = (struct rec){kind, pc, addr, size};
}
static void on_begin(unsigned vcpu, void *ud)
{
    (void)ud;
    if (vcpu || done) return;
    used = 0; matched = false;
    qemu_plugin_u64_set(active, 0, 1);
    put('M', begin_addr, 0, 0);
}
static void on_end(unsigned vcpu, void *ud)
{
    (void)ud;
    if (vcpu || !qemu_plugin_u64_get(active, 0)) return;
    put('M', end_addr, 0, 0);
    qemu_plugin_u64_set(active, 0, 0);
    if (!matched) return;
    fprintf(out, "W %lu\n", kept);
    for (size_t i = 0; i < used; ++i) {
        struct rec *r = &buf[i];
        if (r->kind == 'M') fprintf(out, "M %" PRIx64 " %" PRIx64 "\n", r->pc, r->addr);
        else if (r->kind == 'I') fprintf(out, "I %" PRIx64 "\n", r->pc);
        else fprintf(out, "%c %" PRIx64 " %" PRIx64 " %u\n", r->kind, r->pc, r->addr, r->size);
    }
    fprintf(out, "E\n");
    fflush(out);
    if (++kept >= want) done = true;
}
static void on_mark(unsigned vcpu, void *ud)
{
    if (vcpu || !qemu_plugin_u64_get(active, 0)) return;
    uint64_t arg = UINT64_MAX;
    GByteArray *buf = g_byte_array_new();
    if (x0 && qemu_plugin_read_register(x0, buf) == 8) memcpy(&arg, buf->data, 8);
    g_byte_array_unref(buf);
    put('M', (uint64_t)(uintptr_t)ud, arg, 0);
}
static void on_vcpu_init(qemu_plugin_id_t id, unsigned vcpu)
{
    (void)id;
    if (vcpu) return;
    GArray *regs = qemu_plugin_get_registers();
    qemu_plugin_reg_descriptor *d = (qemu_plugin_reg_descriptor *)regs->data;
    for (unsigned i = 0; i < regs->len; ++i)
        if (!strcmp(d[i].name, "x0")) x0 = d[i].handle;
    g_array_unref(regs);
}
static void on_insn(unsigned vcpu, void *ud)
{
    if (vcpu) return;
    put('I', (uint64_t)(uintptr_t)ud, 0, 0);
}
static void on_mem(unsigned vcpu, qemu_plugin_meminfo_t info, uint64_t vaddr, void *ud)
{
    if (vcpu || !qemu_plugin_u64_get(active, 0)) return;
    bool store = qemu_plugin_mem_is_store(info);
    put(store ? 'W' : 'R', (uint64_t)(uintptr_t)ud, vaddr, 1u << qemu_plugin_mem_size_shift(info));
    if (!store && vaddr == phase_addr) {
        qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
        uint64_t value = v.type == QEMU_PLUGIN_MEM_VALUE_U8 ? v.data.u8 :
            v.type == QEMU_PLUGIN_MEM_VALUE_U16 ? v.data.u16 :
            v.type == QEMU_PLUGIN_MEM_VALUE_U32 ? v.data.u32 : v.data.u64;
        if (value == phase_value) matched = true;
    }
}
static bool skipped(uint64_t pc)
{
    for (unsigned i = 0; i < nskips; ++i)
        if (pc >= skip_lo[i] && pc < skip_hi[i]) return true;
    return false;
}
static void on_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; ++i) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        void *ud = (void *)(uintptr_t)pc;
        if (pc == begin_addr)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, on_begin, QEMU_PLUGIN_CB_NO_REGS, NULL);
        if (pc == end_addr)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, on_end, QEMU_PLUGIN_CB_NO_REGS, NULL);
        for (unsigned m = 0; m < nmarks; ++m)
            if (pc == marks[m])
                qemu_plugin_register_vcpu_insn_exec_cb(insn, on_mark, QEMU_PLUGIN_CB_R_REGS, ud);
        if (skipped(pc)) continue;
        qemu_plugin_register_vcpu_insn_exec_cond_cb(insn, on_insn, QEMU_PLUGIN_CB_NO_REGS,
                                                    QEMU_PLUGIN_COND_NE, active, 0, ud);
        qemu_plugin_register_vcpu_mem_cb(insn, on_mem, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW, ud);
    }
}
static void on_plugin_exit(qemu_plugin_id_t id, void *p)
{
    (void)id; (void)p;
    if (out) { fprintf(out, "# kept %lu\n", kept); fclose(out); }
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info;
    const char *path = "tick-footprint.txt";
    for (int i = 0; i < argc; ++i) {
        char *v = strchr(argv[i], '=');
        if (!v) return -1;
        *v++ = 0;
        if (!strcmp(argv[i], "begin")) begin_addr = strtoull(v, NULL, 16);
        else if (!strcmp(argv[i], "end")) end_addr = strtoull(v, NULL, 16);
        else if (!strcmp(argv[i], "phase")) phase_addr = strtoull(v, NULL, 16);
        else if (!strcmp(argv[i], "value")) phase_value = strtoull(v, NULL, 0);
        else if (!strcmp(argv[i], "windows")) want = strtoul(v, NULL, 0);
        else if (!strcmp(argv[i], "out")) path = v;
        else if (!strcmp(argv[i], "mark") && nmarks < MARKS) marks[nmarks++] = strtoull(v, NULL, 16);
        else if (!strcmp(argv[i], "skip") && nskips < SKIPS) {
            char *dash = strchr(v, '-');
            if (!dash) return -1;
            skip_lo[nskips] = strtoull(v, NULL, 16); skip_hi[nskips++] = strtoull(dash + 1, NULL, 16);
        } else return -1;
    }
    if (!begin_addr || !end_addr || !phase_addr) return -1;
    out = fopen(path, "w");
    if (!out) return -1;
    board = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    active = qemu_plugin_scoreboard_u64(board);
    qemu_plugin_register_vcpu_init_cb(id, on_vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_trans);
    qemu_plugin_register_atexit_cb(id, on_plugin_exit, NULL);
    return 0;
}
