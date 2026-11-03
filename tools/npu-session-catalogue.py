#!/usr/bin/env python3
"""Maintain the reviewed NPU model catalogue and generate an image's admitted table.

`add` records one compiled model with its reference input and output tensor
files: identities, tensor descriptors and the native assembly plan computed by
a host build of the repository decoder and assembler. Only reviewed compiler
output belongs in the catalogue.

`generate` writes the npu_admitted_models table (include/npu_admitted_models.h)
for one image into its build directory. Its entries are the models that the
image's applications list, in list order with repeats removed. A model list is
a JSON object {"npu_models": [catalogue names]} in the application's directory.
An entry makes the model executable by the runtime and session drivers of
that image only.

Build the host library with:
  clang-21 -std=c11 -O2 -shared -fPIC -Iinclude drivers/npu/nbg_assembly.c \
    drivers/npu/nbg_model.c drivers/npu/nbg_relocation.c drivers/npu/nbg_checksum.c drivers/storage/storage_digest.c \
    drivers/npu/nbg_layout_abi.c -o native.so
"""
import argparse
import ctypes as c
import hashlib
import importlib.util
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The list of described models. SUNSTONE_NPU_MODEL_DB names another list, for an application's own graphs: the entries
# only describe tensors and locate files; the OS runtime admits a graph by its structure, not by being listed.
CATALOGUE = Path(os.environ.get('SUNSTONE_NPU_MODEL_DB', ROOT / 'tools/npu-session-models.json'))
# The most tensors, inputs and outputs together, of a graph in the generated table npu_admitted_models (the session driver's own
# table: 2 * NPU_SESSION_TENSORS in include/npu_session.h). The OS runtime's layouts have no such limit.
MAX_TENSORS = 8


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class Requirement(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in ('payload_bytes', 'minimum_bytes', 'allocation_bytes')]


class Layout(c.Structure):
    # Mirrors struct nbg_assembly_layout (include/nbg_assembly.h): the caller's table of allocations (four fixed ones, the
    # tensors and the dynamic-input allocation) and its entry count, then the counts and the dynamic-input flag.
    _fields_ = [('buffers', c.POINTER(Requirement)), ('capacity', c.c_uint32)] + [(n, c.c_uint32) for n in (
        'inputs', 'outputs', 'nn_operations', 'descriptor_patches', 'state_patches',
        'command_body_bytes', 'dynamic', 'program_offset', 'program_bytes')]


# The values of nbg_layout_abi (drivers/npu/nbg_layout_abi.c) in the order of this table: the C size and offsets that the
# ctypes mirror above must equal.
ABI = (('sizeof', None), ('requirement', None), ('buffers', 'buffers'), ('inputs', 'inputs'), ('outputs', 'outputs'),
       ('nn_operations', 'nn_operations'), ('descriptor_patches', 'descriptor_patches'),
       ('state_patches', 'state_patches'), ('command_body_bytes', 'command_body_bytes'), ('dynamic', 'dynamic'))
ABI += (('program_offset', 'program_offset'), ('program_bytes', 'program_bytes'))


def check_abi(lib, layout=Layout):
    """Refuse a mirror of struct nbg_assembly_layout whose size or field offsets differ from the compiled C structure."""
    lib.nbg_layout_abi.argtypes = [c.c_uint]
    lib.nbg_layout_abi.restype = c.c_ulong
    for index, (label, field) in enumerate(ABI):
        want = lib.nbg_layout_abi(index)
        have = c.sizeof(layout) if label == 'sizeof' else c.sizeof(Requirement) if label == 'requirement' else \
            getattr(layout, field).offset
        if have != want:
            raise ValueError(f'the Layout mirror differs from struct nbg_assembly_layout in {label}: {have} against {want}')
    if layout.capacity.offset != lib.nbg_layout_abi(12):
        raise ValueError('the Layout mirror places the table capacity differently')


def identity(path):
    blob = Path(path).read_bytes()
    return dict(bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())


