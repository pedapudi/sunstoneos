#!/usr/bin/env python3
"""Mutation experiments that confirm the fields of shader operations in compiled network binaries.

Each experiment changes one field of a compiled single-operation graph (tools/npu-nbg-mutate.py repairs the
checksums), runs the changed binary in the SDK simulator and compares the output with a prediction computed with NumPy
from the graph input and the unchanged output. A field is confirmed when the output moves as predicted. The graphs are
compiled and simulated first with tools/npu-graph-compile.sh and tools/npu-graph-simulate.sh from the specifications that
`--specs` prints (tools/npu-shader-variants.py writes the model directories). Run offline.

  npu-shader-probe.py --specs                                  print the model specifications as JSON
  npu-shader-probe.py --work WORK --native LIB [--image IMG] [--only NAME ...]

WORK holds frags/GRAPH/export_nbg_unify (the compile and simulate results) and receives the mutants under WORK/m.
LIB is the decoder library that tools/npu-nbg-census.py or tools/npu-shader-state.py builds. IMG is the compiler image
(default ubuntu-npu:v2.0.10.2); simulator runs use --network none and --memory 6g (measured peak 4.3 GB), one container per mutant, three at a time.
"""
import argparse, concurrent.futures as cf, importlib.util, json, struct, subprocess, sys
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
nm = _load('nbg_model', ROOT / 'checks/nbg-model.py')
import ctypes as c

SPECS = [
    {"kind": "elt", "name": "fl_16_64", "op": "Floor"},
    {"kind": "gather", "name": "gather0"},
    {"kind": "elt", "name": "e_recip", "op": "Reciprocal"},
    {"kind": "embed", "name": "emb_s"},
    {"kind": "seq", "name": "s_sin_exp_floor_exp", "ops": ["Sin", "Exp", "Floor", "Exp"]},
    {"kind": "seq", "name": "s_exp_sin", "ops": ["Exp", "Sin"]},
    {"kind": "elt", "name": "ex_16_64", "op": "Exp"},
    {"kind": "resize", "name": "rn_a"},
    {"kind": "topk", "name": "topk"},
]
SIM_ENV = 'export VSIMULATOR_SHADER_CORE_COUNT=1 VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B LD_LIBRARY_PATH=/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools/vsimulator/lib:/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools/common/lib'
f16 = lambda v: np.asarray(v, dtype=np.float32).astype(np.float16).astype(np.float32)


class Lab:
    def __init__(self, work, native, image):
        self.work, self.native, self.image = work.resolve(), native.resolve(), image
        (self.work / 'm').mkdir(exist_ok=True); self.count = 0

    def graph(self, g):
        n = nm.Native(self.native, (self.work / 'frags' / g / 'export_nbg_unify/network_binary.nb').read_bytes())
        return Graph(self, g, n)

    def simulate(self, g, files):
        """Run each mutant file in its own container; returns {name: (output array or None, log)}."""
        d = self.work / 'frags' / g / 'export_nbg_unify'
        inputs = ' '.join('/work/frags/%s/export_nbg_unify/%s' % (g, p.name) for p in sorted(d.glob('sim-input*.tensor')))
        def one(item):
            name, path = item
            out = path.parent / ('out-' + path.stem)
            out.mkdir(exist_ok=True)
            script = ('%s; mkdir -p /work/m/%s/out-%s; cd /work/m/%s/out-%s; timeout 60 /work/frags/%s/export_nbg_unify/f %s %s > sim.log 2>&1; '
                      'chmod -R a+rwX /work/m' % (SIM_ENV, g, path.stem, g, path.stem, g, '/work/m/%s/%s' % (g, path.name), inputs))
            subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--memory', '6g', '--memory-swap', '6g', '-v', '%s:/work' % self.work,
                            self.image, 'bash', '-c', script], capture_output=True)
            found = sorted(out.glob('output*.dat'))
            log = (out / 'sim.log').read_text(errors='replace') if (out / 'sim.log').exists() else ''
            return name, (np.fromfile(found[0], dtype=np.float16) if found else None, log)
        with cf.ThreadPoolExecutor(3) as ex:
            return dict(ex.map(one, files.items()))


