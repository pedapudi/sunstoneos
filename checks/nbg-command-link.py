#!/usr/bin/env python3
"""A command stream longer than one fetch is cut into segments joined by LINK commands.

The command fetch is a 16-bit count of 8-byte units (524,280 bytes). nbg_assembly.c cuts a longer stream between
operations and ends each segment with the vendor's LINK command (opcode 8, the next fetch size, the next address). The
graphs of this repository are far shorter, so the check rebuilds the assembler with a segment limit of 1,024 bytes
(NBG_SEGMENT_BYTES, the smallest of 192 to 1,100 bytes that admits the graph) and assembles compiled graphs in both ways, the one-call assembly and the stepwise one the runtime
uses. It then walks each command allocation the way the front end does, following the links, and requires: the
states executed are exactly the states of the unsplit stream, in order, with the same patched addresses; every
segment, link included, is within the limit; every link names a 64-byte aligned address inside the allocation, a
fetch size that is the rest of the allocation (at most one fetch), and is followed by END words up to its target; the
two assembly paths write the same bytes. With the default limit the same graphs hold no link at all.
"""
import argparse, ctypes as c, importlib.util, mmap, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}', '--graphs', '{fixture:npu-graphs}']]
CHECK_INPUTS = ('tools/npu-graph-specs.json',)
FETCH = 524280
GRAPHS = ('deep', 'chainB', 'ln176', 'sm64', 'sh-gather', 'sh-exp', 'mm', 'glu')
LIMITS = (192, 256, 384, 512, 768, 1100)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded)
    return loaded


admission = module('admission', ROOT / 'checks/nbg-graph-admission.py')
SLOTS = 21


class Buffer(c.Structure):
    _fields_ = [('cpu', c.c_void_p), ('bytes', c.c_size_t), ('device', c.c_uint32)]


def load(path, data):
    lib = c.CDLL(str(path))
    lib.nbg_model_open.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    for name in ('nbg_assembly_layout',):
        getattr(lib, name).argtypes = [c.c_void_p, c.c_void_p]
    lib.nbg_assemble_layout.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
    lib.nbg_assembly_begin.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p]
    lib.nbg_assembly_next.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, c.c_uint]
    return lib


def new_layout(entries=64):
    """A struct nbg_assembly_layout (include/nbg_assembly.h) with a caller's table of entries: {table pointer, capacity, counts}."""
    table = (c.c_uint32 * (3 * entries))()
    layout = (c.c_ubyte * 16384)()
    struct.pack_into('<QI', layout, 0, c.addressof(table), entries)
    layout.table = table   # the layout points into it: keep it alive as long as the layout is
    return layout, table


def requirements(raw, table):
    """(allocation bytes by slot, inputs, outputs, dynamic, nn_operations, command_body_bytes) from a layout and its table."""
    sizes = [struct.unpack_from('<III', bytes(table), 12 * i)[2] for i in range(SLOTS)]
    inputs, outputs, nn, _, _, body, dynamic = struct.unpack_from('<7I', raw, 12)
    return sizes, inputs, outputs, dynamic, nn, body


def assemble(lib, data, stepwise):
    """Assemble one graph into fresh memory; returns (command allocation bytes, device address, command_body_bytes)."""
    buf = c.create_string_buffer(data, len(data))
    model = (c.c_ubyte * 4096)()
    assert lib.nbg_model_open(buf, len(data), model) == 0
    layout, table = new_layout()
    assert lib.nbg_assembly_layout(model, layout) == 1
    sizes, inputs, outputs, dynamic, nn, body = requirements(bytes(layout), table)
    count = 4 + inputs + outputs + dynamic
    maps, array, device = [], (Buffer * count)(), 0x10000000
    for i in range(count):
        if sizes[i]:
            mem = mmap.mmap(-1, sizes[i]); maps.append(mem)
            array[i] = Buffer(c.addressof(c.c_char.from_buffer(mem)), sizes[i], device)
            device += (sizes[i] + 0xfffff) & ~0xfffff
    out, out_table = new_layout()
    if stepwise:
        cursor = (c.c_uint32 * 4)()
        assert lib.nbg_assembly_begin(model, array, count, out, cursor) == 1
        done = 0
        for step in range(nn + 4):
            done = lib.nbg_assembly_next(model, array, out, cursor, 1 + step % 3)
            assert done >= 0, 'stepwise assembly refused'
            if done == 1: break
        assert done == 1
        # The caller fills the rest of the allocation with END pairs (apps and runtime do the same).
        command = bytearray(c.string_at(array[3].cpu, array[3].bytes))
        for at in range(cursor[2], len(command), 8):
            command[at:at + 8] = struct.pack('<II', 0x10000000, 0)
        return bytes(command), array[3].device, body, cursor[2], command
    assert lib.nbg_assemble_layout(model, array, count, out) == 1
    return c.string_at(array[3].cpu, array[3].bytes), array[3].device, body, None, None


