#!/usr/bin/env python3
"""The session catalogue tool's ctypes mirror of struct nbg_assembly_layout matches the C structure.

tools/npu-session-catalogue.py passes a Layout structure to the native assembler. If its size or a field offset differs
from the compiled structure, the assembler writes past the Python object. This check builds the real assembler
and the ABI helper (drivers/npu/nbg_layout_abi.c) from the real header and compares size and every offset; --tool names
another copy of the tool, to show that an older mirror fails.
"""
import argparse, ctypes as c, importlib.util, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['tools/npu-session-catalogue.py', 'include/nbg_assembly.h', 'drivers/npu/nbg_layout_abi.c',
                'drivers/npu/nbg_assembly.c', 'checks/npu-catalogue-layout.py']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--tool', type=Path, default=ROOT / 'tools/npu-session-catalogue.py')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
lib = args.output / 'native.so'
subprocess.run(['/usr/bin/clang-21', '-std=c11', '-O1', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'drivers/npu'), *(str(ROOT / 'drivers' / u) for u in (
                    'npu/nbg_assembly.c', 'npu/nbg_model.c', 'npu/nbg_relocation.c', 'npu/nbg_checksum.c',
                    'storage/storage_digest.c', 'npu/nbg_layout_abi.c')), '-o', str(lib)], check=True)
spec = importlib.util.spec_from_file_location('catalogue_tool', args.tool)
tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(tool)
native = c.CDLL(str(lib))
native.nbg_layout_abi.argtypes = [c.c_uint]
native.nbg_layout_abi.restype = c.c_ulong
# The comparison is made here, from the C values, so that it also judges a tool without its own assertion.
labels = ['sizeof', 'requirement', 'buffers', 'inputs', 'outputs', 'nn_operations', 'descriptor_patches', 'state_patches',
          'command_body_bytes', 'dynamic', 'program_offset', 'program_bytes']
mirror = tool.Layout
have = [c.sizeof(mirror), c.sizeof(tool.Requirement)] + [getattr(getattr(mirror, f, None), 'offset', -1) for f in labels[2:]]
for index, label in enumerate(labels):
    assert have[index] == native.nbg_layout_abi(index), \
        'the Layout mirror differs from struct nbg_assembly_layout in %s: %d against %d' % (label, have[index], native.nbg_layout_abi(index))
assert mirror.capacity.offset == native.nbg_layout_abi(12), 'the table capacity is placed differently'
if hasattr(tool, 'check_abi'):
    tool.check_abi(native)
print('PASS the catalogue tool mirror matches struct nbg_assembly_layout: size %d, capacity at %d' % (have[0], native.nbg_layout_abi(12)))