class Graph:
    def __init__(self, lab, g, n):
        self.lab, self.g, self.n = lab, g, n
        self.ops, self.patches = n.operations(), n.patches()
        self.sh = [o for o in self.ops if o[0] == 1]

    def input(self, i=0):
        return np.loadtxt(self.lab.work / 'frags' / self.g / ('export_nbg_unify/sim-input%d.tensor' % i)).astype(np.float32)

    def table(self, idx, row):
        sp = nm.Span()
        assert self.n.lib.nbg_model_table(c.byref(self.n.view), idx, row, c.byref(sp)) == 1
        return c.string_at(sp.data, sp.bytes)

    def regword(self, o, reg):
        w = struct.unpack('<%dI' % (len(self.table(26, o[3])) // 4), self.table(26, o[3])); i = 0
        while i < len(w):
            h = w[i]
            if h >> 27 != 1: i += 2 if h == 0x48000000 else 1; continue
            cnt = (h >> 16) & 0x7ff
            if (h & 0xffff) <= reg < (h & 0xffff) + cnt: return i + 1 + reg - (h & 0xffff)
            i += 1 + cnt + ((1 + cnt) & 1)

    # mutation arguments for tools/npu-nbg-mutate.py
    @staticmethod
    def u32(v): return struct.pack('<I', v).hex()
    def sec(self, s, off, v): return ['--bytes', str(s), str(off), self.u32(v)]
    def secb(self, s, off, b): return ['--bytes', str(s), str(off), b.hex()]
    def patch(self, i, field, v): return self.sec(9, i * 28 + field * 4, v)          # patch table entry, field 4 = source offset
    def reg(self, o, r, v): return ['--word', str(o[3]), str(self.regword(o, r)), str(v)]
    def opfield(self, i, field, v): return self.sec(3, i * 32 + field * 4, v)       # operation table entry

    def run(self, mutants):
        d = self.lab.work / 'm' / self.g; d.mkdir(parents=True, exist_ok=True)
        files = {}
        src = self.lab.work / 'frags' / self.g / 'export_nbg_unify/network_binary.nb'
        (d / 'base.nb').write_bytes(src.read_bytes()); files['base'] = d / 'base.nb'
        for name, args in mutants.items():
            out = d / (name + '.nb')
            r = subprocess.run([sys.executable, str(ROOT / 'tools/npu-nbg-mutate.py'), str(src), str(out), '--native', str(self.lab.native)] + args,
                               capture_output=True, text=True)
            if r.returncode: raise SystemExit('mutation %s failed: %s%s' % (name, r.stdout, r.stderr))
            files[name] = out
        subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--memory', '1g', '-v', '%s:/work' % self.lab.work, self.lab.image, 'chmod', '-R', 'a+rwX', '/work/m'], capture_output=True)
        return self.lab.simulate(self.g, files)


RESULTS = []
def verdict(name, ok, detail):
    RESULTS.append(ok)
    print('%-5s %-46s %s' % ('ok' if ok else 'FAIL', name, detail))


def close(a, b, tol=0.0): return a is not None and np.abs(a - b).max() <= tol


def exp_elementwise_addresses(lab):
    """Source 1 and 2 (graph input and output): moving the offset by two rows moves the read or the write."""
    g = lab.graph('fl_16_64'); o = g.sh[0]; p = o[4]
    x = g.input(); fl = f16(np.floor(x))
    r = g.run({'in+64': g.patch(p + 1, 4, 64), 'out+64': g.patch(p + 2, 4, 64)})
    base = r['base'][0]
    verdict('floor base equals the model', close(base, fl), 'max|diff| %.3g' % np.abs(base - fl).max())
    pred = np.zeros_like(fl); pred[:-32] = fl[32:]
    verdict('source 1 offset +64: reads 32 elements later', close(r['in+64'][0], pred), 'tail reads zeros')
    pred = np.zeros_like(fl); pred[32:] = fl[:-32]
    verdict('source 2 offset +64: writes 32 elements later', close(r['out+64'][0], pred), 'head stays zero')
    # dispatch geometry and program length
    R = lambda reg, v: g.reg(o, reg, v)
    r = g.run({'gx_half': R(0x250, 0xf), 'count-1': R(0x424, 0xc), 'endpc-1': R(0x425, 0xb), 'count+1': R(0x424, 0xe), 'endpc+1': R(0x425, 0xd),
               'lanes0': R(0x253, 0)})
    b = r['base'][0]; ch = lambda k: np.nonzero(r[k][0] != b)[0]
    verdict('0250 = groups of 32 elements minus one', len(ch('gx_half')) and ch('gx_half')[0] == 512 and np.all(r['gx_half'][0][:512] == b[:512]), 'second half not written')
    verdict('0253 = elements per thread minus one', len(ch('lanes0')) and ch('lanes0')[0] == 256, 'work shrinks to a quarter')
    verdict('0424 shorter by one instruction breaks the program', len(ch('count-1')) > 0, '%d outputs change' % len(ch('count-1')))
    verdict('0425 shorter by one instruction breaks the program', len(ch('endpc-1')) > 0, '%d outputs change' % len(ch('endpc-1')))
    verdict('0424 and 0425 longer by one change nothing', not len(ch('count+1')) and not len(ch('endpc+1')), 'zero padding after the program is harmless')


def exp_gather(lab):
    """Source 5 (constant data): the index list of a gather; sources 1 and 2."""
    g = lab.graph('gather0'); o = g.sh[0]; p = o[4]
    x = g.input().reshape(32, 64)
    gat = lambda ix: f16(np.stack([x[i] for i in ix]))
    r = g.run({'idx_off4': g.patch(p + 2, 4, 4), 'idx_off8': g.patch(p + 2, 4, 8), 'in_off128': g.patch(p + 1, 4, 128),
               'out_off128': g.patch(p + 3, 4, 128), 'idx0=9': g.sec(23, 0, 9)})
    base = r['base'][0].reshape(4, 64)
    verdict('gather base equals x[3,1,7,5]', close(base, gat([3, 1, 7, 5]), 0.002), 'within the half-precision input rounding')
    verdict('source 5 offset +4 reads the next index', close(r['idx_off4'][0].reshape(4, 64), gat([1, 7, 5, 0]), 0.002), 'rows 1,7,5,0')
    verdict('source 5 offset +8 reads two indices on', close(r['idx_off8'][0].reshape(4, 64), gat([7, 5, 0, 0]), 0.002), 'rows 7,5,0,0')
    verdict('constant data word 0 edited to 9', close(r['idx0=9'][0].reshape(4, 64), gat([9, 1, 7, 5]), 0.002), 'row 0 becomes x[9]')
    verdict('source 1 offset +128 reads one row on', close(r['in_off128'][0].reshape(4, 64), gat([4, 2, 8, 6]), 0.002), 'rows 4,2,8,6')
    o128 = r['out_off128'][0].reshape(4, 64)
    verdict('source 2 offset +128 writes one row on', not o128[0].any() and close(o128[1:], base[:3]), 'row 0 stays zero')


def exp_embedding(lab):
    """An embedding lookup: the table is constant data (source 5), the indices a graph input (source 1)."""
    g = lab.graph('emb_s'); o = g.sh[0]; p = o[4]
    tab = np.frombuffer(g.n.section(23), dtype=np.float16).astype(np.float32).reshape(64, 32)
    ix = g.input().astype(int)
    r = g.run({'tab+1row': g.patch(p + 1, 4, 64), 'idx+1': g.patch(p + 2, 4, 4), 'out+1row': g.patch(p + 3, 4, 64),
               'row3=7': g.secb(23, 3 * 64, np.full(32, 7.0, np.float16).tobytes())})
    verdict('embedding base equals table[idx]', close(r['base'][0].reshape(8, 32), tab[ix]), 'exact')
    verdict('source 5 offset +1 row reads table[idx+1]', close(r['tab+1row'][0].reshape(8, 32), tab[(ix + 1) % 64]), 'exact')
    verdict('source 1 offset +4 reads the next index', close(r['idx+1'][0].reshape(8, 32)[:7], tab[ix[1:]]), 'rows 0..6')
    o_ = r['out+1row'][0].reshape(8, 32)
    verdict('source 2 offset +1 row writes one row on', not o_[0].any() and close(o_[1:], r['base'][0].reshape(8, 32)[:-1]), 'exact')
    o_ = r['row3=7'][0].reshape(8, 32)
    verdict('table row 3 edited in the constant data', np.all(o_[0] == 7.0) and close(o_[1:], r['base'][0].reshape(8, 32)[1:]), 'only the output row of index 3 changes')


def exp_reciprocal(lab):
    """Sources 5 and 6 (constant data and dynamic input table): two scale constants the reciprocal reads."""
    g = lab.graph('e_recip'); o = g.sh[0]; p = o[4]
    r = g.run({'coef=3': g.secb(23, 0, struct.pack('<H', 0x4200)), 'dyna=3': g.secb(19, 0, struct.pack('<f', 3.0)),
               'coef_off2': g.secb(23, 2, struct.pack('<H', 0x4000)) + g.patch(p + 1, 4, 2),
               'dyna_off4': g.secb(19, 4, struct.pack('<f', 2.0)) + g.patch(p + 4, 4, 4)})
    base = r['base'][0]
    ratio = lambda k: float(np.nanmedian(r[k][0] / base))
    verdict('source 5 word edited to 3.0 scales the output by 3', abs(ratio('coef=3') - 3) < 0.01, 'median ratio %.3f' % ratio('coef=3'))
    verdict('source 6 word edited to 3.0 scales the output by 3', abs(ratio('dyna=3') - 3) < 0.01, 'median ratio %.3f' % ratio('dyna=3'))
    verdict('source 5 offset +2 reads the marked value 2.0', abs(ratio('coef_off2') - 2) < 0.01, 'median ratio %.3f' % ratio('coef_off2'))
    verdict('source 6 offset +4 reads the marked value 2.0', abs(ratio('dyna_off4') - 2) < 0.01, 'median ratio %.3f' % ratio('dyna_off4'))


def exp_vip_sram(lab):
    """Source 8 (VIP SRAM) between four chained shaders; each offset moved by 64 bytes."""
    g = lab.graph('s_sin_exp_floor_exp'); P = [o[4] for o in g.sh]
    x = g.input(); s0 = f16(np.sin(x)); e1 = f16(np.exp(s0)); fl = f16(np.floor(e1)); out = f16(np.exp(fl))
    A, B = 98560, 100608
    shl = lambda a: np.concatenate([a[32:], np.zeros(32, a.dtype)])
    shr = lambda a: np.concatenate([np.zeros(32, a.dtype), a[:-32]])
    r = g.run({'sh1_in': g.patch(P[1] + 1, 4, A + 64), 'sh1_out': g.patch(P[1] + 2, 4, B + 64), 'sh2_in': g.patch(P[2] + 1, 4, B + 64),
               'sh2_out': g.patch(P[2] + 2, 4, A + 64), 'sh3_in': g.patch(P[3] + 1, 4, A + 64), 'sh0_out': g.patch(P[0] + 2, 4, A + 64)})
    E = lambda a: f16(np.exp(a)); F = lambda a: f16(np.floor(a))
    verdict('chain base equals exp(floor(exp(sin x)))', close(r['base'][0], out), 'exact')
    verdict('shader 0 output (source 8) +64', close(r['sh0_out'][0], E(F(E(shr(s0))))), 'exact')
    verdict('shader 1 input (source 8) +64', close(r['sh1_in'][0][:-32], E(F(E(shl(s0))))[:-32]), 'exact except the last 32')
    verdict('shader 1 output (source 8) +64', close(r['sh1_out'][0], E(F(shr(e1)))), 'exact')
    verdict('shader 2 input (source 8) +64', close(r['sh2_in'][0][:-32], E(F(shl(e1)))[:-32]), 'exact except the last 32')
    pred = np.concatenate([s0[:32], fl[:-32]])        # the first 32 elements of the region keep shader 0's output
    verdict('shader 2 output (source 8) +64', close(r['sh2_out'][0], E(pred), 0.001), 'first 32 elements keep the earlier value (one half-precision step of rounding)')
    verdict('shader 3 input (source 8) +64', close(r['sh3_in'][0][:-32], E(shl(fl))[:-32]), 'exact except the last 32')


def exp_program(lab):
    """Which program runs: shader table row, program table row, and the unused source-0 offset."""
    g = lab.graph('s_sin_exp_floor_exp'); ops = g.ops
    x = g.input(); s0 = f16(np.sin(x)); e1 = f16(np.exp(s0)); fl = f16(np.floor(e1)); out = f16(np.exp(fl))
    sh = [o for o in ops if o[0] == 1]
    ppu = g.n.section(18)
    rows = np.frombuffer(g.n.section(24), dtype='<u4').reshape(-1, 2)           # (offset, length) per program row
    same = [(a, b) for a in range(len(rows)) for b in range(a + 1, len(rows)) if rows[a][1] == rows[b][1] and ppu[rows[a][0]:rows[a][0] + rows[a][1]] == ppu[rows[b][0]:rows[b][0] + rows[b][1]]]
    a, b = same[0]                                                              # two rows that hold the same program
    opa = [o for o in sh if o[1] == a][0]; pa = opa[4]
    r = g.run({'row_a_moved_to_b': g.sec(24, a * 8, int(rows[b][0])),
               'src0_offset_to_b': g.patch(pa, 4, int(rows[b][0])),
               'src0_offset_huge': g.patch(pa, 4, 0x10000),
               'row_a_to_zero_stream_b': g.sec(24, a * 8, 0) + g.reg(opa, 0x40a, int(rows[b][0])),
               'swap_programs': g.sec(24, 0, int(rows[1][0])) + g.sec(24, 4, int(rows[1][1])) + g.sec(24, 8, int(rows[0][0])) + g.sec(24, 12, int(rows[0][1]))})
    base = r['base'][0]
    verdict('program table row moved to an identical program', close(r['row_a_moved_to_b'][0], base), 'same output')
    verdict('source-0 offset to the identical program changes nothing', close(r['src0_offset_to_b'][0], base), 'offset is not used')
    verdict('source-0 offset 0x10000 changes nothing', close(r['src0_offset_huge'][0], base), 'offset is not used')
    verdict('the stream operand of 040a is not used', not close(r['row_a_to_zero_stream_b'][0], base), 'the table row alone decides (program 0 runs)')
    verdict('two program rows swapped breaks both shaders', not close(r['swap_programs'][0], base), 'program is looked up through the row')


def exp_mmu(lab):
    """Addresses far outside every region raise a memory-management exception: the field is an address the engine uses."""
    for gname, items in (('fl_16_64', {'src1': (1, 0), 'src2': (2, 0)}), ('gather0', {'src5': (2, 0)}), ('e_recip', {'src6': (4, 0)}), ('topk', {'src3': (3, 0), 'src6': (4, 0)})):
        g = lab.graph(gname); o = g.sh[0]
        r = g.run({k: g.patch(o[4] + i, 4, 0x8000000) for k, (i, _) in items.items()})
        for k in items:
            verdict('%s: source offset 128 MiB raises an MMU exception' % ('%s %s' % (gname, k)), 'MMU exception' in r[k][1] and r[k][0] is None, 'no output written')


def exp_descriptor(lab):
    """The tensor descriptor words of a nearest-neighbour resize of [1,4,8,8] to [1,4,16,16] (uniform 0xd800 + k):
    word 1 row stride, word 2 height << 16 | width, word 4 channel stride, word 5 depth."""
    g = lab.graph('rn_a'); o = g.sh[0]
    flat = g.input()                                    # 4 channels of 8 x 8 half-precision elements, 2 bytes each
    def up(a): return f16(np.repeat(np.repeat(a, 2, axis=1), 2, axis=2))
    def at(c_stride, r_stride, h, w=8):                 # input as the shader sees it for the given strides (elements);
        a = np.zeros((4, 8, 8), np.float32)             # coordinates past the height or width bound clamp to the last row or column
        for c in range(4):
            for y in range(8):
                for x in range(8):
                    k = c * c_stride + min(y, h - 1) * r_stride + min(x, w - 1)
                    a[c, y, x] = flat[k] if k < len(flat) else 0
        return a
    U = lambda k, v: g.reg(o, 0xd800 + k, v)
    r = g.run({'depth_word': U(5, 2), 'plane_half': U(4, 0x40), 'height_4': U(2, 0x00040008), 'row_stride_x2': U(1, 0x20), 'width_4': U(2, 0x00080004), 'out_plane_half': U(0xc, 0x100)})
    base = r['base'][0].reshape(4, 16, 16)
    verdict('resize base equals the model', close(base, up(at(64, 8, 8)), 0.002), 'within the half-precision input rounding')
    verdict('depth word: no effect (the dispatch fixes the channel count)', close(r['depth_word'][0].reshape(4, 16, 16), base), 'unchanged')
    verdict('word 4 is the channel stride in bytes', close(r['plane_half'][0].reshape(4, 16, 16), up(at(32, 8, 8)), 0.002), 'channel c reads from byte 64 c')
    verdict('word 2 high half is the height bound (clamped)', close(r['height_4'][0].reshape(4, 16, 16), up(at(64, 8, 4)), 0.002), 'rows 4..7 read as row 3 (clamped)')
    verdict('word 2 low half is the width bound (clamped)', close(r['width_4'][0].reshape(4, 16, 16), up(at(64, 8, 8, 4)), 0.002), 'columns 4..7 read as column 3 (clamped)')
    verdict('word 1 is the row stride in bytes', close(r['row_stride_x2'][0].reshape(4, 16, 16), up(at(64, 16, 8)), 0.002), 'row y reads from byte 32 y')
    o_ = r['out_plane_half'][0].reshape(4, 16, 16)
    verdict('output word 4 is the output channel stride', bool((o_ != base).any()), 'channels overlap in memory and the result changes')


def exp_registers(lab):
    """Each configuration operand word of a floor shader changed in turn (to zero and with its low bit flipped); reports
    whether the output of the simulator changes, and checks that the pad words are not read."""
    g = lab.graph('fl_16_64'); o = g.sh[0]
    w = struct.unpack('<%dI' % (len(g.table(26, o[3])) // 4), g.table(26, o[3]))
    words, pad_words, i = [], [], 0
    while i < len(w):
        h = w[i]
        if h >> 27 != 1: i += 2 if h == 0x48000000 else 1; continue
        cnt = (h >> 16) & 0x7ff
        if (h & 0xffff) < 0xd800:
            words += [((h & 0xffff) + k, i + 1 + k, w[i + 1 + k]) for k in range(cnt)]
        if (1 + cnt) & 1: pad_words.append(i + 1 + cnt)
        i += 1 + cnt + ((1 + cnt) & 1)
    mut = {}
    for reg, at, v in words:
        mut['%04x@%d=0' % (reg, at)] = ['--word', str(o[3]), str(at), '0']
        mut['%04x@%d^1' % (reg, at)] = ['--word', str(o[3]), str(at), str(v ^ 1)]
    mut['pads=deadbeef'] = sum([['--word', str(o[3]), str(j), '0xdeadbeef'] for j in pad_words], [])
    r = g.run(mut)
    base = r['base'][0]
    effect = {}
    for reg, at, v in words:
        res = []
        for tag in ('=0', '^1'):
            out = r['%04x@%d%s' % (reg, at, tag)][0]
            res.append('fails' if out is None else ('changes' if not np.array_equal(out, base) else 'none'))
        effect[(reg, at)] = res
        print('      %04x (value %08x): zero -> %-8s low bit flipped -> %s' % (reg, v, res[0], res[1]))
    verdict('pad words overwritten with 0xdeadbeef change nothing', np.array_equal(r['pads=deadbeef'][0], base), '%d pad words' % len(pad_words))


EXPERIMENTS = {'elementwise': exp_elementwise_addresses, 'gather': exp_gather, 'embedding': exp_embedding, 'reciprocal': exp_reciprocal,
               'vip-sram': exp_vip_sram, 'registers': exp_registers, 'program': exp_program, 'mmu': exp_mmu, 'descriptor': exp_descriptor}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--specs', action='store_true'); ap.add_argument('--work', type=Path); ap.add_argument('--native', type=Path)
    ap.add_argument('--image', default='ubuntu-npu:v2.0.10.2'); ap.add_argument('--only', nargs='*', choices=sorted(EXPERIMENTS))
    a = ap.parse_args()
    if a.specs:
        print(json.dumps(SPECS)); return 0
    if not (a.work and a.native): ap.error('--work and --native are required')
    lab = Lab(a.work, a.native, a.image)
    for name in (a.only or EXPERIMENTS):
        print('==', name)
        EXPERIMENTS[name](lab)
    print('%d checks, %d failed' % (len(RESULTS), RESULTS.count(False)))
    return 1 if False in RESULTS else 0


if __name__ == '__main__':
    sys.exit(main())
