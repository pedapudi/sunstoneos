#!/usr/bin/env python3
"""Check that the EL3 stop paths are trace-identical to the tree before the returning lifetime became a run-time branch.

  el3-stop-trace.py SUPERVISED_ELF RETURNING_ELF --out DIR
  el3-stop-trace.py --record REFERENCE_SUPERVISED_ELF REFERENCE_RETURNING_ELF

The stop paths are a733_smp_stop and the EL3 fatal vector (a733_cached_el3_fault through its vector slots). The check
runs them in the unicorn model of checks/cpu-runtime-qualification.py and records, per case, how the path ended, every
store and load below 0x40000000 (MMIO, including the RTC breadcrumb words at 0x07090100 and WDT0 at 0x02050000), the
stores to the result record at 0x42000000 and the stores to the core's EL3 fault record.

The cases:
  - a733_smp_stop for every status from 0 to 16 and one above 15, with WDT0 disarmed, in the resident mode 0x41 and
    in the host-armed mode 0xb1;
  - the fatal vector for a status-0 SMC (the success return), a status-4 SMC (an EL1 fault), an EL3 exception, on
    core 0 and core 3, with the EL3 identity map checks passing and with them failing;
under the EL3 lifetime word 1 (supervised), 2 (resident) and 3 (returning), and the word 0 with the supervised,
resident or returning contract magic staged (a stop during admission, before the word is written; the fatal vector
runs only for a core whose EL3 record exists, which a733_cached_prepare adopts after admission, so the word-0 cases are
stop cases only).

The reference (checks/data/el3-stop-trace.json) holds the same cases from the tree before the change, d8a6b513: its
supervised OS image for the words 1, 2 and 0 with the supervised or resident magic, and its CPU-return latency image
for the word 3 and the word 0 with the returning magic. The supervised image here must match all of them, since it
admits every lifetime. The returning image must match the returning cases.

The check also requires that no stop path loads the EL1 copy of the lifetime (os_sealed, apps/os/sealed.c) and that
every path that reaches the lifetime branch loads the EL3 word (a733_smp_lifetime). The model gives the EL1 copy a value
different from the EL3 word, so a path that read it would also change its trace. Three mutations of the supervised
image must fail the comparison: ungating the crumb store in a733_smp_stop, dropping the returning success-dispatch
branch of the fatal vector, and pointing a733_smp_stop's lifetime load at the EL1 copy.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / 'checks/data/el3-stop-trace.json'
GATE_RUNS = [['{elf:sup}', '{elf:ret-latency}', '--out', '{output}']]
CHECK_INPUTS = ['apps/os/boot.patch', 'apps/os/vectors.patch', 'apps/os/sealed-lifetime.h', 'apps/os/admission.c']
spec = importlib.util.spec_from_file_location('stop_trace_prior', ROOT / 'checks/cpu-runtime-qualification.py')
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)

MAGIC = {'sup': 0x415337334f534331, 'res': 0x415337334f524331, 'ret': 0x415337334f544331}
RESULT = 0x42000000
RESULT_BYTES = 16 * 8
SEALED_LIFETIME = 4100        # apps/os/sealed-lifetime.h
SEALED_READY = 4096
NOP = 0xd503201f
# (label, EL3 lifetime word, staged contract magic) and the reference image that holds the expected trace.
LIFETIMES = [('word1', 1, None, 'sup'), ('word2', 2, None, 'sup'), ('word3', 3, None, 'ret'),
             ('word0-sup', 0, 'sup', 'sup'), ('word0-res', 0, 'res', 'sup'), ('word0-ret', 0, 'ret', 'ret')]
STATUSES = [*range(17), 54]
VECTORS = [
    # (name, vector slot, x0, x1, ESR_EL3): slot 8 is an SMC or exception from a lower EL in AArch64, slot 4 the current EL.
    ('smc-success', 8, 0, 0, 0x5e000000),
    ('smc-el1-fault', 8, 4, 0x96000045, 0x5e000000),
    ('el3-exception', 4, 0x1111, 0x2222, 0x96000010),
]


def local_symbols(path, *names):
    with open(path, 'rb') as f:
        table = ELFFile(f).get_section_by_name('.symtab')
        found = {}
        for s in table.iter_symbols():
            if s.name in names and s['st_info']['type'] == 'STT_OBJECT' and s['st_value'] >= RESULT:
                assert s.name not in found, ('two monitor objects named', s.name)
                found[s.name] = s['st_value']
    return found


class Trace(prior.Model):
    """The cpu-runtime-qualification model, run through a733_smp_stop rather than stopped at its entry."""

    def __init__(self, image):
        image.symbols.setdefault('a733_qualification_report', 0)   # images before the change have no report
        super().__init__(image)
        self.loads = []
        self.uc.hook_add(UC_HOOK_MEM_READ, self.load)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.unmapped)
        if not hasattr(image, 'admission'):
            image.admission = local_symbols(image.path, 'admission').get('admission')
        self.admission = image.admission

    def unmapped(self, uc, access, a, size, value, unused):
        if a >= 0x40000000:
            return False
        uc.mem_map(a & ~0xfff, 0x1000)
        return True

    def load(self, uc, access, a, size, value, unused):
        self.loads.append((self.core, a, size))

    def code(self, uc, a, size, unused):
        if a == self.s['a733_smp_stop']:
            return
        if a == self.s.get('a733_qualification_success_dispatch'):
            self.stop = 'success-dispatch'
            uc.emu_stop()
            return
        if self.image.insn(uc, a).mnemonic == 'dc':
            self.advance(a)
            return
        super().code(uc, a, size, unused)

    def lifetime(self, word, magic, sealed):
        self.put32(self.s['a733_smp_lifetime'], word)
        if self.admission is not None:
            self.put64(self.admission, MAGIC[magic] if magic else 0)
        if 'os_sealed' in self.s:
            self.put32(self.s['os_sealed'] + SEALED_READY, 1)
            self.put32(self.s['os_sealed'] + SEALED_LIFETIME, sealed)

    def el3_ready(self, ready):
        # The fault record stores only after the EL3 identity-map checks pass (checks/returning-watchdog.py).
        l1 = self.s['a733_cached_el3_l1']
        for c in range(8):
            self.reg[c].update(scr_el3=0x531, sctlr_el3=0x1005, tcr_el3=0x80823019, mair_el3=0x44ff00,
                               ttbr0_el3=l1 if ready else 0, esr_el1=0x96000045, elr_el1=0x41012340,
                               far_el1=0x13579bdf, spsr_el1=0x3c5, elr_el3=0x41055550, far_el3=0x42aa0000)
            record = self.s['a733_cached_records'] + c * 128
            self.uc.mem_write(record, struct.pack('<3Q', prior.EPOCH, c, 3))
            self.put64(record + 64, 0x1005)
            self.put64(record + 72, l1)

    def entry(self, core, address, args):
        self.s['stop_trace_entry'] = address
        self.call(core, 'stop_trace_entry', args, budget=400000)

    def summary(self, core):
        records = self.s['a733_cached_records']
        own = records + core * 128
        mmio = [['store', hex(a), size, hex(v & ((1 << 8 * size) - 1))] for _, a, size, v in self.writes if a < 0x40000000]
        reads = [['load', hex(a), size] for _, a, size in self.loads if a < 0x40000000]
        result = [[a - RESULT, size, hex(v & ((1 << 8 * size) - 1))] for _, a, size, v in self.writes
                  if RESULT <= a < RESULT + RESULT_BYTES]
        record = [[a - own, size, hex(v & ((1 << 8 * size) - 1))] for _, a, size, v in self.writes
                  if own <= a < own + 128]
        return {'stop': self.stop, 'mmio': mmio, 'mmio_loads': reads, 'result': result, 'record': record}

    def lifetime_loads(self):
        word = self.s['a733_smp_lifetime']
        sealed = self.s.get('os_sealed')
        return (any(word <= a < word + 4 for _, a, _ in self.loads),
                sealed is not None and any(sealed + SEALED_READY <= a < sealed + SEALED_READY + 8 for _, a, _ in self.loads))


def run_cases(path, patch=None, select=None):
    """Every case of one image: {lifetime label: {case: summary}}, and the cases that load the EL1 copy or skip the word."""
    out, faults = {}, []
    # One parse per run: a mutation patches every case's memory alike, so its decoded-instruction cache stays consistent.
    image = prior.fault.Image(Path(path))
    for label, word, magic, _ in LIFETIMES:
        if select and label not in select:
            continue
        sealed = 1 if word == 3 or magic == 'ret' else 3
        cases = {}
        def model():
            m = Trace(image)
            m.lifetime(word, magic, sealed)
            if patch:
                patch(m)
            return m
        for status in STATUSES:
            for mode in (0, 0x41, 0xb1):
                m = model()
                m.put32(0x02050014, mode)
                m.call(0, 'a733_smp_stop', (status, 0x700 + status))
                name = 'stop-%d-wdt%x' % (status, mode)
                cases[name] = m.summary(0)
                faults += check_loads(m, label, name, reached=True)
        # The fatal vector stores only for a core whose EL3 record a733_cached_prepare adopted, after admission wrote the word.
        for vector, slot, x0, x1, esr in VECTORS if word else ():
            for core in (0, 3):
                for ready in (True, False):
                    m = model()
                    m.put32(0x02050014, 0xb1)
                    m.el3_ready(ready)
                    m.reg[core]['esr_el3'] = esr
                    m.entry(core, m.s['a733_cached_el3_vectors'] + slot * 128, (x0, x1))
                    name = '%s-core%d-%s' % (vector, core, 'mapped' if ready else 'unmapped')
                    cases[name] = m.summary(core)
                    faults += check_loads(m, label, name, reached=True)
        out[label] = cases
    return out, faults


def check_loads(m, label, name, reached):
    word, sealed = m.lifetime_loads()
    faults = []
    if sealed:
        faults.append('%s %s loads the EL1 copy of the lifetime' % (label, name))
    if reached and not word:
        faults.append('%s %s does not load the EL3 lifetime word' % (label, name))
    return faults


def differences(traces, reference, labels):
    found = []
    for label, word, magic, source in LIFETIMES:
        if label not in labels:
            continue
        expected = reference[source]
        for name, got in traces[label].items():
            if got != expected[name]:
                found.append((label, name, got, expected[name]))
    return found


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def record(sup, ret):
    reference = {'tree': 'd8a6b513', 'images': {'sup': digest(sup), 'ret': digest(ret)}}
    sup_traces, _ = run_cases(sup, select={'word1'})
    ret_traces, _ = run_cases(ret, select={'word1'})
    # The images before the change have no stop-path branch on the lifetime: one run each holds every lifetime's trace.
    reference['sup'] = sup_traces['word1']
    reference['ret'] = ret_traces['word1']
    # One line per case, so that a re-recorded reference diffs by case.
    lines = ['{"tree": %s, "images": %s,' % (json.dumps(reference['tree']), json.dumps(reference['images']))]
    for image in ('sup', 'ret'):
        cases = ['  %s: %s' % (json.dumps(name), json.dumps(reference[image][name], sort_keys=True))
                 for name in sorted(reference[image])]
        lines.append(' %s: {\n%s\n }%s' % (json.dumps(image), ',\n'.join(cases), ',' if image == 'sup' else '}'))
    REFERENCE.write_text('\n'.join(lines) + '\n')
    print('recorded', REFERENCE.relative_to(ROOT))


def symbol_code(m, name, count=256):
    base = m.s[name]
    return [(base + 4 * i, struct.unpack('<I', m.uc.mem_read(base + 4 * i, 4))[0]) for i in range(count)]


def branch_target(m, a):
    i = m.image.insn(m.uc, a)
    if i.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') or i.mnemonic.startswith('b.'):
        return i.mnemonic, int(i.op_str.split('#')[-1], 0)
    return i.mnemonic, None


def ungate_crumb(m):
    """NOP the branches in a733_smp_stop that skip its crumb block: those before the crumb's a733_smp_refusal_detail
    call that jump past it."""
    code = symbol_code(m, 'a733_smp_stop', 64)
    call = next(a for a, _ in code if m.image.insn(m.uc, a).mnemonic == 'bl' and
                int(m.image.insn(m.uc, a).op_str.lstrip('#'), 0) == m.s['a733_smp_refusal_detail'])
    skips = [a for a, _ in code if a < call and (branch_target(m, a)[1] or 0) > call]
    assert skips, 'no crumb gate found in a733_smp_stop: update the mutation locator'
    for a in skips:
        m.put32(a, NOP)


def drop_success_dispatch(m):
    target = m.s['a733_qualification_success_dispatch']
    hits = [a for a, _ in symbol_code(m, 'a733_cached_el3_fault', 160) if branch_target(m, a) == ('b.eq', target)]
    assert len(hits) == 1, ('the returning success-dispatch branch', hits)
    m.put32(hits[0], NOP)


def stop_reads_el1_copy(m):
    """Re-encode the ADRP/LDR pair of a733_smp_stop that loads a733_smp_lifetime to load os_sealed's copy."""
    word, copy = m.s['a733_smp_lifetime'], m.s['os_sealed'] + SEALED_LIFETIME
    code = symbol_code(m, 'a733_smp_stop', 64)
    done = 0
    for i, (a, adrp) in enumerate(code):
        if adrp & 0x9f000000 != 0x90000000:
            continue
        rd = adrp & 31
        immhi, immlo = (adrp >> 5) & 0x7ffff, (adrp >> 29) & 3
        imm = (immhi << 2 | immlo)
        imm -= (1 << 21) if imm & (1 << 20) else 0
        page = (a & ~0xfff) + (imm << 12)
        for b, ldr in code[i + 1:i + 4]:
            if ldr & 0xffc00000 == 0xb9400000 and (ldr >> 5) & 31 == rd and page + ((ldr >> 10) & 0xfff) * 4 == word:
                delta = ((copy & ~0xfff) - (a & ~0xfff)) >> 12
                m.put32(a, 0x90000000 | (delta & 3) << 29 | ((delta >> 2) & 0x7ffff) << 5 | rd)
                m.put32(b, (ldr & ~(0xfff << 10)) | ((copy & 0xfff) // 4) << 10)
                done += 1
    assert done, 'no a733_smp_lifetime load found in a733_smp_stop: update the mutation locator'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('supervised', type=Path)
    parser.add_argument('returning', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--record', action='store_true')
    args = parser.parse_args()
    if args.record:
        record(args.supervised, args.returning)
        return
    if not args.out:
        parser.error('--out is required unless --record')
    args.out.mkdir(parents=True, exist_ok=False)
    reference = json.loads(REFERENCE.read_text())
    every = {label for label, *_ in LIFETIMES}
    returning = {label for label, _, _, source in LIFETIMES if source == 'ret'}
    rows = []
    for path, labels in ((args.supervised, every), (args.returning, returning)):
        traces, faults = run_cases(path, select=labels)
        found = differences(traces, reference, labels)
        assert not faults, faults[:4]
        assert not found, found[:2]
        rows.append({'image': path.name, 'lifetimes': sorted(labels),
                     'cases': sum(len(c) for c in traces.values())})
    for name, mutation, labels in (('ungated-crumb', ungate_crumb, {'word1'}),
                                   ('no-success-dispatch', drop_success_dispatch, {'word3'}),
                                   ('stop-reads-el1-copy', stop_reads_el1_copy, {'word1', 'word3'})):
        traces, faults = run_cases(args.supervised, patch=mutation, select=labels)
        found = differences(traces, reference, labels)
        assert faults or found, ('mutation not detected', name)
        rows.append({'mutation': name, 'differing_cases': len(found), 'load_faults': len(faults)})
    (args.out / 'results.json').write_text(json.dumps({'physical_access': False, 'rows': rows,
                                                       'reference': reference['tree']}, indent=1) + '\n')
    print('PASS EL3 stop trace:', ', '.join('%s %d cases' % (r['image'], r['cases']) for r in rows if 'image' in r),
          '; 3 mutations detected')


if __name__ == '__main__':
    main()
