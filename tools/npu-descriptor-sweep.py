#!/usr/bin/env python3
"""Decode the activation descriptor of compiled relu graphs and test the decode by compilation and by mutation.

The 192-byte descriptor of an activation operation (kind 7) follows the field layout of Mesa's struct etna_nn_params
(src/gallium/drivers/etnaviv/etnaviv_ml_nn.c, pinned tree). Two tests check that decode
(docs/hardware/npu-command-formats.md, "Descriptor"):

  sweep:   compile a Relu of one shape per variant, changing the width, the height or the planes, with the pinned
           compiler, and require that each of the 48 descriptor words equals the decode's prediction from the shape.
  mutate:  change descriptor fields of simulated 16-bit graphs (tools/npu-nbg-mutate.py), run each copy in the SDK
           simulator and require the output that the field's meaning predicts.

  npu-descriptor-sweep.py sweep  --work DIR [--widths ...]
  npu-descriptor-sweep.py mutate --work DIR
  npu-descriptor-sweep.py generate OUTDIR ITEM_JSON      (inside the compiler image: writes fragment.onnx and x.npy)

Both tests run the pinned compiler image offline (tools/npu-compiler.lock.json) with --network none and a memory limit,
and write only under --work. The 8-bit variants (int8, uint8) compile to a different operation kind and are listed,
not predicted.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
SDK = '/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools'

# (word, name, lowest bit, bits), from Mesa's struct etna_nn_params
FIELDS = (
    (1, 'in_image_data_type', 2, 2), (1, 'out_image_data_type', 4, 2), (1, 'in_image_x_size', 6, 13), (1, 'in_image_y_size', 19, 13),
    (3, 'out_image_x_size', 6, 13), (3, 'out_image_y_size', 19, 13),
    (4, 'out_image_z_size', 0, 14), (4, 'out_image_tile_x_size', 18, 7), (4, 'out_image_tile_y_size', 25, 7),
    (8, 'out_image_plane_stride_low', 16, 16), (15, 'in_image_data_type_bit_2', 20, 1), (15, 'out_image_data_type_bit_2', 21, 1),
    (16, 'in_image_x_stride', 0, 16), (16, 'in_image_plane_stride_low', 16, 16), (17, 'out_image_x_stride', 0, 16),
    (26, 'in_image_plane_stride_high', 0, 16), (26, 'out_image_plane_stride_high', 16, 16),
)


def generate(out, item):
    """Write one Relu graph of item['shape'] (inside the compiler image: Python 3.8 with NumPy and ONNX)."""
    import numpy as np
    import onnx
    from onnx import helper, TensorProto
    d = Path(out) / item['name']
    d.mkdir(parents=True, exist_ok=True)
    shape = item['shape']
    value = lambda n: helper.make_tensor_value_info(n, TensorProto.FLOAT, shape)
    graph = helper.make_graph([helper.make_node('Relu', ['x'], ['y'])], item['name'], [value('x')], [value('y')])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 17)])
    model.ir_version = 8
    onnx.save(model, str(d / 'fragment.onnx'))
    np.save(str(d / 'x.npy'), np.random.default_rng(7).standard_normal(shape).astype(np.float32))
    (d / 'inputs.json').write_text(json.dumps(['x']))


def image():
    lock = json.loads((ROOT / 'tools/npu-compiler.lock.json').read_text())
    name = lock['loaded_image_id']
    if subprocess.run(['docker', 'image', 'inspect', name], capture_output=True).returncode:
        sys.exit('the compiler image %s is not loaded (tools/npu-compiler.lock.json)' % name)
    return name


def container(work, inside, memory='12g'):
    uid, gid = os.getuid(), os.getgid()
    command = ['docker', 'run', '--rm', '--network', 'none', '--memory', memory, '--memory-swap', memory, '--cpus', '3',
               '--pids-limit', '512', '--cap-drop', 'ALL', '--cap-add', 'CHOWN', '-v', '%s:/work' % work,
               '-v', '%s:/tools:ro' % (ROOT / 'tools'), image(), 'bash', '-c',
               'trap "chown -R %d:%d /work" EXIT; chown -R 0:0 /work; cd /work && %s' % (uid, gid, inside)]
    return subprocess.run(command, capture_output=True, text=True)


def compile_variant(work, name, shape, qtype='int16', quantizer='dynamic_fixed_point', simulate=False):
    """Compile (and optionally simulate) one Relu variant; returns its graph directory."""
    top = work / name
    graph = top / name
    if (graph / 'export_nbg_unify/network_binary.nb').is_file() and (not simulate or list((graph / 'export_nbg_unify').glob('output0_*.dat'))):
        return graph
    top.mkdir(parents=True, exist_ok=True)
    script = (ROOT / 'tools/npu-graph-compile.sh').read_text()
    pinned = '--quantizer dynamic_fixed_point --qtype int16'
    if script.count(pinned) != 1:
        sys.exit('tools/npu-graph-compile.sh no longer names the 16-bit quantizer once')
    (top / 'compile.sh').write_text(script.replace(pinned, '--quantizer %s --qtype %s' % (quantizer, qtype)))
    item = json.dumps({'name': name, 'shape': shape})
    inside = "python3 /tools/npu-descriptor-sweep.py generate /work '%s' && bash /work/compile.sh /work/%s" % (item, name)
    if simulate:
        inside += ' && bash /tools/npu-graph-simulate.sh /work/%s' % name
    done = container(top, inside)
    (top / 'container.log').write_text(done.stdout + done.stderr)
    if done.returncode or not (graph / 'export_nbg_unify/network_binary.nb').is_file():
        sys.exit('%s: compile failed; log %s' % (name, top / 'container.log'))
    return graph


def native(work):
    lib = work / 'nbg.so'
    if not lib.is_file():
        subprocess.run(['/usr/bin/clang-21', '-shared', '-fPIC', '-O1', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'drivers/npu'),
                        '-o', str(lib)] + [str(ROOT / 'drivers/npu' / f) for f in ('nbg_model.c', 'nbg_checksum.c', 'nbg_relocation.c')], check=True)
    return lib


def nbg_model():
    import importlib.util
    spec = importlib.util.spec_from_file_location('nbg_model', ROOT / 'checks/nbg-model.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def descriptor(lib, path):
    """The operation kinds and the 48 descriptor words of each non-INIT operation of a graph."""
    nm = nbg_model()
    data = Path(path).read_bytes()
    n = nm.Native(lib, data)
    section = n.view.sections[6]
    ops = [o for o in n.operations() if o[0] != 0xffff]
    return [o[0] for o in ops], [struct.unpack_from('<48I', data, section.offset + o[1] * 192) for o in ops]


def fields(words):
    return {name: (words[w] >> low) & ((1 << bits) - 1) for w, name, low, bits in FIELDS}


def predict(base, width, height, planes):
    """The descriptor of a 16-bit Relu of planes x height x width from the base graph's words, by the decode."""
    w = list(base)
    plane = width * 2 * height
    tile_x = min(width, 127)
    tile_y = min(height, 320 // tile_x)
    w[1] = (height << 19) | (width << 6) | (base[1] & 0x3f)
    w[3] = (height << 19) | (width << 6) | (base[3] & 0x3f)
    w[4] = (tile_y << 25) | (tile_x << 18) | (base[4] & 0x3c000) | planes
    w[8] = ((plane & 0xffff) << 16) | (base[8] & 0xffff)
    w[16] = ((plane & 0xffff) << 16) | (width * 2)
    w[17] = (base[17] & 0xffff0000) | (width * 2)
    w[26] = ((plane >> 16) << 16) | (plane >> 16)
    # Word 19 (output ring end) is unpatched here: the sentinel 0x03ffffff, with bits 26-27 set only when the output's
    # bytes are a multiple of 64 (inferred from this sweep; no mutation tests it).
    w[19] = (base[19] & ~0x0fffffff) | 0x03ffffff | (0x0c000000 if plane * planes % 64 == 0 else 0)
    return w


def sweep(a):
    work = a.work
    work.mkdir(parents=True, exist_ok=True)
    lib = native(work)
    variants = [('w%d' % w, [1024, w], w, 1024, 1) for w in a.widths]
    variants += [('h%dx176' % h, [h, 176], 176, h, 1) for h in (1, 2, 3, 5, 16, 100, 512, 2048)]
    variants += [('h%dx64' % h, [h, 64], 64, h, 1) for h in (3, 4, 7)]
    variants += [('c%dx64x176' % c, [c, 64, 176], 176, 64, c) for c in (1, 2, 3, 8)]
    variants += [('n1c2x64x176', [1, 2, 64, 176], 176, 64, 2)]
    base = descriptor(lib, compile_variant(work, 'h16x176', [16, 176]) / 'export_nbg_unify/network_binary.nb')[1][0]
    failures = 0
    print('%-14s %5s %5s %3s %5s %5s %6s %6s %6s %6s  %s' % ('variant', 'x', 'y', 'z', 'tileX', 'tileY', 'xstr', 'plane', 'w26', 'kind', 'words differing from the prediction'))
    for name, shape, width, height, planes in variants:
        kinds, ops = descriptor(lib, compile_variant(work, name, shape) / 'export_nbg_unify/network_binary.nb')
        words = ops[0]
        wanted = predict(base, width, height, planes)
        wrong = [i for i in range(48) if words[i] != wanted[i]]
        if len(ops) != 1 or kinds != [7] or wrong:
            failures += 1
        f = fields(words)
        print('%-14s %5d %5d %3d %5d %5d %6d %6d %#6x %6s  %s' % (name, f['in_image_x_size'], f['in_image_y_size'], f['out_image_z_size'],
              f['out_image_tile_x_size'], f['out_image_tile_y_size'], f['in_image_x_stride'],
              f['in_image_plane_stride_low'] | (f['in_image_plane_stride_high'] << 16), words[26], kinds,
              ', '.join('%d: %#x not %#x' % (i, words[i], wanted[i]) for i in wrong) or 'none'))
    for name, qtype, quantizer in (('int8', 'int8', 'dynamic_fixed_point'), ('uint8', 'uint8', 'asymmetric_affine')):
        kinds, ops = descriptor(lib, compile_variant(work, name, [1024, 176], qtype, quantizer) / 'export_nbg_unify/network_binary.nb')
        f = fields(ops[0])
        print('%-14s kinds %s, data types in %d out %d (bit 2: %d, %d), tile %dx%d, x stride %d (listed, not predicted)' % (
              name, kinds, f['in_image_data_type'], f['out_image_data_type'], f['in_image_data_type_bit_2'], f['out_image_data_type_bit_2'],
              f['out_image_tile_x_size'], f['out_image_tile_y_size'], f['in_image_x_stride']))
    print('FAIL %d variants' % failures if failures else 'PASS every 16-bit variant matches the decode in all 48 words')
    return 1 if failures else 0


def run_mutation(work, lib, source, name, changes):
    """Copy a simulated graph, change descriptor words of operation 1, simulate it; returns the output as int16 values."""
    import numpy as np
    top = work / 'mutations' / name
    out = top / 'out.dat'
    if not out.is_file():
        if top.exists():
            trash = CACHE / 'trash'
            trash.mkdir(parents=True, exist_ok=True)
            top.rename(trash / ('npu-descriptor-%s-%d' % (name, os.getpid())))
        top.mkdir(parents=True)
        shutil.copytree(source, top / 'g', symlinks=True)
        export = top / 'g/export_nbg_unify'
        old = top / 'old-output'
        old.mkdir()
        for f in export.glob('output0_*.dat'):
            f.rename(old / f.name)
        command = [sys.executable, '-B', str(ROOT / 'tools/npu-nbg-mutate.py'), str(source / 'export_nbg_unify/network_binary.nb'),
                   str(export / 'network_binary.nb'), '--native', str(lib)]
        for word, value in changes:
            command += ['--descriptor', '1', str(word), hex(value)]
        subprocess.run(command, check=True, capture_output=True)
        inside = ('cd /work/g/export_nbg_unify && export LD_LIBRARY_PATH=%s/vsimulator/lib:%s/common/lib VSIMULATOR_SHADER_CORE_COUNT=1 '
                  'VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B && timeout 900 ./f network_binary.nb sim-input0.qnt.tensor' % (SDK, SDK))
        done = container(top, inside, '8g')
        (top / 'sim.log').write_text(done.stdout + done.stderr)
        produced = sorted(export.glob('output0_*.dat'))
        if done.returncode or not produced:
            sys.exit('%s: simulation failed; log %s' % (name, top / 'sim.log'))
        shutil.copyfile(produced[0], out)
    return np.fromfile(out, dtype='<i2').astype(np.int64)


def mutate(a):
    import numpy as np
    work = a.work
    work.mkdir(parents=True, exist_ok=True)
    lib = native(work)
    small = compile_variant(work, 'sim-h16x176', [16, 176], simulate=True)
    planes = compile_variant(work, 'sim-c2x64x176', [2, 64, 176], simulate=True)
    large = compile_variant(work, 'sim-c2x256x176', [2, 256, 176], simulate=True)

    def codes(graph):
        text = (graph / 'export_nbg_unify/sim-input0.qnt.tensor').read_text().split()
        return np.array([int(v) for v in text], dtype=np.int64)

    def words(graph):
        return descriptor(lib, graph / 'export_nbg_unify/network_binary.nb')[1][0]
    results = []

    def case(field, name, graph, changes, expected):
        got = run_mutation(work, lib, graph, name, changes)
        ok = got.shape == expected.shape and bool(np.array_equal(got, expected))
        results.append(ok)
        print('%-4s %-40s %-22s %s' % ('PASS' if ok else 'FAIL', field, name, ', '.join('word %d = %#x' % c for c in changes)))

    # 16 rows of 176 values, one plane. The relu of a 16-bit code is max(code, 0) at the same fraction length.
    x = codes(small).reshape(16, 176)
    ref = np.maximum(x, 0)
    w = words(small)
    case('control (word 0 rewritten unchanged)', 'control', small, [(0, w[0])], ref.ravel())
    e = ref.copy(); e[:, 88:] = 0
    case('out_image_x_size 176 -> 88', 'out-x-88', small, [(3, (w[3] & ~(0x1fff << 6)) | (88 << 6))], e.ravel())
    case('in_image_x_size 176 -> 88 (border 0)', 'in-x-88', small, [(1, (w[1] & ~(0x1fff << 6)) | (88 << 6))], e.ravel())
    e = ref.copy(); e[8:] = 0
    case('out_image_y_size 16 -> 8', 'out-y-8', small, [(3, (w[3] & 0x7ffff) | (8 << 19))], e.ravel())
    case('in_image_y_size 16 -> 8 (border 0)', 'in-y-8', small, [(1, (w[1] & 0x7ffff) | (8 << 19))], e.ravel())
    e = np.zeros_like(ref); e[0:16:2] = ref[0:8]
    case('out_image_x_stride 352 -> 704, 8 rows', 'out-stride-704', small, [(17, (w[17] & ~0xffff) | 704), (3, (w[3] & 0x7ffff) | (8 << 19))], e.ravel())
    e = np.zeros_like(ref); e[0:8] = ref[0:16:2]
    case('in_image_x_stride 352 -> 704, 8 rows', 'in-stride-704', small,
         [(16, (w[16] & ~0xffff) | 704), (1, (w[1] & 0x7ffff) | (8 << 19)), (3, (w[3] & 0x7ffff) | (8 << 19))], e.ravel())
    case('tile 127x2 -> 64x1 (no output change)', 'tile-64x1', small, [(4, (w[4] & 0x3ffff) | (1 << 25) | (64 << 18))], ref.ravel())
    case('tile 127x2 -> 32x4 (no output change)', 'tile-32x4', small, [(4, (w[4] & 0x3ffff) | (4 << 25) | (32 << 18))], ref.ravel())
    e = np.zeros(16 * 352, dtype=np.int64).reshape(16, 352); e[:, :176] = np.minimum(ref, 255)
    got_bytes = None
    name = 'out-type-bit2-clear'
    got = run_mutation(work, lib, small, name, [(15, w[15] & ~(1 << 21))])
    got_bytes = got.astype('<i2').tobytes()
    as_bytes = np.frombuffer(got_bytes, dtype=np.uint8).astype(np.int64).reshape(16, 352)
    ok = bool(np.array_equal(as_bytes, e))
    results.append(ok)
    print('%-4s %-40s %-22s %s' % ('PASS' if ok else 'FAIL', 'out_image_data_type bit 2 clear (8-bit, saturating at 255)', name, 'word 15 = %#x' % (w[15] & ~(1 << 21))))

    # Two planes of 64 x 176: plane stride 22,528 bytes, in the low halves of words 16 and 8.
    x = codes(planes)
    ref = np.maximum(x, 0)
    P = 64 * 176
    w = words(planes)
    case('control, two planes', 'control-planes', planes, [(0, w[0])], ref)
    case('in plane stride 22528 -> 0', 'in-plane-0', planes, [(16, w[16] & 0xffff)], np.concatenate([ref[:P], ref[:P]]))
    case('out plane stride 22528 -> 0', 'out-plane-0', planes, [(8, w[8] & 0xffff)], np.concatenate([ref[P:], np.zeros(P, np.int64)]))
    case('out_image_z_size 2 -> 1', 'z-1', planes, [(4, (w[4] & ~0x3fff) | 1)], np.concatenate([ref[:P], np.zeros(P, np.int64)]))
    # Two planes of 256 x 176: plane stride 90,112 bytes = 0x16000, so word 26 holds 1 in each half.
    x = codes(large)
    ref = np.maximum(x, 0)
    P = 256 * 176
    w = words(large)
    shift = 0x6000 // 2
    case('word 26 low half 1 -> 0 (in plane stride bits 16-31)', 'w26-low-0', large, [(26, w[26] & 0xffff0000)],
         np.concatenate([ref[:P], np.maximum(x[shift:shift + P], 0)]))
    e = np.concatenate([ref[:P], ref[P + P - shift:], np.zeros(P - shift, np.int64)])
    case('word 26 high half 1 -> 0 (out plane stride bits 16-31)', 'w26-high-0', large, [(26, w[26] & 0xffff)], e)
    failed = results.count(False)
    print('FAIL %d of %d mutations' % (failed, len(results)) if failed else 'PASS all %d mutations' % len(results))
    return 1 if failed else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'generate':
        generate(sys.argv[2], json.loads(sys.argv[3]))
        return 0
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    s = sub.add_parser('sweep')
    s.add_argument('--work', type=Path, required=True)
    s.add_argument('--widths', type=int, nargs='+', default=[32, 48, 64, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256, 288, 320, 384, 448, 512])
    m = sub.add_parser('mutate')
    m.add_argument('--work', type=Path, required=True)
    a = p.parse_args()
    return sweep(a) if a.command == 'sweep' else mutate(a)


if __name__ == '__main__':
    sys.exit(main())
