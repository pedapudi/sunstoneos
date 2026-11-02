#!/usr/bin/env python3
"""Decode and check the load-state stream of shader (kind 1) operations in compiled network binaries.

The rules are the loader specification of docs/hardware/npu-command-formats.md (section "Shader operations"). Each rule
has a number (S1 ...) that the document and the failure messages use. The tool reads graphs with the OS decoder
(drivers/npu/nbg_*.c through checks/nbg-model.py); it executes nothing from the vendor toolkit.

  npu-shader-state.py --work NEWDIR [--show] [--check] GRAPH.nb ...   decode and check every shader operation
  npu-shader-state.py --work NEWDIR --self-test                       check the rules against a built-in stream and
                                                                      against 40 single-change variants of it

--work names a directory that does not exist yet; the decoder library is built there (clang), as tools/npu-nbg-census.py
does. --native LIB reuses an existing library instead.
"""
import argparse, ctypes as c, importlib.util, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('nbg_model', ROOT / 'checks/nbg-model.py')
nm = importlib.util.module_from_spec(spec); spec.loader.exec_module(nm)

# Section indices of the native decoder (nbg_model.h); the vendor enumeration is this value plus one.
SEC_NN, SEC_SH, SEC_PATCH, SEC_PPU_PARAM, SEC_PPU_INSTR, SEC_DYNA_IO, SEC_LOAD_STATES = 6, 8, 9, 15, 18, 19, 20
TAB_COEFF, TAB_PPU_INSTR, TAB_DYNA_IO, TAB_LOAD_STATES = 22, 24, 25, 26
NOP, STALL = 0x18000000, 0x48000000
UNIFORM_FIRST, UNIFORM_END = 0xd800, 0xd900          # register range of the uniform bank accepted by rule S4

# Rule S2: the registers that follow the uniform packets, in order. A uniform packet may stand between any two of them
# (the compiler places constant-buffer words for the shader after the first four). Count is the load-state word count.
PROLOGUE = [(0x5580, 1), (0x0e02, 1)]                 # then a STALL packet; present on the first shader of a graph only
BODY = [(0x02a4, 1), (0x0420, 1), (0x0416, 1), (0x0409, 1), (0x021f, 1), (0x0424, 1), (0x040a, 1), (0x5580, 1), (0x021a, 1),
        (0x0425, 1), (0x0427, 1), (0x0402, 3), (0x0228, 1), (0x028c, 1), (0x0e07, 1), (0x040c, 1), (0x020c, 1), (0x0201, 1),
        (0x02aa, 1), (0x0e22, 1), (0x0412, 1), (0x0e27, 1), (0x0240, 1), (0x0249, 1), (0x022e, 1), (0x0247, 1), (0x024b, 1),
        (0x024d, 1), (0x024f, 1), (0x0256, 1), (0x0257, 1), (0x0258, 1), (0x0250, 6), (0x0248, 1)]
EPILOGUE = [(0x0e03, 1), (0x0e02, 1)]                # then a STALL packet
# Rule S3: registers whose operand equals one value in every observed shader operation.
FIXED = {0x5580: 2, 0x0e02: 0x701, 0x0e03: 0x24, 0x0248: 0xbadabeeb, 0x0201: 1, 0x020c: 0x0f3f0000, 0x021a: 1, 0x021f: 0,
         0x0228: 0, 0x022e: 0x80, 0x0249: 0, 0x024b: 0, 0x024d: 0, 0x024f: 0, 0x02aa: 0, 0x0404: 0xf00, 0x0409: 0,
         0x040c: 0, 0x0412: 0, 0x0416: 0, 0x0427: 0, 0x0e22: 0x007f0000, 0x0e27: 0}
SOURCES = {0: 'command (the shader program)', 1: 'graph input', 2: 'graph output', 3: 'memory pool', 5: 'constant data',
           6: 'dynamic input table', 8: 'VIP SRAM'}


