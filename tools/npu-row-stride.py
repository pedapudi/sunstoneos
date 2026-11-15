#!/usr/bin/env python3
"""Give a compiled 16-bit relu graph a wider output row stride, for the board and for the SDK simulator.

The relu's output row stride is descriptor word 17, bits 0-15 (docs/hardware/npu-command-formats.md, confirmed by
mutation). This tool sets it to --stride bytes and widens the output tensor record to the same row length, so that the
OS sizes the output allocation for rows at that stride. The output keeps the compiled image width: each row holds the
graph's values followed by padding the engine must not write.

  npu-row-stride.py graph    --source DIR --out DIR --stride BYTES
      Write the board fixture: DIR/NAME/export_nbg_unify/network_binary.nb (the changed graph), input0.bin (the
      compiled input), reference0.bin (each row of the compiled reference followed by the padding pattern) and
      DIR/artifacts.json naming it graph-NAME. NAME is the source name with "-s" and the stride appended.
  npu-row-stride.py simulate --source DIR --work DIR --stride BYTES --window BYTES
      Run the changed graph in the SDK simulator with an output tensor of --window bytes, prefilled with the padding
      pattern. Require that the first row bytes of every row equal the compiled output, that the padding of every row
      and every byte past stride x rows keep the pattern, and print the result.

DIR for --source is a graph directory built by tools/npu-graph-build.py (relu-w144). The padding pattern is the byte
0xa5: as a 16-bit value it is negative, which a relu output never is. The simulator runs in the pinned compiler image
(tools/npu-compiler.lock.json) offline, with --network none and a memory limit, and writes only under --work.
"""
import argparse
import hashlib
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
PATTERN = 0xa5
NBG_OUTPUT = 1                      # include/nbg_model.h, enum nbg_section_id
OUT_X_STRIDE_WORD = 17


def native(work):
    lib = work / 'nbg.so'
    if not lib.is_file():
        work.mkdir(parents=True, exist_ok=True)
        subprocess.run(['/usr/bin/clang-21', '-shared', '-fPIC', '-O1', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'drivers/npu'),
                        '-o', str(lib)] + [str(ROOT / 'drivers/npu' / f) for f in ('nbg_model.c', 'nbg_checksum.c', 'nbg_relocation.c')], check=True)
    return lib


