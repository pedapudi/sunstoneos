#!/usr/bin/env python3
"""Render the fixed-model C compatibility table from the reviewed JSON catalogue."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
CATALOGUE = ROOT / 'tools/npu-models.json'
OUTPUT = ROOT / 'drivers/npu/npu_models.inc'
FAMILIES = {'nn': 0, 'sh-nt': 1, 'nn-pool': 2}
GUARDS = {'sh-nt': 'SUNSTONE_NPU_LARGE_GRAPH', 'nn-pool': 'SUNSTONE_NPU_NN_POOL_GRAPH'}


def unsigned(value):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise ValueError('catalogue value must be an unsigned 32-bit integer')
    return f'{value}u'


def table(entries):
    if not isinstance(entries, list) or not entries:
        raise ValueError('catalogue models must be a nonempty list')
    lines = ['/* Compatibility view of tools/npu-models.json; refresh with drivers/npu/catalogue.py.',
             ' * Hash identity does not establish device confinement. */']
    names, digests = set(), set()
    for model in entries:
        try:
            name = model['name']
            digest = model['model']['sha256']
            family = model.get('family', 'nn')
            if (not isinstance(name, str) or not name or name in names or
                not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest) or
                digest in digests or family not in FAMILIES):
                raise ValueError('invalid or duplicate model identity or execution family')
            names.add(name)
            digests.add(digest)
            sizes = [model['model']['bytes'], model['input']['bytes'],
                     model['reference']['bytes'], model['vip_sram_bytes'], model['coeff_bytes']]
            tensors = [model['input_tensor'], model['output_tensor']]
            if any(not isinstance(t, list) or len(t) != 12 for t in tensors):
                raise ValueError('each fixed-model tensor requires twelve descriptor words')
            counts = [model[k] for k in ('nn_operations', 'descriptor_patches',
                                         'state_patches', 'command_body_bytes')]
            # Validate all emitted numbers before appending any record.
            for value in sizes + tensors[0] + tensors[1] + counts:
                unsigned(value)
            if any(value == 0 for value in sizes[:4]) or not counts[0]:
                raise ValueError('model, tensor, SRAM and operation extents must be nonzero')
            guard = GUARDS.get(family)
            if guard:
                lines.append(f'#if defined({guard})')
            rendered_digest = ','.join(f'0x{b:02x}' for b in bytes.fromhex(digest))
            lines.append('{ {' + rendered_digest + '}, ' + ','.join(map(unsigned, sizes)) + ',')
            for tensor in tensors:
                lines.append('  {' + ','.join(map(unsigned, tensor)) + '},')
            lines.append('  ' + ','.join(map(unsigned, counts)) +
                         f' NPU_MODEL_FAMILY({FAMILIES[family]}) }},')
            if guard:
                lines.append('#endif')
        except (KeyError, TypeError) as error:
            raise ValueError('invalid fixed-model catalogue record') from error
    return '\n'.join(lines) + '\n'


def render(path=CATALOGUE):
    try:
        document = json.loads(Path(path).read_text())
        return table(document['models'])
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise ValueError('invalid model catalogue JSON') from error


def verify():
    if OUTPUT.read_text() != render():
        raise ValueError('stale npu_models.inc; run drivers/npu/catalogue.py --output drivers/npu/npu_models.inc')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.write_text(render())
    else:
        verify()


if __name__ == '__main__':
    main()
