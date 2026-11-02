#!/usr/bin/env python3
"""Admission of compiled single-operation graphs by the OS assembler, and refusal of mutated copies.

Runs nbg_assembly_layout (drivers/npu/nbg_assembly.c with the decoder, checksum and relocation units) on graphs that
tools/npu-graph-variants.py and tools/npu-graph-compile.sh build from model descriptions of this repository: every
graph must be admitted. Then each refusal case changes one field of a copy (tools/npu-nbg-mutate.py repairs the
checksums, so only the loader's own rule can refuse it) and the copy must be refused. A negative control rebuilds the
assembler without the rule that the case tests; the mutated copy must then be admitted, so the case does test the
rule. The graphs are found under --graphs (one directory per graph with export_nbg_unify/network_binary.nb).
"""
import argparse, ctypes as c, importlib.util, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_relocation.c', 'drivers/npu/nbg_model.c',
                'include/nbg_assembly.h', 'tools/npu-nbg-mutate.py', 'checks/nbg-graph-admission.py')
GRAPHS = ('add', 'sub', 'mul', 'relu', 'sig', 'tanh', 'swish', 'ln176', 'ln64', 'sm64', 'sm32', 'chainA', 'chainB',
          'chainC', 'deep', 'mm', 'glu')


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded)
    return loaded


mutate = module('mutate', ROOT / 'tools/npu-nbg-mutate.py')
nm = mutate.nm


def build(work, name, replacements=()):
    """Compile the assembler (and decoder) into a shared library, optionally with exact text replacements."""
    sources = {}
    for unit in ('nbg_model.c', 'nbg_checksum.c', 'nbg_relocation.c', 'nbg_assembly.c'):
        text = (ROOT / 'drivers/npu' / unit).read_text()
        for unit_name, old, new in replacements:
            if unit_name == unit:
                assert text.count(old) == 1, (unit, old)
                text = text.replace(old, new)
        path = work / (name + '-' + unit)
        path.write_text(text)
        sources[unit] = path
    lib = work / (name + '.so')
    subprocess.run(['/usr/bin/clang-21', '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-function', '-I' + str(ROOT / 'include'),
                    '-I' + str(ROOT / 'drivers/npu'), '-o', str(lib), *map(str, sources.values())], check=True)
    return lib


def admit(library, data):
    lib = c.CDLL(str(library))
    buf = c.create_string_buffer(data, len(data))
    model, layout = (c.c_ubyte * 4096)(), (c.c_ubyte * 16384)()
    lib.nbg_model_open.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.nbg_assembly_layout.argtypes = [c.c_void_p, c.c_void_p]
    if lib.nbg_model_open(buf, len(data), model) != 0:
        return False
    return bool(lib.nbg_assembly_layout(model, layout))


class Graph:
    """A graph file with byte edits; `edit` returns a new graph with repaired checksums."""
    def __init__(self, data, native):
        self.data, self.native = data, native
        self.view = nm.Native(native, data).view
        self.native_obj = nm.Native(native, data)

    def section(self, index): return self.view.sections[index]

    def with_bytes(self, section, offset, raw):
        data = bytearray(self.data)
        at = self.section(section).offset + offset
        data[at:at + len(raw)] = raw
        out = Graph.__new__(Graph)
        out.data, out.native, out.view, out.native_obj = mutate.repair(bytes(data), self.native, self.view), self.native, self.view, self.native_obj
        return out

    def patch_offset(self, op, destination):
        """Byte offset in the patch section of the descriptor patch with this destination."""
        operation = self.native_obj.operations()[op]
        for i in range(operation[4], operation[4] + operation[5]):
            row = self.native_obj.patches()[i]
            if row[0] == 1 and row[1] == destination:
                return 28 * i
        raise KeyError(destination)

    def state_offset(self, op):
        """Byte offset in the load-states section of the load-state stream of an operation."""
        span = nm.Span()
        assert self.native_obj.lib.nbg_model_table(c.byref(self.native_obj.view), 26, self.native_obj.operations()[op][3], c.byref(span)) == 1
        return span.data - self.native_obj.view.data - self.section(20).offset

    def word_offset(self, op, word):
        return self.native_obj.operations()[op][1] * 192 + word * 4