def nbg_model():
    import importlib.util
    spec = importlib.util.spec_from_file_location('nbg_model', ROOT / 'checks/nbg-model.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def shape(lib, path):
    """The relu's descriptor words and its output tensor record (dimensions and sizes) of a graph file."""
    nm = nbg_model()
    data = Path(path).read_bytes()
    n = nm.Native(lib, data)
    ops = [o for o in n.operations() if o[0] != 0xffff]
    if [o[0] for o in ops] != [7]:
        sys.exit('%s: one activation operation (kind 7) expected, found kinds %s' % (path, [o[0] for o in ops]))
    words = struct.unpack_from('<48I', data, n.view.sections[6].offset + ops[0][1] * 192)
    record = struct.unpack_from('<7I', data, n.view.sections[NBG_OUTPUT].offset)
    return words, record


def change(lib, source, target, stride, rows):
    """Write target from source with the output row stride and an output tensor of stride / 2 by rows 16-bit values."""
    words, record = shape(lib, source)
    width, height = (words[3] >> 6) & 0x1fff, (words[3] >> 19) & 0x1fff
    if record[:3] != (2, width, height) or (words[OUT_X_STRIDE_WORD] & 0xffff) != 2 * width or ((words[4] & 0x3fff) != 1):
        sys.exit('%s: a one-plane 16-bit relu of %d x %d with a compiled row stride is expected' % (source, width, height))
    if stride % 2 or stride < 2 * width or stride > 0xffff:
        sys.exit('the stride must be even, at least %d bytes and at most 65,535' % (2 * width))
    value = (words[OUT_X_STRIDE_WORD] & 0xffff0000) | stride
    command = [sys.executable, '-B', str(ROOT / 'tools/npu-nbg-mutate.py'), str(source), str(target), '--native', str(lib),
               '--descriptor', '1', str(OUT_X_STRIDE_WORD), hex(value),
               '--bytes', str(NBG_OUTPUT), '4', struct.pack('<2I', stride // 2, rows).hex()]
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode or 'checksum match' not in done.stdout:
        sys.exit('npu-nbg-mutate.py failed: %s%s' % (done.stdout, done.stderr))
    changed, out = shape(lib, target)
    moved = [i for i in range(48) if changed[i] != words[i]]
    if moved != [OUT_X_STRIDE_WORD] or out[1:3] != (stride // 2, rows):
        sys.exit('%s: the change moved descriptor words %s and the output record to %s' % (target, moved, out[1:3]))
    return width, height


def padded(reference, width, height, stride):
    rows = reference.reshape(height, 2 * width)
    pad = bytes([PATTERN]) * (stride - 2 * width)
    return b''.join(bytes(r) + pad for r in rows)


def graph(a):
    import numpy as np
    lib = native(a.out / '.native')
    name = a.source.name + '-s%d' % a.stride
    top = a.out / name
    if top.exists():
        trash = CACHE / 'trash'
        trash.mkdir(parents=True, exist_ok=True)
        top.rename(trash / ('%s-%d' % (name, os.getpid())))
    (top / 'export_nbg_unify').mkdir(parents=True)
    words, record = shape(lib, a.source / 'export_nbg_unify/network_binary.nb')
    width, height = change(lib, a.source / 'export_nbg_unify/network_binary.nb', top / 'export_nbg_unify/network_binary.nb',
                           a.stride, record[2])
    shutil.copyfile(a.source / 'input0.bin', top / 'input0.bin')
    reference = np.frombuffer((a.source / 'reference0.bin').read_bytes(), dtype=np.uint8)
    if reference.size != 2 * width * height:
        sys.exit('%s: the reference holds %d bytes, not %d' % (a.source, reference.size, 2 * width * height))
    (top / 'reference0.bin').write_bytes(padded(reference, width, height, a.stride))
    artifacts = a.out / 'artifacts.json'
    entries = json.loads(artifacts.read_text()) if artifacts.is_file() else {}
    entries['graph-' + name] = {'model': str(top / 'export_nbg_unify/network_binary.nb'), 'inputs': [str(top / 'input0.bin')],
                                'references': [str(top / 'reference0.bin')]}
    artifacts.write_text(json.dumps(entries, indent=1) + '\n')
    for f in ('export_nbg_unify/network_binary.nb', 'input0.bin', 'reference0.bin'):
        print('%s %s %d bytes sha256 %s' % (name, f, (top / f).stat().st_size, hashlib.sha256((top / f).read_bytes()).hexdigest()))
    return 0


def image():
    lock = json.loads((ROOT / 'tools/npu-compiler.lock.json').read_text())
    name = lock['loaded_image_id']
    if subprocess.run(['docker', 'image', 'inspect', name], capture_output=True).returncode:
        sys.exit('the compiler image %s is not loaded (tools/npu-compiler.lock.json)' % name)
    return name


def simulate(a):
    import numpy as np
    work = a.work
    lib = native(work)
    if a.window % a.stride:
        sys.exit('the window must be a whole number of rows')
    top = work / ('sim-s%d' % a.stride)
    if top.exists():
        trash = CACHE / 'trash'
        trash.mkdir(parents=True, exist_ok=True)
        top.rename(trash / ('npu-row-stride-%s-%d' % (top.name, os.getpid())))
    shutil.copytree(a.source, top / 'g', symlinks=True)
    export = top / 'g/export_nbg_unify'
    old = top / 'old-build'
    old.mkdir()
    for f in list(export.glob('output0_*.dat')) + list(export.glob('*.o')) + [export / 'f']:
        if f.exists():
            f.rename(old / f.name)
    rows = a.window // a.stride
    width, height = change(lib, a.source / 'export_nbg_unify/network_binary.nb', export / 'network_binary.nb', a.stride, rows)
    # The harness declares the output tensor; widen it to the window and prefill it with the pattern before the run.
    source = (export / 'vnn_f.c').read_text()
    declared = '    attr.size[0] = %d;\n    attr.size[1] = %d;\n' % (width, height)
    if source.count(declared) != 2 or '@attach_y/out0_0:out0' not in source.split(declared)[0]:
        sys.exit('vnn_f.c: the output tensor declaration was not found first')
    source = source.replace(declared, '    attr.size[0] = %d;\n    attr.size[1] = %d;\n' % (a.stride // 2, rows), 1)
    (export / 'vnn_f.c').write_text(source)
    main = (export / 'main.c').read_text()
    run = '    status = vnn_ProcessGraph( graph );\n'
    if main.count(run) != 1:
        sys.exit('main.c: the graph run was not found once')
    prefill = ('    {\n        vsi_nn_tensor_t *o = vsi_nn_GetTensor(graph, graph->output.tensors[0]);\n'
               '        size_t n = (size_t)vsi_nn_GetElementNum(o) * 2u;\n        uint8_t *b = (uint8_t *)malloc(n);\n'
               '        memset(b, 0x%02x, n);\n        if (vsi_nn_CopyDataToTensor(graph, o, b) != VSI_SUCCESS) { printf("prefill failed\\n"); return -1; }\n'
               '        free(b);\n        printf("prefilled %%zu bytes\\n", n);\n    }\n' % PATTERN)
    (export / 'main.c').write_text(main.replace(run, prefill + run))
    command = ['docker', 'run', '--rm', '--network', 'none', '--memory', '8g', '--memory-swap', '8g', '--cpus', '3',
               '--pids-limit', '512', '--cap-drop', 'ALL', '--cap-add', 'CHOWN', '-v', '%s:/work' % top,
               '-v', '%s:/tools:ro' % (ROOT / 'tools'), image(), 'bash', '-c',
               'trap "chown -R %d:%d /work" EXIT; chown -R 0:0 /work; cd /work/g/export_nbg_unify && '
               'make -f makefile.linux USE_IDE_LIB=1 VIVANTE_SDK_DIR=%s/vsimulator > /work/build.log 2>&1 && '
               'export LD_LIBRARY_PATH=%s/vsimulator/lib:%s/common/lib VSIMULATOR_SHADER_CORE_COUNT=1 '
               'VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B && timeout 900 ./f network_binary.nb sim-input0.qnt.tensor'
               % (os.getuid(), os.getgid(), SDK, SDK, SDK)]
    done = subprocess.run(command, capture_output=True, text=True)
    (top / 'sim.log').write_text(done.stdout + done.stderr)
    produced = sorted(export.glob('output0_*.dat'))
    if done.returncode or len(produced) != 1 or 'prefilled' not in done.stdout:
        print('FAIL the simulator did not run the changed graph (exit %d); log %s' % (done.returncode, top / 'sim.log'))
        return 1
    got = np.frombuffer(produced[0].read_bytes(), dtype=np.uint8)
    compiled = np.frombuffer(sorted((a.source / 'export_nbg_unify').glob('output0_*.dat'))[0].read_bytes(), dtype=np.uint8)
    results = []

    def check(name, ok, detail):
        results.append(ok)
        print('%-4s %-52s %s' % ('PASS' if ok else 'FAIL', name, detail))
    check('output tensor bytes', got.size == a.window, '%d (window %d)' % (got.size, a.window))
    if got.size != a.window:
        return 1
    image_rows = got[:height * a.stride].reshape(height, a.stride)
    valid = image_rows[:, :2 * width]
    check('first %d bytes of every row equal the compiled output' % (2 * width),
          bool(np.array_equal(valid, compiled.reshape(height, 2 * width))),
          '%d rows differ' % int((valid != compiled.reshape(height, 2 * width)).any(axis=1).sum()))
    pad = image_rows[:, 2 * width:]
    check('padding of every row keeps the pattern', bool((pad == PATTERN).all()),
          '%d of %d bytes changed' % (int((pad != PATTERN).sum()), pad.size))
    tail = got[height * a.stride:]
    check('no byte past stride x %d rows changed' % height, bool((tail == PATTERN).all()),
          '%d of %d bytes changed' % (int((tail != PATTERN).sum()), tail.size))
    print('PASS all checks' if all(results) else 'FAIL %d checks' % results.count(False))
    return 0 if all(results) else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    g = sub.add_parser('graph')
    g.add_argument('--source', type=Path, required=True)
    g.add_argument('--out', type=Path, required=True)
    g.add_argument('--stride', type=int, required=True)
    s = sub.add_parser('simulate')
    s.add_argument('--source', type=Path, required=True)
    s.add_argument('--work', type=Path, required=True)
    s.add_argument('--stride', type=int, required=True)
    s.add_argument('--window', type=int, required=True)
    a = p.parse_args()
    return graph(a) if a.command == 'graph' else simulate(a)


if __name__ == '__main__':
    sys.exit(main())