def add(args):
    decoder = module('decoder', ROOT / 'checks/nbg-model.py')
    data = args.model.read_bytes()
    native = decoder.Native(args.native, data)
    check_abi(native.lib)
    native.lib.nbg_assembly_layout.argtypes = [c.POINTER(decoder.View), c.POINTER(Layout)]
    layout = Layout()
    # The table is ours: one entry for each of the four fixed allocations, every tensor and the dynamic-input allocation.
    table = (Requirement * (5 + native.view.header.input_count + native.view.header.output_count))()
    layout.buffers = c.cast(table, c.POINTER(Requirement)); layout.capacity = len(table)
    if native.lib.nbg_assembly_layout(c.byref(native.view), c.byref(layout)) != 1:
        raise ValueError('native assembler refused the model')
    header = native.view.header
    inputs = native.tensors(0, header.input_count)
    outputs = native.tensors(1, header.output_count)
    if len(args.inputs) != len(inputs) or len(args.references) != len(outputs):
        raise ValueError('one reference file is required per tensor')
    tensors = []
    for tensor, path in zip(inputs + outputs, args.inputs + args.references):
        if Path(path).stat().st_size != tensor['logical_bytes']:
            raise ValueError(f'{path} does not hold {tensor["logical_bytes"]} bytes')
        tensors.append(dict(tensor, reference=identity(path)))
    entry = dict(name=args.name, model=identity(args.model), inputs=tensors[:len(inputs)],
                 outputs=tensors[len(inputs):], vip_sram_bytes=header.vip_sram_bytes,
                 pool_bytes=header.pool_bytes,
                 nn_operations=layout.nn_operations, descriptor_patches=layout.descriptor_patches,
                 state_patches=layout.state_patches, command_body_bytes=layout.command_body_bytes,
                 compiler=args.compiler)
    catalogue = json.loads(CATALOGUE.read_text()) if CATALOGUE.exists() else dict(
        compiler_lock='tools/npu-compiler.lock.json', models=[])
    if any(m['name'] == args.name or m['model']['sha256'] == entry['model']['sha256']
           for m in catalogue['models']):
        raise ValueError('model or name already catalogued')
    catalogue['models'].append(entry)
    CATALOGUE.write_text(json.dumps(catalogue, indent=1) + '\n')


def artifact_files(paths, name):
    """Resolve a catalogue name in an artifact map; callers verify every file."""
    # Application example: existing maps use the historical mimus layer name.
    # Prefer the current key when both exist so invalid new entries still fail.
    if name == 'linear-i16-per-row' and name not in paths:
        return paths['mimus-linear-q']
    return paths[name]


def application_models(lists):
    """Catalogue entries that the model lists name, in list order, each once."""
    catalogue = {m['name']: m for m in json.loads(CATALOGUE.read_text())['models']}
    names = []
    for path in lists:
        listed = json.loads(Path(path).read_text())
        if set(listed) != {'npu_models'} or not isinstance(listed['npu_models'], list):
            raise ValueError(f'{path} is not a model list')
        for name in listed['npu_models']:
            if name not in catalogue:
                raise ValueError(f'{path}: {name} is not a reviewed catalogue model')
            if name not in names:
                names.append(name)
    if not names:
        raise ValueError('the applications list no NPU model')
    return [catalogue[n] for n in names]


def table(entries):
    lines = ['/* Generated by tools/npu-session-catalogue.py from tools/npu-session-models.json',
             ' * and the image\'s application model lists. Reviewed compiler output only; hash',
             ' * identity does not establish device confinement. */',
             '#include "npu_admitted_models.h"',
             'const struct npu_admitted_model npu_admitted_models[] = {']
    for m in entries:
        digest = ','.join(f'0x{b:02x}' for b in bytes.fromhex(m['model']['sha256']))
        sizes = [t['logical_bytes'] for t in m['inputs'] + m['outputs']]
        if len(sizes) > 2 * MAX_TENSORS:
            raise ValueError(f"{m['name']} has more tensors than the table holds")
        lines.append(f"/* {m['name']} */")
        lines.append(f"{{ {{{digest}}}, {m['model']['bytes']}u, {len(m['inputs'])}u, "
                     f"{len(m['outputs'])}u,")
        lines.append('  {' + ','.join(f'{v}u' for v in sizes) + '},')
        lines.append(f"  {m['nn_operations']}u,{m['descriptor_patches']}u,{m['state_patches']}u,"
                     f"{m['command_body_bytes']}u }},")
    lines.append('};')
    lines.append(f'const unsigned npu_admitted_model_count = {len(entries)}u;')
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    a = sub.add_parser('add')
    a.add_argument('name')
    a.add_argument('--model', type=Path, required=True)
    a.add_argument('--native', type=Path, required=True)
    a.add_argument('--inputs', type=Path, nargs='+', required=True)
    a.add_argument('--references', type=Path, nargs='+', required=True)
    a.add_argument('--compiler', required=True, help='fixture generator and quantizer that produced the model')
    g = sub.add_parser('generate')
    g.add_argument('--application', type=Path, action='append', required=True,
                   help='an application model list; repeat for each application in the image')
    g.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    if args.command == 'add':
        add(args)
    else:
        text = table(application_models(args.application))
        if not args.output.exists() or args.output.read_text() != text:
            args.output.write_text(text)


if __name__ == '__main__':
    main()