def u32(value): return struct.pack('<I', value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graphs', type=Path, default=Path.home() / '.cache/sunstoneos/build/npu-graph-variants')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    native = build(args.output, 'fixed')
    graphs = {}
    for name in GRAPHS:
        path = args.graphs / name / 'export_nbg_unify/network_binary.nb'
        assert path.exists(), 'missing graph ' + str(path)
        graphs[name] = Graph(path.read_bytes(), native)
        assert admit(native, graphs[name].data), name + ' is refused'
    print('admitted', len(GRAPHS), 'graphs:', ' '.join(GRAPHS))
    results = []
    RELOCATION_OFF = [('nbg_relocation.c', 'p->source_offset >= p->source_bytes ||', '0 ||'),
                      ('nbg_relocation.c', 'p->referenced_bytes > p->source_bytes - p->source_offset ||', '0 ||')]

    def case(label, graph, edits, control):
        mutated = graphs[graph]
        for section, offset, raw in edits:
            mutated = mutated.with_bytes(section, offset, raw)
        assert not admit(native, mutated.data), label + ': the loader admitted the mutated graph'
        entry = {'case': label, 'refused': True}
        if control:
            lib = build(args.output, 'control%d' % len(results), control)
            assert admit(lib, mutated.data), label + ': the loader without its rule still refused'
            entry['control_admits'] = True
        results.append(entry)
        print('refused:', label, '(without its rule: admitted)' if control else '(refused by the relocation bound)')
    g = graphs
    NN, PATCH, INPUT, STATES = 6, 9, 0, 20
    # 1. A second input outside the mapping.
    case('second input names a graph input that does not exist', 'add', [(PATCH, g['add'].patch_offset(1, 136) + 12, u32(7))], None)
    case('second input ring lies beyond VIP SRAM', 'chainB', [(PATCH, g['chainB'].patch_offset(2, 136) + 16, u32(98304 + 0x100000)),
          (PATCH, g['chainB'].patch_offset(2, 140) + 16, u32(103936 + 0x100000))],
         [('nbg_assembly.c', '!size || size > limit || limit > end->source_bytes ||', '!size ||'), *RELOCATION_OFF])
    # 2. A wrong extent.
    case('second input ring smaller than its start offset allows', 'chainB', [(NN, g['chainB'].word_offset(2, 33), u32(0x10))],
         [('nbg_assembly.c', 'start->source_offset < limit - size ||', '0 ||')])
    case('descriptor names more bytes than the graph input holds', 'relu', [(INPUT, 4, u32(88))],
         [('nbg_assembly.c', '(word(image->nn + word_index * 4) >> 16) > t.logical_bytes) return 0;', '0) return 0;')])
    # 3. A mismatched shape or type.
    case('second graph input is larger than the first', 'mul', [(INPUT, 112 + 4, u32(352))],
         [('nbg_assembly.c', 'a.logical_bytes != b.logical_bytes) return 0;', '0) return 0;')])
    case('second graph input has another type than the first', 'add', [(INPUT, 112 + 28, u32(5) + u32(0) + u32(1) + u32(13))],
         [('nbg_assembly.c', 'a.format != b.format || a.quantization != b.quantization ||', '0 ||')])
    # 4. Aliasing with the output: move the third operation's output ring of graph "deep" onto its second input.
    case('output overlaps the second input', 'deep',
         [(PATCH, g['deep'].patch_offset(3, 28) + 16, u32(104448)), (PATCH, g['deep'].patch_offset(3, 76) + 16, u32(110080))],
         [('nbg_assembly.c', 'aliased(nn, slot, source, op.kind) ||', '0 ||')])
    # 5. Stream-processor programs: only reviewed bodies, at their position in the program memory.
    program_check = ('nbg_assembly.c', '|| !sp_program_reviewed(span.data, pairs)) return 0;', ') return 0;')
    case('one word of a stream-processor program changed', 'ln176', [(STATES, g['ln176'].state_offset(2) + 12, u32(0x12345678))],
         [program_check])
    case('a program placed at another position of the program memory', 'ln176', [(STATES, g['ln176'].state_offset(2) + 4, u32(6))],
         [('nbg_assembly.c', 'if (word(span.data + 4) != (*program & 255)', 'if (0')])
    print('PASS %d admission cases and %d refusal cases' % (len(GRAPHS), len(results)))


if __name__ == '__main__':
    main()
