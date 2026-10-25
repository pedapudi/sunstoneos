#!/usr/bin/env python3
"""Board-bundle preparation of the returning GPU images on port 3-1.

tools/prepare-board-bundles.py calls prepare_bundle(row, build, check,
output) inside its sandbox for three rows: the provider-readiness checkpoint
(apps/gpu-provider-ready/build.py), and the power-only and identity variants
of apps/gpu-power-id/build.py. The function copies the image, its binary,
its build record and its focused host-check result into the bundle with the
kernel runner's host tools, and validate() checks the copy:

- probe.bin is the loadable segments of image.elf, and both match the build
  record, whose variant and success text are the row's;
- the host-check result names the same image, passed every case of its
  variant and, for the power-only variant, records no GPU register access;
- equivalence.json lists every linked object with its origin: compiled by
  the build ("new") or reused from the image the build names, none of which
  has run on a board to completion;
- in both power variants, the GPU controller's step tables in image.elf
  (on_steps and off_steps of drivers/gpu/a733_gpu_power.c) power TOP5
  through the domain-5 row before the first CORE6 access and remove it
  only after the last; the power-only image defines no identity reader.

Nothing here contacts a device.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OBJCOPY = '/usr/lib/llvm-21/bin/llvm-objcopy'
HOST_TOOLS = {'check-a733-kernel.py': 'tools/check-a733-kernel.py', 'check-a733-smp.py': 'tools/check-a733-smp.py',
              'usb-access.py': 'tools/usb-access.py'}
SCHEMA = 'board-gpu-returning-bundle-v1'
# The image each build reuses objects of, by its build record's identity.
PROVIDER_READY = '3adfa4ad6ba386ce8d39f76646f540107c2a5d02ce42db6c1c35c90de68a75ed'
FAILED_POWER_ID = 'fd3d5839f700b5e0f569b0ce722e186c33beba65512815b136d9c969a7341afe'
CASES = {'provider-ready': 11, 'power-only': 9, 'identity': 13}
# struct step of drivers/gpu/a733_gpu_power.c: kind, bank, offset, mask, value.
STEP = struct.Struct('<5I')
KIND_DOMAIN = 3        # enum kind: CHECK, UPDATE, DELAY, DOMAIN, CORE_POLICY, ...
BANK_CORE = 2          # enum a733_gpu_power_bank: A733_GPU_CCU, A733_GPU_R_CCU, A733_GPU_CORE


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def variant_of(record):
    return record.get('variant', 'provider-ready')


def step_table(elf, name):
    """The rows of one static step table of the linked image, read from its loadable bytes."""
    from elftools.elf.elffile import ELFFile
    with Path(elf).open('rb') as stream:
        image = ELFFile(stream)
        found = [s for s in image.get_section_by_name('.symtab').iter_symbols()
                 if s.name == name and s['st_info']['type'] == 'STT_OBJECT']
        require(len(found) == 1, f'{elf} defines {len(found)} objects named {name}')
        symbol = found[0]
        section = image.get_section(symbol['st_shndx'])
        data = section.data()[symbol['st_value'] - section['sh_addr']:][:symbol['st_size']]
    require(len(data) == symbol['st_size'] and len(data) % STEP.size == 0, f'{name} is not a table of steps')
    return [STEP.unpack_from(data, i) for i in range(0, len(data), STEP.size)]


def power_order(elf):
    """TOP5 before CORE6 at ON, CORE6 before TOP5 at OFF, in the image's own step tables."""
    on, off = step_table(elf, 'on_steps'), step_table(elf, 'off_steps')
    on_domain = [i for i, s in enumerate(on) if s[0] == KIND_DOMAIN]
    off_domain = [i for i, s in enumerate(off) if s[0] == KIND_DOMAIN]
    on_core = [i for i, s in enumerate(on) if s[1] == BANK_CORE and s[0] != KIND_DOMAIN]
    off_core = [i for i, s in enumerate(off) if s[1] == BANK_CORE and s[0] != KIND_DOMAIN]
    require(len(on_domain) == 1 and on[on_domain[0]][4] == 1, 'ON has no single domain-5 ON step')
    require(len(off_domain) == 1 and off[off_domain[0]][4] == 0, 'OFF has no single domain-5 OFF step')
    require(on_core and min(on_core) > on_domain[0], 'A CORE6 access precedes the domain-5 ON request')
    require(off_core and max(off_core) < off_domain[0], 'A CORE6 access follows the domain-5 OFF request')
    require(on[-1][1] == BANK_CORE and on[-1][2] == 8 and on[-1][4] == 8, 'ON does not end with the CORE6 status poll')
    return {'on_steps': len(on), 'off_steps': len(off), 'on_domain_step': on_domain[0], 'on_first_core_step': min(on_core),
            'on_last_step': 'CORE6 status poll for 8', 'off_last_core_step': max(off_core),
            'off_domain_step': off_domain[0]}


