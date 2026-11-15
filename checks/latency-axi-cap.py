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

With --build, a build of the cap image, it also checks the image's exits as linked. The gate builds no cap image, so
GATE_RUNS does not give --build. The exit clear must run at EL1: run 1 stored from a733_qualification_worker_done,
which the terminal tasks call at EL0, and faulted on the CurrentEL read. The check fails unless:
1. every reference to the terminal wrap (apps/latency/axi-exit.S) is a branch inside a733_cached_el1_vectors that only
   the vectors' synchronous slots reach, and nothing takes its address (ADR, ADRP with ADD, or an 8-byte word);
2. every caller of the stop-hook wrap (apps/latency/axi-privilege.c) is one of terminal, failure_claim,
   a733_cached_el1_vectors, a733_smp_exit_privileged and a733_cached_el3_fault; each is reached from the EL1 or EL3
   vectors or from an EL1 task, and none from an EL0 entry; nothing takes the wrap's address;
3. no function of the two wrapper units that an EL0 entry reaches executes an MRS or MSR that EL0 cannot.
The EL0 entries are the terminal tasks, os_cpu_return_el0_fault and every function of the application's units whose
address is taken, less the EL1 tasks. The EL1 tasks are the entries the privilege decision compares against: the code
of pxPortInitialiseStack and of each task_privileged in os_applications, and everything they call. Reach follows direct
branches. An indirect branch may go to any symbol whose address is in a data word or is taken by code already reached,
except the privilege decision and the EL1 tasks, which task creation enters rather than a call. Two mutants must fail:
the image relinked with 847f234e3's worker_done wrap fails rule 3, and the image with a function an EL0 task calls
patched to take the terminal wrap's address fails rule 1. The check refuses if the built ELF or map changes.
"""
import argparse
import bisect
import collections
import hashlib
import importlib.util
import json
import shlex
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


# The static part: the exits of the linked image (--build). It does not run in the gate, which builds no cap image.
STOP_WRAP = '__wrap_a733_storage_maintenance_stop'
TERMINAL_WRAP = '__wrap_a733_qualification_terminal_privileged'
EL1_VECTORS, EL3_VECTORS = 'a733_cached_el1_vectors', 'a733_cached_el3_vectors'
SYNC_SLOTS = {0, 4, 8, 12}  # the synchronous slot of each of the four vector groups; SVC enters through one
STOP_CALLERS = {'terminal', 'failure_claim', EL1_VECTORS, 'a733_smp_exit_privileged', 'a733_cached_el3_fault'}
WRAPPER_UNITS = ('/apps/latency/axi-privilege.c.obj', '/apps/latency/axi-exit.S.obj')
APPLICATION_UNITS = 'CMakeFiles/latency.dir/'
EL0_ENTRIES = (('terminal', 'apps/os/cpu-return.c'), ('os_cpu_return_el0_fault', 'apps/os/cpu-return.c'))
IMAGE = 'sunstone-a733-os-latency'
# Run 1's wrapper (commit 847f234e3): the stores in front of a733_qualification_worker_done, which the terminal tasks
# call at EL0. Mutant 1 appends it to apps/latency/axi-privilege.c and links it with --wrap.
WORKER_DONE_WRAP = r'''
extern _Noreturn void __real_a733_qualification_worker_done(unsigned core);
_Noreturn void __wrap_a733_qualification_worker_done(unsigned core)
{
    clear_cap();
    __real_a733_qualification_worker_done(core);
}
'''


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def privileged_register(word):
    """Whether an MRS or MSR names a register or PSTATE field that EL0 cannot access: op1 other than 3, and for the
    PSTATE form also DAIFSet and DAIFClr, which trap at EL0 unless SCTLR_EL1.UMA is set."""
    if word & 0xffd00000 == 0xd5100000:  # MRS and MSR (register): op0 2 or 3
        return (word >> 16) & 7 != 3
    if word & 0xfff8f01f == 0xd500401f:  # MSR (immediate) to a PSTATE field
        return not ((word >> 16) & 7 == 3 and (word >> 5) & 7 in (1, 2, 4))
    return False


class Exits:
    """The references, call graph and EL0 reach of one linked image."""

    def __init__(self, irq, build, elf, map_path, wrapper_units=WRAPPER_UNITS):
        from elftools.elf.elffile import ELFFile
        self.irq, self.image, self.cache = irq, irq.Image(build, elf, map_path), {}
        self.wrapper_units = wrapper_units
        symbols = ELFFile(open(elf, 'rb')).get_section_by_name('.symtab')
        self.objects = {s.name: (s['st_value'], s['st_size']) for s in symbols.iter_symbols() if s['st_info']['type'] == 'STT_OBJECT'}
        self.refresh()

    def refresh(self):
        """Recompute everything derived from the code bytes, after a mutant patches them."""
        image = self.image
        self.cache.clear()
        self.instructions = [i for base, data in image.code for i in image.disassembler.disasm(bytes(data), base) if i.id]
        self.takes = collections.defaultdict(set)  # symbol -> the symbols its code takes the address of
        for start in image.starts:
            for value in self.addresses(self.irq.instructions_from(image, start)[0]):
                if value in image.names:
                    self.takes[start].add(value)
        self.data_taken = {v for base, data in image.data for v in struct.unpack_from('<%dQ' % (len(data) // 8), data)
                           if v in image.names}
        self.privileged, self.accepted = self.privilege_decision()
        roots = {a for name, unit in EL0_ENTRIES for a in image.defined(name, unit)}
        assert len(roots) == len(EL0_ENTRIES), 'an EL0 entry of the OS is missing'
        roots |= {a for a in image.taken if APPLICATION_UNITS in (image.unit(a) or '')}
        self.el0 = self.reach(roots - self.accepted)
        self.entered = self.reach(self.vector_roots(EL1_VECTORS) | self.vector_roots(EL3_VECTORS) | self.accepted)

    def addresses(self, instructions):
        """(site, value) of each ADR, and ADRP with ADD, in a run of one symbol's instructions."""
        from capstone import arm64
        pages = {}
        for instruction in instructions:
            if not instruction.id:
                continue
            operands, mnemonic = instruction.operands, instruction.mnemonic
            if mnemonic == 'adrp':
                pages[operands[0].reg] = operands[1].imm
            elif mnemonic == 'adr':
                yield operands[1].imm
            elif mnemonic == 'add' and len(operands) == 3 and operands[2].type == arm64.ARM64_OP_IMM and operands[1].reg in pages:
                yield pages[operands[1].reg] + operands[2].imm

    def owner(self, address):
        image = self.image
        return image.starts[bisect.bisect_right(image.starts, address) - 1]

    def references(self, target):
        """(branch sites, address sites) of target: direct branches, and ADR, ADRP with ADD and 8-byte words in the
        code and data sections that equal it."""
        from capstone import arm64
        branches, taken, pages, owner = [], [], {}, None
        for instruction in self.instructions:
            at, mnemonic, text, operands = instruction.address, instruction.mnemonic, instruction.op_str, instruction.operands
            if self.owner(at) != owner:
                pages, owner = {}, self.owner(at)
            if (mnemonic in self.irq.BRANCHES or mnemonic.startswith('b.')) and '#' in text:
                if int(text.split('#')[-1], 16) == target:
                    branches.append(at)
            elif mnemonic == 'adrp':
                pages[operands[0].reg] = operands[1].imm
            elif mnemonic == 'adr' and operands[1].imm == target or mnemonic == 'add' and len(operands) == 3 \
                    and operands[1].reg in pages and operands[2].type == arm64.ARM64_OP_IMM \
                    and pages[operands[1].reg] + operands[2].imm == target:
                taken.append(at)
        for base, data in self.image.code + self.image.data:
            taken += [base + 8 * i for i, v in enumerate(struct.unpack_from('<%dQ' % (len(data) // 8), data)) if v == target]
        return branches, taken

    def slots(self, table):
        """Address in the vector table -> the slots whose code reaches it without leaving the table."""
        image = self.image
        base = image.address[table]
        end, reached = base + image.extent(base), collections.defaultdict(set)
        for slot in range(16):
            seen, pending = set(), [base + 0x80 * slot]
            while pending:
                at = pending.pop()
                while base <= at < end and at not in seen:
                    seen.add(at)
                    instruction = next(image.disassembler.disasm(image.words(at, 4), at), None)
                    if instruction is None or not instruction.id or instruction.mnemonic in ('eret', 'ret', 'br', 'blr'):
                        break
                    mnemonic, text = instruction.mnemonic, instruction.op_str
                    if (mnemonic in self.irq.BRANCHES[1:] or mnemonic.startswith('b.')) and '#' in text:
                        pending.append(int(text.split('#')[-1], 16))
                    if mnemonic == 'b':
                        at = int(text.split('#')[-1], 16)
                        continue
                    at += 4
            for at in seen:
                reached[at].add(slot)
        return reached

    def vector_roots(self, table):
        """The symbols outside the table that its code branches to or calls."""
        image, base = self.image, self.image.address[table]
        roots = set()
        for at in self.slots(table):
            instruction = next(image.disassembler.disasm(image.words(at, 4), at))
            if (instruction.mnemonic in self.irq.BRANCHES or instruction.mnemonic.startswith('b.')) and '#' in instruction.op_str:
                target = int(instruction.op_str.split('#')[-1], 16)
                if not base <= target < base + image.extent(base):
                    roots.add(target)
        return roots

    def privilege_decision(self):
        """(the privilege decision, the task entries it compares against). Task creation asks pxPortInitialiseStack,
        which calls the board predicate, which calls each task_privileged of the os_applications table
        (struct os_application in apps/os/application.h: name, task_privileged, pins, start). An entry is privileged
        when one of them compares it equal, so every entry whose address their code takes is counted as an EL1 task."""
        image = self.image
        base, size = self.objects['os_applications']
        assert size % 32 == 0, 'struct os_application changed size'
        data = next(bytes(d) for b, d in image.data if b <= base < b + len(d))
        offset = base - next(b for b, d in image.data if b <= base < b + len(d))
        column = [struct.unpack_from('<Q', data, offset + i + 8)[0] for i in range(0, size, 32)]
        assert column and all(a in image.functions for a in column), 'os_applications holds no task_privileged column'
        decision, pending = set(), [image.address['pxPortInitialiseStack']] + column
        while pending:
            at = pending.pop()
            if at not in decision:
                decision.add(at)
                pending += [t for t in self.irq.scan(image, at, self.cache)[0] if image.in_code(t)]
        return decision, set().union(*(self.takes[a] for a in decision)) - decision

    def reach(self, roots):
        """Symbol -> (the symbol it was first reached from, how). An indirect branch may go to any symbol whose address
        is in a data word or is taken by code already reached; the privilege decision and the EL1 task entries it
        accepts are not such targets, since task creation, not a call, enters them."""
        image, excluded = self.image, self.privileged | self.accepted
        seen, stack, pending, indirect_site = {}, [(r, None, 'entry') for r in sorted(roots)], set(self.data_taken), None
        while True:
            while stack:
                at, parent, how = stack.pop()
                if at in seen:
                    continue
                seen[at] = (parent, how)
                targets, indirect, _ = self.irq.scan(image, at, self.cache)
                stack += [(t, at, 'direct') for t in targets if image.in_code(t)]
                pending |= self.takes[at]
                if indirect and indirect_site is None:
                    indirect_site = at
            fresh = sorted(a for a in pending - excluded if a not in seen) if indirect_site is not None else []
            if not fresh:
                return seen
            stack += [(a, indirect_site, 'indirect') for a in fresh]

    def chain(self, seen, at):
        names = []
        while at is not None and len(names) < 12:
            names.append(self.image.name(at))
            at = seen[at][0]
        return ' <- '.join(names)

    def wrapper_unit(self, at):
        return (self.image.unit(at) or '').endswith(self.wrapper_units)

    def rule1(self):
        image = self.image
        if TERMINAL_WRAP not in image.address:
            return ['rule 1: %s is not linked' % TERMINAL_WRAP]
        branches, taken = self.references(image.address[TERMINAL_WRAP])
        slots = self.slots(EL1_VECTORS)
        problems = ['rule 1: %x in %s takes the address of the terminal wrap' % (a, image.name(self.owner(a))) for a in taken]
        for at in branches:
            if not slots.get(at) or not slots[at] <= SYNC_SLOTS:
                problems.append('rule 1: the branch at %x in %s is not on an SVC path of %s (slots %s)'
                                % (at, image.name(self.owner(at)), EL1_VECTORS, sorted(slots.get(at, ()))))
        if not branches:
            problems.append('rule 1: nothing branches to the terminal wrap')
        return problems

    def rule2(self):
        image = self.image
        if STOP_WRAP not in image.address:
            return ['rule 2: %s is not linked' % STOP_WRAP]
        branches, taken = self.references(image.address[STOP_WRAP])
        problems = ['rule 2: %x in %s takes the address of the stop-hook wrap' % (a, image.name(self.owner(a))) for a in taken]
        for caller in sorted({self.owner(a) for a in branches}):
            name = image.name(caller)
            if name not in STOP_CALLERS:
                problems.append('rule 2: %s calls the stop-hook wrap' % name)
            if caller in self.el0:
                problems.append('rule 2: %s calls the stop-hook wrap and is reached from an EL0 entry: %s' % (name, self.chain(self.el0, caller)))
            if caller not in self.entered and name not in (EL1_VECTORS, EL3_VECTORS):
                problems.append('rule 2: %s calls the stop-hook wrap and no vector or EL1 task reaches it' % name)
        if not branches:
            problems.append('rule 2: nothing calls the stop-hook wrap')
        return problems

    def rule3(self):
        image, problems = self.image, []
        for start in sorted(a for a in self.el0 if self.wrapper_unit(a)):
            seen, pending = set(), [start]
            while pending:
                at = pending.pop()
                if at in seen:
                    continue
                seen.add(at)
                instructions, _ = self.irq.instructions_from(image, at)
                for instruction in instructions:
                    word = struct.unpack('<I', instruction.bytes)[0] if instruction.id else 0
                    if instruction.mnemonic in ('mrs', 'msr') and privileged_register(word):
                        problems.append('rule 3: %x in %s: %s %s, reached at EL0: %s' % (
                            instruction.address, image.name(at), instruction.mnemonic, instruction.op_str,
                            self.chain(self.el0, start)))
                pending += [t for t in self.irq.scan(image, at, self.cache)[0] if self.wrapper_unit(t)]
        return problems

    def problems(self):
        return self.rule1() + self.rule2() + self.rule3()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relink_worker_done(build, link, output):
    """Mutant 1: the image linked with 847f234e3's worker_done wrap added, in output; the build is not touched."""
    commands = json.loads((build / 'compile_commands.json').read_text())
    entry = next(e for e in commands if e['file'].endswith('apps/latency/axi-privilege.c'))
    source = output / 'axi-privilege-worker-done.c'
    source.write_text((ROOT / 'apps/latency/axi-privilege.c').read_text() + WORKER_DONE_WRAP)
    words = shlex.split(entry['command'])
    obj = output / 'axi-privilege-worker-done.o'
    words[words.index('-o') + 1] = str(obj)
    words[words.index(entry['file'])] = str(source)
    subprocess.run(words, cwd=entry['directory'], check=True)
    words = shlex.split(link.read_text())
    elf, mapped = output / 'mutant-worker-done.elf', output / 'mutant-worker-done.map'
    words[words.index('-o') + 1] = str(elf)
    words = [('-Wl,-Map,%s' % mapped) if w.startswith('-Wl,-Map,') else str(obj) if w.endswith('/apps/latency/axi-privilege.c.obj') else w
             for w in words]
    assert str(obj) in words, 'the link command names no axi-privilege.c object'
    words.insert(words.index('-o'), '-Wl,--wrap=a733_qualification_worker_done')
    subprocess.run(words, cwd=build, check=True)
    return elf, mapped


def static_check(build, output):
    irq = load('irq_general_regs', ROOT / 'checks/irq-general-regs.py')
    found = [(e, m) for target, e, m, _ in irq.linked_images(build) if target == IMAGE]
    assert len(found) == 1, 'the build links no %s' % IMAGE
    elf, map_path = found[0]
    before = digest(elf), digest(map_path)

    exits = Exits(irq, build, elf, map_path)
    problems = exits.problems()
    assert not problems, '\n'.join(problems)
    stop = exits.image.address[STOP_WRAP]
    callers = sorted(exits.image.name(exits.owner(a)) for a in exits.references(stop)[0])
    print('image %s: rules 1-3 hold; the stop-hook wrap is called from %s; %d symbols reached from EL0 entries'
          % (before[0][:16], ', '.join(callers), len(exits.el0)))

    # Mutant 2: a function an EL0 task calls takes the terminal wrap's address.
    image = exits.image
    victim = min(a for a, (parent, how) in exits.el0.items()
                 if how == 'direct' and APPLICATION_UNITS in (image.unit(a) or '') and a in image.sized and parent is not None
                 and exits.el0[parent][1] == 'entry')
    target, original = image.address[TERMINAL_WRAP], image.words(victim, 8)
    pages = ((target >> 12) - (victim >> 12)) & 0x1fffff
    image.patch(victim, 0x90000010 | (pages & 3) << 29 | (pages >> 2) << 5)  # adrp x16, target
    image.patch(victim + 4, 0x91000210 | (target & 0xfff) << 10)  # add x16, x16, :lo12:target
    exits.refresh()
    mutated = exits.problems()
    assert mutated and all(p.startswith('rule 1:') for p in mutated) and any(image.name(victim) in p for p in mutated), mutated
    print('mutant 2 (%s takes the terminal wrap address) fails rule 1: %s' % (image.name(victim), mutated[0]))
    image.patch(victim, struct.unpack_from('<I', original)[0])
    image.patch(victim + 4, struct.unpack_from('<I', original, 4)[0])

    # Mutant 1: run 1's worker_done wrap.
    link = build / 'CMakeFiles' / (IMAGE + '.dir') / 'link.txt'
    mutant = Exits(irq, build, *relink_worker_done(build, link, output), WRAPPER_UNITS + ('/axi-privilege-worker-done.o',))
    wrap = mutant.image.address.get('__wrap_a733_qualification_worker_done')
    terminal = next(iter(mutant.image.defined('terminal', 'apps/os/cpu-return.c')))
    assert wrap is not None and wrap in mutant.irq.scan(mutant.image, terminal, mutant.cache)[0], \
        'the mutant terminal does not call the worker_done wrap'
    mutated = mutant.problems()
    witness = [p for p in mutated if p.startswith('rule 3:') and 'currentel' in p.lower()
               and '__wrap_a733_qualification_worker_done' in p]
    assert witness and not mutant.rule1(), mutated
    print('mutant 1 (847f234e3 worker_done wrap) fails rule 3: %s' % witness[0])
    assert (digest(elf), digest(map_path)) == before, 'the check changed the built image'
    print('PASS latency-axi-cap static: terminal wrap only from the EL1 SVC path, stop-hook wrap only from EL1 and EL3 '
          'exits, no privileged register access reached from EL0; both mutants refused; image %s unchanged' % before[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--build', type=Path, help='a build of %s with SUNSTONE_LATENCY_AXI_CAP: also check its exits' % IMAGE)
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
    if args.build:
        static_check(args.build.resolve(), args.output)


if __name__ == '__main__':
    main()