class Violation(Exception):
    pass


def words_of(b): return struct.unpack('<%dI' % (len(b) // 4), b)


def packets(w):
    """Split a stream into packets: ('load', first_word, register, [operands]), ('stall', first_word, token), ('pad', word, value).
    Rule S1."""
    out, i = [], 0
    while i < len(w):
        h = w[i]
        if h >> 27 == 1:
            count = (h >> 16) & 0x7ff
            if h & 0x04000000 or not 1 <= count <= 64 or i + 1 + count > len(w):
                raise Violation('S1 load-state header %08x at word %d' % (h, i))
            out.append(('load', i, h & 0xffff, list(w[i + 1:i + 1 + count])))
            i += 1 + count
            if (1 + count) & 1:
                # The pad word that aligns the packet to 8 bytes is skipped by the command fetcher; the compilers write
                # 0x18000000 or 0xdeadbeef. The loader writes 0x18000000 whatever the graph holds.
                if i >= len(w): raise Violation('S1 odd packet at word %d has no pad word' % (i - 1 - count))
                out.append(('pad', i, w[i])); i += 1
        elif h == STALL:
            if i + 1 >= len(w): raise Violation('S1 stall without token at word %d' % i)
            out.append(('stall', i, w[i + 1])); i += 2
        else:
            raise Violation('S1 unknown packet header %08x at word %d' % (h, i))
    return out


ALLOWED = {r for r, _ in PROLOGUE + BODY + EPILOGUE}
PINNED = {0x0e02: 0x701, 0x0e03: 0x24}              # rule S2a: loader-owned flush and synchronization writes


def check_stream(w, patches, io, view, strict=False):
    """Check one shader operation. w: state words. patches: list of 10-tuples of nbg patch fields (kind, destination,
    source, index, source_offset, transformed, ...). io: dict with 'inputs', 'outputs' (lists of logical byte sizes),
    'pool', 'vip_sram', 'coeff' (row lengths), 'dyna' (row lengths), 'ppu_len' (length of the program span of this
    operation). Returns a dict describing the operation; raises Violation on the first broken rule."""
    if len(w) % 2: raise Violation('S1 stream is not a multiple of 8 bytes')
    pk = packets(w)
    loads = [p for p in pk if p[0] == 'load']
    # S2: order. Walk the packets; uniform packets may stand anywhere after the optional prologue.
    seq, uni, regpos = [], [], {}
    stall_after = {}
    written = set()
    for p in pk:
        if p[0] == 'stall': stall_after[len(seq) - 1] = p; continue
        if p[0] != 'load': continue
        reg, data = p[2], p[3]
        if UNIFORM_FIRST <= reg < UNIFORM_END:
            if reg + len(data) > UNIFORM_END or len(data) > 4:
                raise Violation('S4 uniform packet at %04x with %d words' % (reg, len(data)))
            if written & set(range(reg, reg + len(data))):
                raise Violation('S4 uniform word written twice at %04x' % reg)
            written |= set(range(reg, reg + len(data))); uni.append(p)
        else:
            seq.append((reg, len(data))); regpos[reg] = p
    for p in loads:
        for reg, v in PINNED.items():
            if p[2] == reg and p[3][0] != v: raise Violation('S2a register %04x is %x, must be %x' % (reg, p[3][0], v))
        if not UNIFORM_FIRST <= p[2] < UNIFORM_END and p[2] not in ALLOWED:
            raise Violation('S2a register %04x is not in the observed set' % p[2])
    if any(p[0] == 'stall' and p[2] != 0x701 for p in pk): raise Violation('S2a stall token must be 0x701')
    want = BODY + EPILOGUE
    if seq[:2] == PROLOGUE:
        # prologue: the two loads, then a stall
        if 1 not in stall_after: raise Violation('S2 prologue without stall')
        seq = seq[2:]; stall_after = {k - 2: v for k, v in stall_after.items() if k != 1}
        regpos[0x5580] = None
    if strict and seq != want:
        have = [r for r, _ in seq]
        raise Violation('S2 register sequence differs from the grammar: %s' % ' '.join('%04x' % r for r in have))
    if strict and stall_after.get(len(seq) - 1, (0, 0, 0))[2] != 0x701:
        raise Violation('S2 stream does not end with a stall on token 0x701')
    for reg, value in (FIXED.items() if strict else ()):
        for p in loads:
            if p[2] <= reg < p[2] + len(p[3]) and p[3][reg - p[2]] != value:
                raise Violation('S3 register %04x is %x, observed value is %x' % (reg, p[3][reg - p[2]], value))
    for r in (0x0424, 0x0425, 0x040a):
        if r not in regpos: raise Violation('S5 register %04x missing' % r)
    def reg_value(r): return regpos[r][3][0]
    # S5: program length
    count = reg_value(0x0424)
    if reg_value(0x0425) != count - 1 or count == 0:
        raise Violation('S5 end program counter %x is not instruction count %x minus one' % (reg_value(0x0425), count))
    if count * 16 != io['ppu_len']:
        raise Violation('S5 instruction count %d x 16 differs from the program span of %d bytes' % (count, io['ppu_len']))
    # S6: patches
    dest = {}
    for p in pk:
        if p[0] != 'load': continue
        for k in range(len(p[3])): dest[(p[1] + 1 + k) * 4] = (p[2] + k, p)
    seen, program, tensors = set(), None, []
    for q in patches:
        kind, d, src, idx, off, transformed = q[:6]
        if kind != 0: raise Violation('S6 patch kind %d (only kind 0 patches a state stream)' % kind)
        if d in seen or d not in dest: raise Violation('S6 patch destination %d is not a unique operand word' % d)
        seen.add(d)
        reg, p = dest[d]
        if src not in SOURCES: raise Violation('S6 patch source %d' % src)
        if src == 0:
            if reg != 0x040a or idx != -1 or off or transformed or program is not None:
                raise Violation('S6 program patch must be the only source-0 patch, at register 040a, index -1, offset 0')
            program = d; continue
        if not UNIFORM_FIRST <= reg < UNIFORM_END: raise Violation('S6 patch of register %04x outside the uniform bank' % reg)
        if transformed not in (0, 14): raise Violation('S6 patch transformed field %d' % transformed)
        if transformed and src != 6: raise Violation('S6 transformed field %d on source %d' % (transformed, src))
        # region and extent
        if src in (1, 2):
            sizes = io['inputs' if src == 1 else 'outputs']
            if not 0 <= idx < len(sizes): raise Violation('S6 tensor index %d' % idx)
            region = sizes[idx]
        elif src == 3: region = io['pool']; ok = idx == -1
        elif src == 8: region = io['vip_sram']
        elif src == 5:
            if not 0 <= idx < len(io['coeff']): raise Violation('S6 constant-data row %d' % idx)
            region = io['coeff'][idx]
        else:
            if not 0 <= idx < len(io['dyna']): raise Violation('S6 dynamic-table row %d' % idx)
            region = io['dyna'][idx]
        if src in (3, 8) and idx != -1: raise Violation('S6 source %d index %d' % (src, idx))
        # tensor descriptor: the patched word opens a 4-word uniform packet followed by a 4-word packet
        extent = None
        if p[2] == reg and len(p[3]) == 4 and (reg - UNIFORM_FIRST) % 4 == 0:
            nxt = [u for u in uni if u[2] == reg + 4 and len(u[3]) == 4]
            if nxt and src != 6:
                stride, hw, fmt = p[3][1:4]
                plane, depth = nxt[0][3][0], nxt[0][3][1]
                height = hw >> 16
                if plane != stride * height: raise Violation('S7 plane %x differs from stride %x x height %x' % (plane, stride, height))
                if depth == 0: raise Violation('S7 depth 0')
                extent = plane * depth
        if extent is not None and off + extent > region:
            raise Violation('S8 %s at register %04x: offset %d + extent %d exceeds the region of %d bytes' % (SOURCES[src], reg, off, extent, region))
        tensors.append((src, idx, off, reg, extent, region))
    if program is None: raise Violation('S6 no program patch')
    return {'instructions': count, 'tensors': tensors, 'registers': len(loads), 'uniform_words': sum(len(u[3]) for u in uni)}


def load(path, native):
    return nm.Native(native, Path(path).read_bytes())


def table_span(n, idx, row):
    sp = nm.Span()
    if n.lib.nbg_model_table(c.byref(n.view), idx, row, c.byref(sp)) != 1: raise Violation('table %d row %d' % (idx, row))
    return sp


def check_graph(path, native, show=False):
    n = load(path, native)
    v = n.view
    h = v.header
    ops, pat = n.operations(), n.patches()
    ppu_rows = v.sections[TAB_PPU_INSTR].bytes // 8
    sh = words_of(n.section(SEC_SH)) if v.sections[SEC_SH].bytes else ()
    if v.sections[SEC_PPU_PARAM].bytes: raise Violation('S9 graph has a PPU parameter section (not examined)')
    inputs = [t['logical_bytes'] for t in n.tensors(0, h.input_count)]
    outputs = [t['logical_bytes'] for t in n.tensors(1, h.output_count)]
    coeff = [table_span(n, TAB_COEFF, r).bytes for r in range(v.sections[TAB_COEFF].bytes // 8)]
    dyna = [table_span(n, TAB_DYNA_IO, r).bytes for r in range(v.sections[TAB_DYNA_IO].bytes // 8)]
    ppu_len = v.sections[SEC_PPU_INSTR].bytes
    reports, first = [], True
    for o in ops:
        if o[0] != 1: continue
        kind, index, layer, state, p0, pc = o[:6]
        # S9: the program is the row of the program table that row `index` of the shader table names
        if 2 * index + 1 >= len(sh): raise Violation('S9 shader table row %d' % index)
        row, param = sh[2 * index], sh[2 * index + 1]
        if param or row >= ppu_rows: raise Violation('S9 shader table row %d is (%d, %d)' % (index, row, param))
        span = table_span(n, TAB_PPU_INSTR, row)
        start = span.data - (v.data + v.sections[SEC_PPU_INSTR].offset)
        if start % 256 or start + span.bytes > ppu_len or span.bytes % 16 or not span.bytes:
            raise Violation('S9 program row %d at %d length %d in a section of %d bytes' % (row, start, span.bytes, ppu_len))
        sp = table_span(n, TAB_LOAD_STATES, state)
        w = words_of(c.string_at(sp.data, sp.bytes))
        io = {'inputs': inputs, 'outputs': outputs, 'pool': h.pool_bytes, 'vip_sram': h.vip_sram_bytes, 'coeff': coeff,
              'dyna': dyna, 'ppu_len': span.bytes}
        r = check_stream(w, pat[p0:p0 + pc], io, v)
        r.update(op=o, program_offset=start, program_row=row, first=w[0] == 0x08015580)
        reports.append(r)
        if show:
            print('  shader op %d: %d instructions at program offset %d, %d state bytes, %d uniform words' % (
                index, r['instructions'], start, sp.bytes, r['uniform_words']))
            for src, idx, off, reg, ext, region in r['tensors']:
                print('    %-26s index %2d offset %7d  uniform %04x  extent %s  region %d' % (SOURCES[src], idx, off, reg,
                      '-' if ext is None else ext, region))
    return reports


# A shader operation compiled from an element-wise floor of a 16 x 64 half-precision tensor (single-operation graph,
# target VIP9000NANODI_PLUS_PID0X1000003B): 190 state words, patches, and the region sizes it is checked against. It is
# the compiler output of a model that this repository generates (tools/npu-shader-variants.py, kind elt, op Floor).
FIXTURE = [
    0x08015580, 0x00000002, 0x08010e02, 0x00000701, 0x48000000, 0x00000701,
    0x0804d800, 0x00000000, 0x00000800, 0x00010400, 0x44404071, 0x18000000,
    0x0804d804, 0x00000800, 0x00000001, 0x10dd10b0, 0x00000000, 0x18000000,
    0x0804d808, 0x00000000, 0x00000800, 0x00010400, 0x44404071, 0x18000000,
    0x0804d80c, 0x00000800, 0x00000001, 0x10dd10b0, 0x00000000, 0x18000000,
    0x0804d810, 0x01010101, 0x00000000, 0x00010000, 0x00030002, 0x18000000,
    0x0804d814, 0x02020202, 0x00000000, 0x00000000, 0x00000100, 0x18000000,
    0x0804d818, 0x00003c00, 0x00000000, 0x00003c00, 0x00000000, 0x18000000,
    0x0804d81c, 0x00003c00, 0x00000000, 0x00003c00, 0x00000000, 0x18000000,
    0x0804d820, 0x01010101, 0x00000000, 0x00050004, 0x00070006, 0x18000000,
    0x0804d824, 0x02020202, 0x00000000, 0x00000000, 0x00000100, 0x18000000,
    0x0804d828, 0x00003c00, 0x00000000, 0x00003c00, 0x00000000, 0x18000000,
    0x0804d82c, 0x00003c00, 0x00000000, 0x00003c00, 0x00000000, 0x18000000,
    0x0804d830, 0x3f800000, 0x00000000, 0x3f800000, 0x00000000, 0x18000000,
    0x0804d834, 0x11111111, 0x11110000, 0x06040200, 0x06040200, 0x18000000,
    0x0804d838, 0x22222222, 0x00000000, 0x00000000, 0x00000100, 0x18000000,
    0x0804d83c, 0x00003c00, 0x00003c00, 0x00003c00, 0x00003c00, 0x18000000,
    0x0804d840, 0x00003c00, 0x00003c00, 0x00003c00, 0x00003c00, 0x18000000,
    0x080102a4, 0x00000000, 0x08010420, 0x00000000, 0x08010416, 0x00000000,
    0x08010409, 0x00000000, 0x0801021f, 0x00000000, 0x08010424, 0x0000000d,
    0x0801040a, 0x00000000, 0x08015580, 0x00000002, 0x0801021a, 0x00000001,
    0x08010425, 0x0000000c, 0x08010427, 0x00000000, 0x08030402, 0x00001f01,
    0x00000003, 0x00000f00, 0x08010228, 0x00000000, 0x0801028c, 0x00000100,
    0x08010e07, 0x00000000, 0x0801040c, 0x00000000, 0x0801020c, 0x0f3f0000,
    0x08010201, 0x00000001, 0x080102aa, 0x00000000, 0x08010e22, 0x007f0000,
    0x08010412, 0x00000000, 0x08010e27, 0x00000000, 0x08010240, 0x03000003,
    0x08010249, 0x00000000, 0x0801022e, 0x00000080, 0x08010247, 0x00000001,
    0x0801024b, 0x00000000, 0x0801024d, 0x00000000, 0x0801024f, 0x00000000,
    0x08010256, 0x00000008, 0x08010257, 0x00000001, 0x08010258, 0x00000001,
    0x08060250, 0x0000001f, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
    0x00000000, 0x18000000, 0x08010248, 0xbadabeeb, 0x08010e03, 0x00000024,
    0x08010e02, 0x00000701, 0x48000000, 0x00000701,
]
FIXTURE_PATCHES = [(0, 484, 0, -1, 0, 0), (0, 28, 1, 0, 0, 0), (0, 76, 2, 0, 0, 0)]
FIXTURE_IO = {'inputs': [2048], 'outputs': [2048], 'pool': 0, 'vip_sram': 98304, 'coeff': [], 'dyna': [], 'ppu_len': 208}


def self_test():
    import copy
    def index_of(w, header): return w.index(header)
    cases = []
    def case(name, rule, fn):
        cases.append((name, rule, fn))
    def mutate(edit, strict=False):
        def run():
            w, pt, io = list(FIXTURE), copy.deepcopy(FIXTURE_PATCHES), copy.deepcopy(FIXTURE_IO)
            r = edit(w, pt, io)
            if r is not None: w = r
            return check_stream(w, pt, io, None, strict)
        return run
    def setw(i, v):
        def e(w, pt, io): w[i] = v
        return e
    at = {h: FIXTURE.index(h) for h in (0x0804d800, 0x0804d804, 0x0804d808, 0x0804d80c, 0x08010424, 0x08010425, 0x0801040a, 0x08010248, 0x08010e03, 0x08010257, 0x0804d840)}
    case('control: unmodified stream is accepted', None, mutate(lambda w, pt, io: None))
    case('S1: header with the float bit set', 'S1', mutate(setw(at[0x0804d804], 0x0c04d804)))
    case('S1: header of an unknown opcode', 'S1', mutate(setw(at[0x0804d804], 0x2804d804)))
    case('S1: count runs past the stream end', 'S1', mutate(setw(len(FIXTURE) - 4, 0x08050e02)))
    case('S1: pad word removed', 'S1', mutate(lambda w, pt, io: w[:at[0x0804d800] + 5] + w[at[0x0804d800] + 6:]))
    case('S1: stream of an odd word count', 'S1', mutate(lambda w, pt, io: w + [NOP]))
    case('S2 (observed order): register 0427 removed', 'S2', mutate(lambda w, pt, io: w[:at[0x08010424] + 6] + [0x08010425, 0x0c] + w[at[0x08010425] + 4:], True))
    case('S2a: unknown register 0x0430 inserted', 'S2a', mutate(lambda w, pt, io: w[:at[0x08010248]] + [0x08010430, 0] + w[at[0x08010248]:]))
    case('S2 (observed order): registers 0257 and 0248 swapped', 'S2', mutate(lambda w, pt, io: w[:at[0x08010257]] + [0x08010248, 0xbadabeeb] + w[at[0x08010257] + 2:at[0x08010248]] + [0x08010257, 1] + w[at[0x08010248] + 2:], True))
    case('control: swapped registers are admitted without the observed-order check', None, mutate(lambda w, pt, io: w[:at[0x08010257]] + [0x08010248, 0xbadabeeb] + w[at[0x08010257] + 2:at[0x08010248]] + [0x08010257, 1] + w[at[0x08010248] + 2:]))
    case('S2a: final stall token changed', 'S2a', mutate(setw(len(FIXTURE) - 1, 0x700)))
    case('S3 (observed value): kick value changed', 'S3', mutate(setw(at[0x08010248] + 1, 0xbadabeea), True))
    case('control: changed kick value is admitted without the observed-value check', None, mutate(setw(at[0x08010248] + 1, 0xbadabeea)))
    case('S2a: cache flush value changed', 'S2a', mutate(setw(at[0x08010e03] + 1, 0x7f)))
    case('S2a: register above the uniform bank', 'S2a', mutate(setw(at[0x0804d840], 0x0804da00)))
    case('S4: a uniform word written twice', 'S4', mutate(setw(at[0x0804d840], 0x0804d800)))
    case('S4: uniform packet beyond four words', 'S4', mutate(lambda w, pt, io: w[:at[0x0804d840]] + [0x0805d840] + w[at[0x0804d840] + 1:at[0x0804d840] + 5] + [0] + w[at[0x0804d840] + 6:]))
    case('S5: end counter not count minus one', 'S5', mutate(setw(at[0x08010425] + 1, 0x0d)))
    case('S5: count differs from the program span', 'S5', mutate(lambda w, pt, io: io.update(ppu_len=192)))
    case('S6: patch kind 1', 'S6', mutate(lambda w, pt, io: pt.__setitem__(1, (1, 28, 1, 0, 0, 0))))
    case('S6: patch on a header word', 'S6', mutate(lambda w, pt, io: pt.__setitem__(1, (0, 24, 1, 0, 0, 0))))
    case('S6: two patches on one word', 'S6', mutate(lambda w, pt, io: pt.__setitem__(2, (0, 28, 2, 0, 0, 0))))
    case('S6: source 4 (AXI SRAM)', 'S6', mutate(lambda w, pt, io: pt.__setitem__(1, (0, 28, 4, -1, 0, 0))))
    case('S6: program patch with an offset', 'S6', mutate(lambda w, pt, io: pt.__setitem__(0, (0, 484, 0, -1, 256, 0))))
    case('S6: program patch missing', 'S6', mutate(lambda w, pt, io: (pt.pop(0), None)[1]))
    case('S6: second program patch', 'S6', mutate(lambda w, pt, io: pt.__setitem__(2, (0, 76, 0, -1, 0, 0))))
    case('S6: patch of a configuration register', 'S6', mutate(lambda w, pt, io: pt.__setitem__(2, (0, at[0x08010257] * 4 + 4, 2, 0, 0, 0))))
    case('S6: input index out of range', 'S6', mutate(lambda w, pt, io: pt.__setitem__(1, (0, 28, 1, 1, 0, 0))))
    case('S6: transformed field 1', 'S6', mutate(lambda w, pt, io: pt.__setitem__(1, (0, 28, 1, 0, 0, 1))))
    case('S7: plane differs from stride times height', 'S7', mutate(setw(at[0x0804d804] + 1, 0x7c0)))
    case('S7: depth zero', 'S7', mutate(setw(at[0x0804d804] + 2, 0)))
    case('S8: extent beyond the input tensor', 'S8', mutate(lambda w, pt, io: io.update(inputs=[1024])))
    case('S8: offset plus extent beyond the output tensor', 'S8', mutate(lambda w, pt, io: pt.__setitem__(2, (0, 76, 2, 0, 64, 0))))
    bad = 0
    for name, rule, fn in cases:
        try:
            r = fn(); got = None
        except Violation as e:
            got = str(e).split()[0]
        ok = got == rule
        bad += not ok
        print('%-4s %s%s' % ('ok' if ok else 'FAIL', name, '' if ok else '   (expected %s, got %s)' % (rule, got)))
    print('%d cases, %d failed' % (len(cases), bad))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, help='new directory for the decoder library')
    ap.add_argument('--native', type=Path, help='an existing decoder library')
    ap.add_argument('--show', action='store_true')
    ap.add_argument('--check', action='store_true', help='exit 1 when a shader operation breaks a rule')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('graphs', nargs='*', type=Path)
    a = ap.parse_args()
    if a.self_test and not a.graphs:
        return 1 if self_test() else 0
    native = a.native
    if not native:
        if not a.work: ap.error('--work or --native is required')
        a.work.mkdir(parents=True, exist_ok=False)
        native = a.work / 'nbg.so'
        subprocess.run(['/usr/bin/clang-21', '-shared', '-fPIC', '-O1', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'drivers/npu'),
                        '-o', str(native), *(str(ROOT / 'drivers/npu' / f) for f in ('nbg_model.c', 'nbg_checksum.c', 'nbg_relocation.c'))], check=True)
    failed = 0
    for g in a.graphs:
        try:
            reports = check_graph(g, native, a.show)
            print('%s: %d shader operations, all rules hold' % (g, len(reports)) if reports else '%s: no shader operation' % g)
        except Violation as e:
            failed += 1
            print('%s: VIOLATION %s' % (g, e))
    if a.self_test: failed += self_test()
    return 1 if failed and a.check else 0


if __name__ == '__main__':
    sys.exit(main())