def admits(lib, data):
    buf = c.create_string_buffer(data, len(data)); model = (c.c_ubyte * 4096)()
    return lib.nbg_model_open(buf, len(data), model) == 0 and lib.nbg_assembly_layout(model, new_layout()[0]) == 1


def walk(command, device, limit):
    """Follow the stream like the front end. Returns (executed packets, segments [(start, end)], links)."""
    executed, segments, links = [], [], []
    at = start = 0
    while True:
        w = struct.unpack_from('<I', command, at)[0]
        op = w >> 27
        if w == 0x10000000:
            segments.append((start, at + 8)); break
        if op == 8:
            target = struct.unpack_from('<I', command, at + 4)[0] - device
            fetch = (w & 0xffff) * 8
            assert target > at and target % 64 == 0 and target < len(command), 'link target'
            assert fetch == min(len(command) - target, FETCH), 'link fetch size'
            assert all(struct.unpack_from('<II', command, i) == (0x10000000, 0) for i in range(at + 8, target, 8)), 'filler'
            segments.append((start, at + 8)); links.append((at, target))
            at = start = target
            continue
        if op == 1:
            n = (w >> 16) & 0x7ff
            executed.append((w,) + struct.unpack_from('<%dI' % n, command, at + 4))
            at += 4 * (1 + n + ((1 + n) & 1))
        elif w == 0x48000000:
            executed.append((w, struct.unpack_from('<I', command, at + 4)[0])); at += 8
        else:
            raise AssertionError('unexpected command word %08x at %d' % (w, at))
    for first, last in segments:
        assert last - first <= limit, 'segment longer than the limit'
    return executed, segments, links


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--graphs', type=Path, default=Path.home() / '.cache/sunstoneos/build/npu-graph-variants')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    normal = load(admission.build(args.output, 'normal'), None)
    libs = {limit: load(admission.build(args.output, 'limit%d' % limit, [('nbg_assembly.c', '#define NBG_SEGMENT_BYTES A733_NPU_FETCH_BYTES',
                                                                         '#define NBG_SEGMENT_BYTES %du' % limit)]), None)
            for limit in LIMITS}
    total_links = 0
    for name in GRAPHS:
        data = (args.graphs / name / 'export_nbg_unify/network_binary.nb').read_bytes()
        whole, device, body, _, _ = assemble(normal, data, False)
        plain = walk(whole, device, FETCH)[0]
        assert not walk(whole, device, FETCH)[2], name + ': a short stream holds a link'
        limit = next(l for l in LIMITS if admits(libs[l], data))
        results = []
        for stepwise in (False, True):
            command, device_s, body_s, end, filled = assemble(libs[limit], data, stepwise)
            executed, segments, links = walk(command, device_s, limit)
            assert executed == plain, name + ': the split stream executes other states'
            results.append(command)
            if not stepwise:
                total_links += len(links)
            print('%-10s limit %4d %s: %5d bytes in %2d segments, %2d links, %d states' % (
                name, limit, 'stepwise' if stepwise else 'one-call', body_s + 56, len(segments), len(links), len(executed)))
        assert results[0] == results[1], name + ': the stepwise and the one-call assembly write different bytes'
    assert total_links >= 8, 'the limit split too few streams to mean anything'
    # Controls: a link to the wrong address, and a fetch size larger than the rest of the allocation, are caught by the walk.
    for label, old, new in (('a link to the wrong address', 'put(commands + from + 4, device + to);', 'put(commands + from + 4, device + to + 8);'),
                            ('a fetch size past the allocation', 'if (window > A733_NPU_FETCH_BYTES) window = A733_NPU_FETCH_BYTES;',
                             'window = A733_NPU_FETCH_BYTES;')):
        bad = load(admission.build(args.output, 'bad%d' % len(label), [('nbg_assembly.c', '#define NBG_SEGMENT_BYTES A733_NPU_FETCH_BYTES',
                                                                       '#define NBG_SEGMENT_BYTES 256u'), ('nbg_assembly.c', old, new)]), None)
        data = (args.graphs / 'ln176' / 'export_nbg_unify/network_binary.nb').read_bytes()
        command, device_b, _, _, _ = assemble(bad, data, False)
        try:
            walk(command, device_b, 256)
        except AssertionError:
            print('control caught:', label)
        else:
            raise SystemExit('control missed: ' + label)
    # A limit that no operation fits is refused rather than looped on.
    tiny = load(admission.build(args.output, 'tiny', [('nbg_assembly.c', '#define NBG_SEGMENT_BYTES A733_NPU_FETCH_BYTES',
                                                       '#define NBG_SEGMENT_BYTES 128u')]), None)
    data = (args.graphs / 'sh-exp' / 'export_nbg_unify/network_binary.nb').read_bytes()
    buf = c.create_string_buffer(data, len(data)); model = (c.c_ubyte * 4096)()
    assert tiny.nbg_model_open(buf, len(data), model) == 0 and tiny.nbg_assembly_layout(model, new_layout()[0]) == 0
    print('PASS %d graphs, %d links, tiny limit refused' % (len(GRAPHS), total_links))


if __name__ == '__main__':
    main()