def prepare_bundle(row, build, check, output):
    """Copy one GPU image and its records into the bundle directory output, then validate it."""
    build, check = Path(build), Path(check)
    record = json.loads(build.read_text())
    directory = build.parent
    binary = directory / ('probe.bin' if (directory / 'probe.bin').is_file() else 'checkpoint.bin')
    output.mkdir()
    shutil.copyfile(directory / 'image.elf', output / 'image.elf')
    shutil.copyfile(binary, output / 'probe.bin')
    shutil.copyfile(build, output / 'build.json')
    shutil.copyfile(check, output / 'host-check.json')
    tools = output / 'host/tools'
    tools.mkdir(parents=True)
    for name, rel in HOST_TOOLS.items():
        shutil.copyfile(ROOT / rel, tools / name)
    source = PROVIDER_READY if variant_of(record) != 'provider-ready' else record.get('base_image_sha256')
    origin = {PROVIDER_READY: 'gpu-provider-ready', FAILED_POWER_ID: 'gpu-power-id-fd3d5839'}.get(source)
    require(origin is not None, 'The build reuses objects of an unrecorded image.')
    objects = {}
    for item in record['new_objects']:
        objects[item.get('source', Path(item['object']).name)] = {'sha256': item['sha256'], 'origin': 'new'}
    for item in record['reused_objects']:
        objects[Path(item.get('original', item['object'])).name] = {'sha256': item['sha256'], 'origin': origin}
    (output / 'equivalence.json').write_text(json.dumps({'objects': objects, 'image_sha256': record['image_sha256']},
                                                        indent=2) + '\n')
    return validate(row, output)


def validate(row, output):
    record = json.loads((output / 'build.json').read_text())
    variant = variant_of(record)
    require(variant == row['variant'], f'The build is the {variant} variant, not the row\'s {row["variant"]}.')
    require(record['expect'] == row['expect'], 'The build\'s success text is not the row\'s.')
    require(sha256(output / 'image.elf') == record['image_sha256'] and
            sha256(output / 'probe.bin') == record['binary_sha256'], 'The bundle files are not the built image.')
    flat = output.parent / (output.name + '-objcopy.bin')
    result = subprocess.run([OBJCOPY, '-O', 'binary', str(output / 'image.elf'), str(flat)], capture_output=True,
                            text=True)
    require(result.returncode == 0, f'objcopy failed: {result.stderr.strip()}')
    same = flat.read_bytes() == (output / 'probe.bin').read_bytes()
    flat.unlink()
    require(same, 'probe.bin is not the loadable segments of image.elf.')
    checked = json.loads((output / 'host-check.json').read_text())
    require(checked.get('image_sha256') == record['image_sha256'] and 'failed_case' not in checked
            and checked.get('physical_access') is False and len(checked.get('cases', [])) == CASES[variant],
            f'The host check did not pass all {CASES[variant]} cases of this image.')
    require(checked.get('variant', 'provider-ready') == variant, 'The host check ran another variant.')
    equivalence = json.loads((output / 'equivalence.json').read_text())
    require(equivalence['image_sha256'] == record['image_sha256'], 'equivalence.json names another image.')
    value = {'schema': SCHEMA, 'row': row['name'], 'variant': variant, 'image_sha256': record['image_sha256'],
             'new_objects': sorted(k for k, o in equivalence['objects'].items() if o['origin'] == 'new'),
             'reused_objects': sum(o['origin'] != 'new' for o in equivalence['objects'].values()),
             'reused_from': sorted({o['origin'] for o in equivalence['objects'].values()} - {'new'}),
             'check_cases': len(checked['cases']), 'power_order': None}
    if variant != 'provider-ready':
        value['power_order'] = power_order(output / 'image.elf')
        names = subprocess.run(['/usr/lib/llvm-21/bin/llvm-nm', '-P', str(output / 'image.elf')], capture_output=True,
                               text=True, check=True).stdout.split()
        identity = 'id_read' in names or 'id_reads' in names
        require(record['gpu_register_window_access'] == (variant == 'identity') and identity == (variant == 'identity'),
                'The image\'s GPU register access does not match its variant.')
        require(all(not case['gpu_writes'] for case in checked['cases']), 'A host-check case wrote the GPU window.')
        if variant == 'power-only':
            require(all(not case['gpu_reads'] for case in checked['cases']), 'A host-check case read the GPU window.')
    return value
