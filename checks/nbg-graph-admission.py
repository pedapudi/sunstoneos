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
          'chainC', 'deep', 'mm', 'glu', 'sh-exp', 'sh-gather', 'sh-resize', 'sh-pad')


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
        path = work / (name + '-' + Path(unit).name)
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

    def state_words(self, op):
        """The load-state words of an operation, with the byte offset of the stream in the load-states section."""
        span = nm.Span()
        assert self.native_obj.lib.nbg_model_table(c.byref(self.native_obj.view), 26, self.native_obj.operations()[op][3], c.byref(span)) == 1
        at = self.state_offset(op)
        raw = self.data[self.section(20).offset + at:self.section(20).offset + at + span.bytes]
        return at, struct.unpack('<%dI' % (len(raw) // 4), raw)

    def state_word_offset(self, op, value, occurrence=0):
        """Byte offset in the load-states section of the n-th word equal to value."""
        at, words = self.state_words(op)
        found = [i for i, w in enumerate(words) if w == value]
        return at + 4 * found[occurrence]

    def kind0_patch_offset(self, op, destination):
        """Byte offset in the patch section of the state-stream patch of an operation with this destination."""
        operation = self.native_obj.operations()[op]
        for i in range(operation[4], operation[4] + operation[5]):
            if self.native_obj.patches()[i][1] == destination:
                return 28 * i
        raise KeyError(destination)

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
    PATCH_SECTION = PATCH
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
    # 4b. A stream-processor operation that keeps its result in the engine's scratch carries an offset in its output
    # pointer word that no patch sets (the steps of a softmax over 92 positions in compiled attention layers hold 0x4220 to 0x1cee0). The low
    # device range below NBG_ASSEMBLY_LOW_RESERVED is never given to a buffer (placed()), so such a word cannot reach a mapped
    # byte: it is admitted, and a word at or above the range is refused.
    low = g['sm64'].with_bytes(NN, g['sm64'].word_offset(1, 7), u32(0x4220))
    assert admit(native, low.data), 'an unpatched output pointer inside the reserved low range is refused'
    print('admitted: an unpatched output pointer word of 0x4220 (inside the reserved low device range)')
    case('an unpatched output pointer word lies outside the reserved low device range', 'sm64',
         [(NN, g['sm64'].word_offset(1, 7), u32(0x100000))],
         [('nbg_assembly.c', 'word(descriptor + (size_word == 20 ? 24 : 28)) < NBG_ASSEMBLY_LOW_RESERVED &&', '1 &&')])
    # 5. Stream-processor programs and unseen graphs. There is no table of reviewed programs or of reviewed graphs: a
    # program the loader has never seen, and so a graph whose digest nobody listed, is admitted, because the engine
    # reaches memory only through the page tables (docs/designs/npu-loader-admission.md). The operand that places the
    # program in the 256-entry program memory must fit it.
    admitted = []

    def admitted_case(label, graph, edits):
        mutated = graphs[graph]
        for section, offset, raw in edits:
            mutated = mutated.with_bytes(section, offset, raw)
        assert mutated.data != graphs[graph].data and admit(native, mutated.data), label + ': the loader refused it'
        admitted.append(label)
        print('admitted:', label)
    admitted_case('a stream-processor program word never seen before', 'ln176',
                  [(STATES, g['ln176'].state_offset(2) + 12, u32(0x12345678))])
    admitted_case('a program placed at another position of the program memory', 'ln176',
                  [(STATES, g['ln176'].state_offset(2) + 4, u32(6))])
    case('a program placed beyond the program memory', 'ln176', [(STATES, g['ln176'].state_offset(2) + 4, u32(256))],
         [('nbg_assembly.c', 'if (word(span.data + 4) > 255) return 0;', '')])
    # 6. Shader operations (docs/hardware/npu-command-formats.md, "Loader specification (shader operations)"). A shader
    # stream may write only the registers the compiler writes and the uniform bank. The MMU, cache-flush and
    # command-fetch registers are refused one by one; each case has a control that rebuilds the assembler without the
    # rule, and the changed graph must then be admitted.
    SH, SHP = 'sh-exp', 9
    REGISTER_RULE = [('nbg_assembly.c', 'if (!uniform && !shader_register(r)) return 0;', 'if (0) return 0;')]
    base = g[SH].native_obj.operations()[1]
    header_02a4 = 0x080102a4
    for label, register in (('the MMU control register (0x388)', 0x00e2), ('the MMU status register (0x38c)', 0x00e3),
                            ('the command-fetch size register (0x3a4)', 0x00e9), ('the command-fetch control register (0x3a8)', 0x00ea),
                            ('the command-fetch address register (0x654)', 0x0195), ('the control register (0x000)', 0x0000),
                            ('a register that the compiler does not write (0x1234)', 0x1234)):
        case('a shader stream writes ' + label, SH, [(STATES, g[SH].state_word_offset(1, header_02a4), u32(0x08010000 | register))],
             REGISTER_RULE)
    flush = 0x08010e03
    case('a shader stream selects other cache-flush bits than the loader-owned 0x24', SH,
         [(STATES, g[SH].state_word_offset(1, flush) + 4, u32(0x25))],
         [('nbg_assembly.c', 'if ((r == 0x0e02 && v != 0x701) || (r == 0x0e03 && v != 0x24)) return 0;', 'if (0) return 0;')])
    case('a shader stream uses another synchronization token than 0x701', SH,
         [(STATES, g[SH].state_word_offset(1, 0x48000000) + 4, u32(0x700))],
         [('nbg_assembly.c', 'word(state.data + 4 * (i + 1)) != 0x701) return 0;', '0) return 0;')])
    case('a shader stream writes a uniform word twice', SH,
         [(STATES, g[SH].state_word_offset(1, 0x0804d804), u32(0x0804d800))],
         [('nbg_assembly.c', 'if (uniform_seen[bit / 8] & (1u << (bit % 8))) return 0;', 'if (0) return 0;')])
    case('a shader program length differs from its instruction count', SH,
         [(STATES, g[SH].state_word_offset(1, 0x08010424) + 4, u32(5))],
         [('nbg_assembly.c', '(uint64_t)instructions * 16 != program.bytes) return 0;', '0) return 0;'),
          ('nbg_assembly.c', 'last_pc != instructions - 1 ||', '0 ||')])
    case('a shader operation patches a second program address', SH,
         [(PATCH_SECTION, g[SH].kind0_patch_offset(1, 28) + 8, u32(0))],
         [('nbg_assembly.c', 'if (reg != 0x040a || p.index != -1 || p.source_offset || p.transformed || program_patch) return 0;', 'if (0) return 0;')])
    case('a shader patch names a source that the loader does not know', 'sh-gather',
         [(PATCH_SECTION, 28 * (g['sh-gather'].native_obj.operations()[1][4] + 1) + 8, u32(4))], None)
    case('a shader patch has an unknown transformed field', 'sh-gather',
         [(PATCH_SECTION, 28 * (g['sh-gather'].native_obj.operations()[1][4] + 1) + 20, u32(5))],
         [('nbg_assembly.c', 'if (p.transformed && !(p.transformed == 14 && p.source == 6)) return 0;', 'if (0) return 0;'),
          ('nbg_relocation.c', 'p->source_offset >= p->source_bytes ||', '0 ||')])
    case('a shader program is not 256-byte aligned', SH, [(24, 0, u32(128))],
         [('nbg_assembly.c', 'if ((start & 255) || !program.bytes', 'if (!program.bytes')])
    case('a shader descriptor reaches beyond the tensor it names', SH,
         [(PATCH_SECTION, g[SH].kind0_patch_offset(1, 28) + 16, u32(64))],
         [('nbg_assembly.c', 'if ((uint64_t)p.source_offset + extent > region) return 0;', 'if (0) return 0;'), *RELOCATION_OFF])
    # 7. A descriptor of a long tensor. The compiler writes width + (height << 16) in 32 bits without masking: the
    # relative-shift table of an attention layer over T = 184 positions is four rows of 67,528 elements (hw
    # 0x000507c8, carried), and the index table of T = 276 positions is 76,176 rows of one element (hw 0x29900001,
    # cut). The graph is the small gather with its input descriptor and input size replaced by those of the layer.
    G = 'sh-gather'
    data_header = 0x0804d800
    plane_header = 0x0804d804

    def long_tensor(stride, hw, plane, size):
        return [(INPUT, 4, u32(size)), (STATES, g[G].state_word_offset(1, data_header) + 8, u32(stride)),
                (STATES, g[G].state_word_offset(1, data_header) + 12, u32(hw)),
                (STATES, g[G].state_word_offset(1, plane_header) + 4, u32(plane))]
    admitted_case('a shader descriptor whose width carries into the height field (T = 184)', G,
                  long_tensor(135056, 0x000507c8, 540224, 540224))
    admitted_case('a shader descriptor whose height is cut to 16 bits (T = 276)', G,
                  long_tensor(4, 0x29900001, 304704, 304704))
    WIDTH_RULE = [('nbg_assembly.c', 'if (!height || width > stride) return 0;', 'if (!height) return 0;')]
    case('a shader descriptor whose width exceeds its row stride', G, long_tensor(135056, 0x000907c8, 540224, 540224), WIDTH_RULE)
    case('a shader descriptor whose channel stride is not a multiple of its row stride', G,
         long_tensor(135056, 0x000507c8, 540225, 540225),
         [('nbg_assembly.c', 'if (!stride || !depth || plane % stride) return 0;', 'if (!stride || !depth) return 0;')])
    case('a shader descriptor whose height field disagrees with its channel stride', G,
         long_tensor(128, 0x00300040, 4096, 4096), WIDTH_RULE)
    print('PASS %d graphs, %d unseen programs admitted and %d refusal cases' % (len(GRAPHS), len(admitted), len(results)))


if __name__ == '__main__':
    main()
