#!/usr/bin/env python3
"""Check catalogue generation against the accepted compiled model table."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'drivers/npu/catalogue.py'
    assert source.is_file(), 'missing data catalogue generator'
    spec = importlib.util.spec_from_file_location('catalogue', source)
    catalogue = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(catalogue)
    entries = json.loads(catalogue.CATALOGUE.read_text())['models']
    generated = catalogue.table(entries)
    assert generated == catalogue.OUTPUT.read_text(), 'tracked compatibility output is stale'
    # Preserve all fields and conditional layouts through the actual C compiler.
    prefix = '''#include <stdint.h>
#include <stdio.h>
struct model {
 unsigned char digest[32];
 uint32_t bytes,input_bytes,output_bytes,sram,coeff;
 uint32_t input[12],output[12],operations,descriptors,states,commands;
#ifdef SUNSTONE_NPU_LARGE_GRAPH
 uint32_t family;
#define NPU_MODEL_FAMILY(v) ,v##u
#else
#define NPU_MODEL_FAMILY(v)
#endif
};
static const struct model models[]={
'''
    suffix = '''};
int main(void) {
 return fwrite(models,sizeof(models),1,stdout)!=1;
}
'''
    generated_path = args.output / 'generated.inc'
    generated_path.write_text(generated)
    profiles = [[], ['SUNSTONE_NPU_LARGE_GRAPH'],
                ['SUNSTONE_NPU_NN_POOL_GRAPH'],
                ['SUNSTONE_NPU_LARGE_GRAPH', 'SUNSTONE_NPU_NN_POOL_GRAPH']]
    counts = []
    for n, flags in enumerate(profiles):
        binaries = []
        for label, path in [('before', args.baseline.resolve()), ('after', generated_path)]:
            src = args.output / f'{label}-{n}.c'
            executable = args.output / f'{label}-{n}'
            src.write_text(prefix + '#include "' + str(path) + '"\n' + suffix)
            subprocess.run(['clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *['-D' + f for f in flags], str(src), '-o', str(executable)], check=True)
            binaries.append(subprocess.check_output([str(executable)]))
        assert binaries[0] == binaries[1], f'compiled model fields differ for {flags}'
        counts.append(len(binaries[0]))
    bad = []
    mutations = [lambda v: v[0].update(family='unknown'),
                 lambda v: v[0]['model'].update(sha256='invalid'),
                 lambda v: v[0].update(input_tensor=[0]),
                 lambda v: v[0].update(nn_operations=-1),
                 lambda v: v.append(copy.deepcopy(v[0]))]
    for index, mutate in enumerate(mutations):
        changed = copy.deepcopy(entries)
        mutate(changed)
        try:
            catalogue.table(changed)
        except ValueError:
            bad.append(index)
        else:
            raise AssertionError(f'corrupt catalogue accepted: {index}')
    for document in ('{', '{"models": null}', '{"wrong": []}'):
        path = args.output / 'corrupt.json'
        path.write_text(document)
        try:
            catalogue.render(path)
        except ValueError:
            pass
        else:
            raise AssertionError('corrupt JSON accepted')
    stale = args.output / 'stale.inc'
    stale.write_text(generated.replace('1928u', '1929u', 1))
    catalogue.OUTPUT = stale
    try:
        catalogue.verify()
    except ValueError:
        pass
    else:
        raise AssertionError('stale compatibility table accepted')
    assert len(entries) == 6
    result = dict(result='pass', catalogue_models=len(entries), profile_bytes=counts,
                  refused_corrupt_catalogues=len(bad))
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS data catalogue: six entries; four compiled family profiles unchanged; corrupt entries refused')


if __name__ == '__main__':
    main()
