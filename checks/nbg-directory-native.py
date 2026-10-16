#!/usr/bin/env python3
"""Compare native directory decoding with the saved SDK layout/CRC evidence."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('model', type=Path)
    ap.add_argument('native', type=Path)
    ap.add_argument('reference', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    data = args.model.read_bytes()
    saved = args.reference.read_bytes()
    assert hashlib.sha256(data).hexdigest() == 'db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca'
    assert hashlib.sha256(saved).hexdigest() == 'd6f38448546ef1c35f4bf5456907f7595d791d7cfb28ebd632b0ed89f3e9c856'
    expected = json.loads(saved)
    path = Path(__file__).with_name('nbg-model.py')
    spec = importlib.util.spec_from_file_location('nbg_model', path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    native = module.Native(args.native, data)
    for section in expected['sections']:
        index = section['type'] - 1
        view = native.view.sections[index]
        assert (view.offset, view.bytes) == (section['offset'], section['bytes'])
        assert native.section(index) == data[view.offset:view.offset + view.bytes]
    assert [list(row) for row in native.operations()] == expected['records']['4']
    patches = [row + [0, 0, 0] for row in expected['records']['10']]
    for row in patches:
        if row[3] >= 1 << 31:
            row[3] -= 1 << 32  # The native source index is signed, including the -1 sentinel.
    assert [list(row) for row in native.patches()] == patches
    for wire, index in [('23', 22), ('27', 26)]:
        assert [list(row) for row in native.table_entries(index)] == expected['records'][wire]
    for tensor in expected['tensors']:
        actual = native.tensors(tensor['type'] - 1, 1)[0]
        assert actual['dimensions'] == tensor['sizes'][:tensor['dimensions']]
        for field, key in [('name','name'), ('format','data_format'),
                           ('buffer_type','data_type'), ('quantization','quant_format'),
                           ('zero_point','zero_point')]:
            assert actual[field] == tensor[key]
        assert actual['scale_bits'] == int(tensor['scale_bits'], 16)
    report = dict(result='pass', sections_compared=len(expected['sections']),
                  operations_compared=2, patches_compared=5, tensors_compared=2,
                  ranges_compared=native.table_calls,
                  model_sha256=hashlib.sha256(data).hexdigest(),
                  native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
                  reference_sha256=hashlib.sha256(saved).hexdigest(),
                  sdk_executed=False, physical_access=False)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
