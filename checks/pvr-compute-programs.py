#!/usr/bin/env python3
"""Compare native offline compute programs with the pinned Mesa compiler/PDS path."""
import argparse
import hashlib
import json
import pathlib
import re
import shlex
import subprocess

REVISION = '86c7176d2dd0065555521bf8c61f208bf53c75d6'
SOURCE_HASHES = {'src/imagination/pco/usclib/common.cl': '0b89e8c7cf9af7182ab97220fa3dac5c87380325af94aaa8fc0ff850da7ee1cf',
 'src/imagination/pco/pco_data.h': '062dec5efcbc802230a0261ca90e0a88b0aa7082d9acba954508d039e1259178',
 'src/imagination/pco/uscgen/pco_clc.c': '1281aa7e7b909d496a25f145ebc72bcdebc2755aec83bdde351b700091e9a1d4',
 'src/imagination/pco/uscgen/meson.build': '295c3de027936f0efb44428e4ddd6fb29b988985edb7c01bde6373976aee9098',
 'src/imagination/vulkan/pds/pvr_pds.c': 'dfa8cd8b3fcfedcc44f181fc0e2e4ed6fbfc2ada92dd8a00fc1072f777a6339d',
 'src/imagination/vulkan/pds/pvr_pds.h': '43b2bc22ea75c778ba1f1316b4f8cf6f5117dd4c7d44c0f35065b8d002d19e12',
 'src/imagination/vulkan/pds/pvr_rogue_pds_defs.h': 'ec127fdc17d922eb69240815302d95f11dd02cb1fccd11abefd470c373995662',
 'src/imagination/vulkan/pds/pvr_rogue_pds_encode.h': '9559113eb4894220462153e23366b7c4a661fc9c0e52486914465790db032bd2',
 'src/imagination/common/device_info/bxm-4-64.h': '4fc2ef4cfa1652f77d72cc868760bdd760f9ccd361dd491d8a097f87ea6ec145',
 'src/imagination/csbgen/rogue/cr.xml': 'e66018b7e9aa58623364c2ebdc52b1e901ab0b41423283ac99e7a7047aaefa27',
 'src/imagination/csbgen/rogue/lls.xml': 'a66e5eeaf8b968756cd614be9d74451c8283d033c023ce5dcf9727dd64498146',
 'src/imagination/vulkan/pvr_arch_job_context.c': 'abfcfcd0bd6d6824a3269f8707fa037b98b7699ecca2517e2b4a95b2f5f71bc0',
 'src/imagination/vulkan/pvr_device.c': 'e55b28baaa45a68d2bebbcbdd1f08c81275a0ed26f9aeb4b5c99d1a1c14ad55c'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, cwd, evidence, name):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    (evidence / (name + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{name} failed: {result.stderr[-3000:]}')
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa-source', type=pathlib.Path, required=True)
    parser.add_argument('--mesa-build', type=pathlib.Path, required=True)
    parser.add_argument('--evidence', type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    source, build, evidence = (p.resolve() for p in
                              (args.mesa_source, args.mesa_build, args.evidence))
    evidence.mkdir(parents=True, exist_ok=True)
    for name, expected in SOURCE_HASHES.items():
        if digest(source / name) != expected:
            raise RuntimeError(f'unpinned source: {name}')
    # Compiler generation is an explicit prerequisite, never an automatic rerun.
    generated = build / 'src/imagination/pco/uscgen/pco_uscgen_programs.c'
    metadata = source / 'src/imagination/pco/pco_data.h'
    pds = source / 'src/imagination/vulkan/pds/pvr_pds.c'
    text = generated.read_text()
    arrays = []
    usc = {}
    for name in ('cs_store_sr_1024_common_0_common', 'cs_load_sr_256_common_0_common'):
        match = re.search(r'static const uint32_t ' + name + r'\[\d+\] = \{.*?\};', text, re.S)
        if not match:
            raise RuntimeError(f'missing USC output: {name}')
        arrays.append(match[0])
        words = [int(x, 16) for x in re.findall(r'0x([0-9a-f]+)', match[0])]
        if len(words) < 3 or not any(words[2:]):
            raise RuntimeError('empty USC program')
        usc[name] = {'metadata_words': words[:2], 'binary_bytes': (len(words) - 2) * 4,
                     'binary_sha256': hashlib.sha256(b''.join(w.to_bytes(4, 'little') for w in words[2:])).hexdigest()}
    struct = re.search(r'typedef struct PACKED _pco_precomp_data \{.*?\} pco_precomp_data;', metadata.read_text(), re.S)[0]
    (evidence / 'reference-usc.inc').write_text(struct + '\n' + '\n'.join(arrays) + '\n')
    # Only unrelated indirect-draw functions need unavailable generated headers.
    # Keep the entire preceding source unchanged, including all selected routines.
    prefix = pds.read_text().split('#include "pvr_draw_indirect_arrays0.h"')[0]
    (evidence / 'mesa-pds-prefix.c').write_text(prefix)
    commands = json.loads((build / 'compile_commands.json').read_text())
    common = next(c for c in commands if c['file'].endswith('pvr_device_info.c'))
    flags = shlex.split(common['command'])
    flags = flags[:flags.index('-MD')]
    flags = [f for f in flags if f != '-DNDEBUG']
    flags += ['-I' + str(source / 'src/imagination/vulkan'),
              '-I' + str(source / 'src/imagination/vulkan/pds'),
              '-I' + str(root / 'include'), '-I' + str(evidence), '-DPVR_BUILD_ARCH_ROGUE']
    used = []
    used.append(run(flags + ['-c', str(evidence / 'mesa-pds-prefix.c'), '-o', str(evidence / 'mesa-pds.o')], build, evidence, 'reference-pds'))
    used.append(run(flags + ['-DUPSTREAM', str(root / 'checks/pvr-compute-programs.c'),
                             str(evidence / 'mesa-pds.o'), str(build / 'src/util/libmesa_util.a'),
                             '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(evidence / 'reference')], build, evidence, 'reference-build'))
    used.append(run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                     '-I' + str(root / 'include'), str(root / 'checks/pvr-compute-programs.c'),
                     str(root / 'drivers/gpu/pvr_compute_programs.c'), '-o', str(evidence / 'native')], root, evidence, 'native-build'))
    comparisons = []
    for variant in (0, 1):
        for impl in ('reference', 'native'):
            used.append(run([str(evidence / impl), str(variant), str(evidence / f'{impl}-{variant}.bin')], root, evidence, f'{impl}-{variant}'))
        a, b = (evidence / f'reference-{variant}.bin').read_bytes(), (evidence / f'native-{variant}.bin').read_bytes()
        if a != b:
            at = next(i for i, (x, y) in enumerate(zip(a, b)) if x != y)
            raise RuntimeError(f'byte mismatch at {at} in variant {variant}')
        comparisons.append({'variant': variant, 'bytes': len(a), 'sha256': hashlib.sha256(a).hexdigest()})
    artifacts = [generated, build / 'src/imagination/pco/uscgen/pco_clc',
                 build / 'src/compiler/clc/mesa_clc', build / 'src/imagination/pco/usclib/pco_usclib.spv',
                 build / 'compile_commands.json', metadata, pds,
                 evidence / 'native', evidence / 'reference']
    artifacts += [root / f for f in ('include/pvr_compute_programs.h', 'drivers/gpu/pvr_compute_programs.c',
                 'drivers/gpu/pvr_compute_usc.inc', 'checks/pvr-compute-programs.c', 'checks/pvr-compute-programs.py')]
    manifest = {'mesa_revision': REVISION, 'scope': 'Offline single-core profile construction; no device execution',
                'usc': usc, 'comparisons': comparisons, 'commands': used,
                'artifacts': {str(p): digest(p) for p in artifacts}}
    (evidence / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'usc': usc, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
