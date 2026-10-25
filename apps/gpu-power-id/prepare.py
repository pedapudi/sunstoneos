#!/usr/bin/env python3
"""Board-bundle preparation of the returning GPU images on port 3-1.

tools/prepare-board-bundles.py calls prepare_bundle(row, build, check,
output) inside its sandbox for the GPU rows: the provider-readiness checkpoint
(apps/gpu-provider-ready/build.py), and the power-only, identity,
wrapper-read and probe-read variants of apps/gpu-power-id/build.py. The function copies the image, its binary,
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
  only after the last; the power-only image defines no identity reader;
- the host check's normal case ran the variant's number of ON/OFF cycles:
  two for the power-only variant, one for the identity and wrapper-read
  variants;
- the wrapper-read image's wrapper_words table holds exactly the four GPU
  wrapper words, in their order, and every host-check case read only a
  prefix of them, with 32-bit loads and no GPU-window store;
- the probe-read image's probe_steps table holds exactly its fifteen
  accesses, every host-check case made only accesses of that table, in its
  order, and each vendor write was restored, by the client or, after a
  synchronous abort, by its restore vectors.

Nothing here contacts a device.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OBJCOPY = '/usr/lib/llvm-21/bin/llvm-objcopy'
HOST_TOOLS = {'check-a733-kernel.py': 'tools/check-a733-kernel.py', 'check-a733-smp.py': 'tools/check-a733-smp.py',
              'usb-access.py': 'tools/usb-access.py'}
SCHEMA = 'board-gpu-returning-bundle-v1'
# The image each build reuses objects of, by its build record's identity.
PROVIDER_READY = '3adfa4ad6ba386ce8d39f76646f540107c2a5d02ce42db6c1c35c90de68a75ed'
FAILED_POWER_ID = 'fd3d5839f700b5e0f569b0ce722e186c33beba65512815b136d9c969a7341afe'
CASES = {'provider-ready': 11, 'power-only': 11, 'identity': 13, 'wrapper-read': 7, 'probe-read': 13, 'vendor-clk': 7}
# ON/OFF cycles of the client's normal run: the power-only variant powers
# CORE6 on twice, so the second ON shows whether TOP5 OFF returned CORE6 to
# its power-up state.
CYCLES = {'power-only': 2, 'identity': 1, 'wrapper-read': 1, 'probe-read': 0, 'vendor-clk': 0}
# The host-check case that completes ON and OFF in each variant.
NORMAL = {'power-only': 'normal', 'identity': 'normal', 'wrapper-read': 'wrapper-on', 'probe-read': 'pr-board', 'vendor-clk': 'vc-board'}
# The only GPU-window addresses of the wrapper-read image, in read order.
WRAPPER_WORDS = [0x01880104, 0x01880100, 0x01880018, 0x0188001c]
# The vendor-clk image's reordered ON steps (on_steps in vendor-clk-power.c):
# kind, bank, offset, mask, value. The CORE6 delay row is dropped and the GPU
# bus reset (0xb24 bit16) is released before the bus gate (0xb24 bit0) and the
# core clock (0xb20 bit31), as sunxi_enable_clks_wrap does.
CORE, CCU = 2, 0
VENDOR_CLK_ON_STEPS = [
    (0, CCU, 0x05c0, 1 << 7, 1 << 7), (0, CCU, 0x05e0, 1 << 16, 1 << 16), (0, 1, 0x01ac, 1, 1),
    (0, CCU, 0x0b20, 1 << 31, 0), (0, CCU, 0x0b24, (1 << 16) | 1, 0),
    (3, CCU, 0, 0, 1), (4, CORE, 0, 0, 0), (5, CORE, 8, 0, 0),
    (8, CORE, 0xfb0, 0, 0), (8, CORE, 0x14, 0, 1), (6, CORE, 0x20, 0, 0),
    (1, CORE, 0x20, 0xffffffff, 0), (0, CORE, 0x20, 0xffffffff, 0), (9, CORE, 8, 0, 3),
    (1, CORE, 0, 0xffffffff, 8), (0, CORE, 0, 0xffffffff, 8), (9, CORE, 8, 0, 4),
    (1, CCU, 0x0b24, 0, 1 << 16), (0, CCU, 0x0b24, (1 << 16) | 1, 1 << 16),
    (1, CCU, 0x0b24, 0, 1), (0, CCU, 0x0b24, (1 << 16) | 1, (1 << 16) | 1),
    (1, CCU, 0x0b20, 0x8700000f, 0x0b000000), (0, CCU, 0x0b20, 0x8700000f, 0x03000000),
    (1, CCU, 0x0b20, 0, (1 << 31) | (1 << 27)), (0, CCU, 0x0b20, 0x8700000f, 0x83000000),
    (8, CORE, 0x14, 0, 5), (7, CORE, 8, 0xffffffff, 8)]
# enum kind: CHECK0 UPDATE1 DELAY2 DOMAIN3 CORE_POLICY4 CORE_STATUS5 CORE_CONFIG6 POLL7 NOTE8 SETTLE9.
# The probe-read image's access table: address, value, kind (0 read32, 1
# write32, 2 write32 of the value read by the row named in value).
PROBE_STEPS = [(0x01880014, 0, 0), (0x01880010, 0, 0), (0x0a0000b0, 0, 0), (0x01800024, 0, 0), (0x01800020, 0, 0),
               (0x0a0000b0, 2, 1), (0x0a0000b0, 0, 0), (0x01880014, 0, 1), (0x01880014, 0, 0), (0x01800024, 0, 0),
               (0x01800020, 0, 0), (0x01880014, 0, 2), (0x01880014, 0, 0), (0x0a0000b0, 2, 2), (0x0a0000b0, 0, 0)]
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


def wrapper_words(elf):
    """The wrapper_words table of a linked wrapper-read image."""
    from elftools.elf.elffile import ELFFile
    with Path(elf).open('rb') as stream:
        image = ELFFile(stream)
        found = [s for s in image.get_section_by_name('.symtab').iter_symbols()
                 if s.name == 'wrapper_words' and s['st_info']['type'] == 'STT_OBJECT']
        require(len(found) == 1 and found[0]['st_size'] == 16, f'{elf} has no single 16-byte wrapper_words table')
        section = image.get_section(found[0]['st_shndx'])
        data = section.data()[found[0]['st_value'] - section['sh_addr']:][:16]
    return list(struct.unpack('<4I', data))


def probe_steps(elf):
    """The probe_steps table of a linked probe-read image."""
    from elftools.elf.elffile import ELFFile
    size = 12 * len(PROBE_STEPS)
    with Path(elf).open('rb') as stream:
        image = ELFFile(stream)
        found = [s for s in image.get_section_by_name('.symtab').iter_symbols()
                 if s.name == 'probe_steps' and s['st_info']['type'] == 'STT_OBJECT']
        require(len(found) == 1 and found[0]['st_size'] == size, f'{elf} has no single {size}-byte probe_steps table')
        section = image.get_section(found[0]['st_shndx'])
        data = section.data()[found[0]['st_value'] - section['sh_addr']:][:size]
    return [struct.unpack_from('<3I', data, 12 * i) for i in range(len(PROBE_STEPS))]


def power_order(elf, cycles):
    """TOP5 before CORE6 at ON, CORE6 before TOP5 at OFF, in the image's own step tables, which
    every one of the client's cycles runs."""
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
    return {'cycles': cycles, 'on_steps': len(on), 'off_steps': len(off), 'on_domain_step': on_domain[0],
            'on_first_core_step': min(on_core),
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
    # A temporary directory, so that a frozen, read-only set can be validated again.
    with tempfile.TemporaryDirectory() as scratch:
        flat = Path(scratch) / 'objcopy.bin'
        result = subprocess.run([OBJCOPY, '-O', 'binary', str(output / 'image.elf'), str(flat)], capture_output=True,
                                text=True)
        require(result.returncode == 0, f'objcopy failed: {result.stderr.strip()}')
        same = flat.read_bytes() == (output / 'probe.bin').read_bytes()
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
        normal = [case for case in checked['cases'] if case.get('case') == NORMAL[variant]]
        require(len(normal) == 1 and normal[0].get('cycles') == CYCLES[variant],
                f'The host check\'s normal case did not run {CYCLES[variant]} ON/OFF cycles.')
        value['power_order'] = power_order(output / 'image.elf', CYCLES[variant])
        names = subprocess.run(['/usr/lib/llvm-21/bin/llvm-nm', '-P', str(output / 'image.elf')], capture_output=True,
                               text=True, check=True).stdout.split()
        identity = 'id_read' in names or 'id_reads' in names
        require(identity == (variant == 'identity'), 'The image\'s identity reader does not match its variant.')
        wrapper = 'wrapper_read' in names or 'wrapper_words' in names
        require(wrapper == (variant == 'wrapper-read'), 'The image\'s wrapper reader does not match its variant.')
        if variant == 'wrapper-read':
            require(wrapper_words(output / 'image.elf') == WRAPPER_WORDS,
                    'The wrapper_words table is not the four GPU wrapper words in their order.')
            for case in checked['cases']:
                reads = [[hex(a), 4] for a in WRAPPER_WORDS]
                require(case['gpu_reads'] == reads[:len(case['gpu_reads'])],
                        'A host-check case read a GPU-window address other than the wrapper words in order.')
            value['wrapper_words'] = [hex(a) for a in WRAPPER_WORDS]
        probe = 'probe_access' in names or 'probe_steps' in names
        require(probe == (variant == 'probe-read'), 'The image\'s probe-read client does not match its variant.')
        if variant == 'probe-read':
            require(probe_steps(output / 'image.elf') == PROBE_STEPS,
                    'The probe_steps table is not the fifteen probe-read accesses in their order.')
            for case in checked['cases']:
                rows = [a[0] for a in case['pr_accesses']]
                require(rows == sorted(set(rows)) and all(
                    a[1] == hex(PROBE_STEPS[r][0]) and a[2] == int(PROBE_STEPS[r][2] in (1, 2))
                    for r, a in zip(rows, case['pr_accesses'])),
                    'A host-check case made an access outside the probe-read table or its order.')
                restored = case.get('handler_writes') == [['0x1880014', '0x1'], ['0xa0000b0', '0x5']] or (
                    (5 not in rows or {13, 14} <= set(rows)) and (7 not in rows or {11, 12} <= set(rows)))
                require('refused' in case or restored, 'A host-check case left a vendor write unrestored.')
                require('a733_gpu_platform_off_begin' not in case['calls'], 'A host-check case powered the GPU OFF.')
            value['probe_steps'] = [[hex(a), hex(v), k] for a, v, k in PROBE_STEPS]
        vclk = 'identity_words' in names
        require(vclk == (variant == 'vendor-clk'), 'The image\'s vendor-clk client does not match its variant.')
        if variant == 'vendor-clk':
            steps = step_table(output / 'image.elf', 'on_steps')
            require(steps == VENDOR_CLK_ON_STEPS, 'on_steps is not the vendor clock and reset order.')
            require(not any(s[1] == BANK_CORE and s[2] in (0x170, 0x174, 0xc00, 0xc04, 0xc10) for s in steps),
                    'The vendor-clk ON steps still write the CORE6 delay row.')
            for case in checked['cases']:
                reads = [[hex(a), 4] for a in (0x01800024, 0x01800020)]
                require(case['gpu_reads'] == reads[:len(case['gpu_reads'])],
                        'A host-check case read a GPU address other than the identity words in order.')
                require('a733_gpu_platform_off_begin' not in case['calls'], 'A host-check case powered the GPU OFF.')
            value['on_steps_order'] = 'vendor clock and reset; CORE6 delay row dropped'
        require(all(not case['gpu_writes'] for case in checked['cases']), 'A host-check case wrote the GPU window.')
        if variant == 'power-only':
            require(all(not case['gpu_reads'] for case in checked['cases']), 'A host-check case read the GPU window.')
    return value
