#!/usr/bin/env python3
"""Board-bundle preparation of the returning GPU images on port 3-1.

tools/prepare-board-bundles.py calls prepare_bundle(row, build, check,
output) inside its sandbox for the GPU rows: the provider-readiness checkpoint
(apps/gpu-provider-ready/build.py), and the power-only, identity,
wrapper-read, probe-read, firmware-start and firmware-stop variants of
apps/gpu-power-id/build.py. The
function copies the image, its binary, its build record and its focused
host-check result into the bundle with the kernel runner's host tools, and
validate() checks the copy:

- probe.bin is the loadable segments of image.elf, and both match the build
  record, whose variant and success text are the row's;
- the host-check result names the same image, passed every case of its
  variant and, for the power-only variant, records no GPU register access;
- equivalence.json lists every linked object with its origin: compiled by
  the build ("new") or reused from the image the build names, none of which
  has run on a board to completion;
- in every power variant, the GPU controller's step tables in image.elf
  (on_steps and off_steps of drivers/gpu/a733_gpu_power.c) power TOP5
  through the domain-5 row before the first CORE6 access and remove it
  only after the last, follow the vendor order of
  docs/designs/gpu-vendor-default.md (PLL_GPU0 enabled, the GPU clocks
  opened, PLL_GPU0 at 1008 MHz, then CORE6 policy 8; OFF closes the clocks
  and disables PLL_GPU0 after CORE6 OFF) and never touch the CORE6 delay
  row; the power-only image defines no identity reader;
- the host check's normal case ran the variant's number of ON/OFF cycles:
  two for the power-only and identity variants, one for the wrapper-read
  variant, and every identity case read only the two identity words, high
  word first, in each cycle;
- the wrapper-read image's wrapper_words table holds exactly the four GPU
  wrapper words, in their order, and every host-check case read only a
  prefix of them, with 32-bit loads and no GPU-window store;
- the probe-read image's probe_steps table holds exactly its fifteen
  accesses, every host-check case made only accesses of that table, in its
  order, and each vendor write was restored, by the client or, after a
  synchronous abort, by its restore vectors;
- the firmware-start image's fws_rows table holds exactly its eight
  identity, entry and secure-configuration accesses, it embeds the pinned
  open firmware, and every host-check case wrote only SMC0 +0xb0 = 2 and
  drm_ctrl = 0, once each and never back, and the start registers of
  pvr_gpu_start.c, never SOFT_RESET2, and never powered the GPU off;
- the firmware-stop image passes the same table, firmware and secure
  configuration checks; its cases also wrote only the kernel CCB kick, the
  interrupt clear and the stop's thread-0 association words, and powered
  the GPU off only after the firmware received the power-off request.

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
# The firmware-start images: the start alone, the stop, the first job and the job session.
FWS_VARIANTS = ('fw-start', 'fw-stop', 'fw-job', 'fw-session')
CASES = {'provider-ready': 11, 'power-only': 11, 'identity': 12, 'wrapper-read': 7, 'probe-read': 13, 'fw-start': 11,
         'fw-stop': 12, 'fw-job': 13, 'fw-session': 15}
# ON/OFF cycles of the client's normal run: the power-only and identity
# variants power the GPU on twice, so the second ON shows that OFF left a
# state from which ON succeeds again.
CYCLES = {'power-only': 2, 'identity': 2, 'wrapper-read': 1, 'probe-read': 0, 'fw-start': 0, 'fw-stop': 1, 'fw-job': 1, 'fw-session': 1}
# The host-check case that completes ON and OFF in each variant.
NORMAL = {'power-only': 'normal', 'identity': 'normal', 'wrapper-read': 'wrapper-on', 'probe-read': 'pr-board',
          'fw-start': 'fs-board', 'fw-stop': 'fx-board', 'fw-job': 'fj-board',
          'fw-session': 'fq-board'}
# The only GPU-window addresses of the wrapper-read image, in read order.
WRAPPER_WORDS = [0x01880104, 0x01880100, 0x01880018, 0x0188001c]
# The probe-read image's access table: address, value, kind (0 read32, 1
# write32, 2 write32 of the value read by the row named in value).
PROBE_STEPS = [(0x01880014, 0, 0), (0x01880010, 0, 0), (0x0a0000b0, 0, 0), (0x01800024, 0, 0), (0x01800020, 0, 0),
               (0x0a0000b0, 2, 1), (0x0a0000b0, 0, 0), (0x01880014, 0, 1), (0x01880014, 0, 0), (0x01800024, 0, 0),
               (0x01800020, 0, 0), (0x01880014, 0, 2), (0x01880014, 0, 0), (0x0a0000b0, 2, 2), (0x0a0000b0, 0, 0)]
# The firmware-start image's fws_rows table, in the same form, and the
# pinned open firmware it embeds.
FWS_ROWS = [(0x01800024, 0, 0), (0x01800020, 0, 0), (0x01880014, 0, 0), (0x0a0000b0, 0, 0),
            (0x0a0000b0, 2, 1), (0x0a0000b0, 0, 0), (0x01880014, 0, 1), (0x01880014, 0, 0)]
FIRMWARE_SHA256 = '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'
# The GPU registers the start writes (pvr_gpu_start.c, 29 steps), as the
# 32-bit words the image writes: SYS_BUS_SECURE, SOFT_RESET (both words),
# SLC_CTRL_MISC, the eleven MIPS wrapper registers and AXI_ACE_LITE.
FWS_GPU_WRITES = {0xa100, 0x100, 0x104, 0x3800, 0x810, 0x818, 0x81c, 0x820, 0x824, 0x858, 0x85c, 0x860, 0x864,
                  0x828, 0x82c, 0x830, 0x834, 0x838, 0x83c, 0x840, 0x844, 0xb50, 0xb54, 0x8c8, 0x38c0, 0x38c4}
# struct step of drivers/gpu/a733_gpu_power.c: kind, bank, offset, mask, value.
STEP = struct.Struct('<5I')
# enum kind: CHECK0 UPDATE1 DELAY2 DOMAIN3 CORE_POLICY4 CORE_STATUS5 CORE_CONFIG6 POLL7 NOTE8 SETTLE9 PLL10
# LOCK11 UPDATED12.
KIND_CHECK, KIND_UPDATE, KIND_DOMAIN, KIND_NOTE, KIND_PLL, KIND_LOCK = 0, 1, 3, 8, 10, 11
MASTER_GATES = (0x5c0, 0x5e0)  # CCU AHB and MBUS master gate words
BANK_CCU, BANK_CORE = 0, 2  # enum a733_gpu_power_bank: A733_GPU_CCU, A733_GPU_R_CCU, A733_GPU_CORE
PLL_GPU0 = 0x0e0
CORE6_DELAY_ROW = (0x170, 0x174, 0xc00, 0xc04, 0xc10)
# The last PLL_GPU0 read-back of ON: enable, LDO, lock enable and output gate
# set, P0 2 and N 84 (1008 MHz), M1 1.
PLL_1008 = (KIND_PLL, BANK_CCU, PLL_GPU0, 0xe870ff02, 0xe8105300)


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


def table(elf, name, size):
    """The bytes of one static table of a linked image."""
    from elftools.elf.elffile import ELFFile
    with Path(elf).open('rb') as stream:
        image = ELFFile(stream)
        found = [s for s in image.get_section_by_name('.symtab').iter_symbols()
                 if s.name == name and s['st_info']['type'] == 'STT_OBJECT']
        require(len(found) == 1 and found[0]['st_size'] == size, f'{elf} has no single {size}-byte {name} table')
        section = image.get_section(found[0]['st_shndx'])
        return section.data()[found[0]['st_value'] - section['sh_addr']:][:size]


def embedded_firmware(elf):
    """The firmware between gpu_firmware and gpu_firmware_end of a linked image."""
    from elftools.elf.elffile import ELFFile
    with Path(elf).open('rb') as stream:
        image = ELFFile(stream)
        symbols = {s.name: s['st_value'] for s in image.get_section_by_name('.symtab').iter_symbols()
                   if s.name in ('gpu_firmware', 'gpu_firmware_end')}
        require(len(symbols) == 2, 'The image embeds no firmware.')
        for section in image.iter_sections():
            start = section['sh_addr']
            if start <= symbols['gpu_firmware'] < start + section['sh_size'] and section['sh_type'] == 'SHT_PROGBITS':
                return section.data()[symbols['gpu_firmware'] - start:symbols['gpu_firmware_end'] - start]
    raise ValueError('The firmware lies in no loaded section.')


def power_order(elf, cycles):
    """The vendor order in the image's own step tables, which every one of the client's cycles runs.
    ON (sunxi_platform_init, then sunxiPrePowerState): CORE6 PWCR 0, PLL_GPU0 at 1008 MHz and the GPU
    clocks, then the domain-5 (TOP5) ON request, then CORE6 policy 8 and its status poll. OFF
    (sunxiPostPowerState): CORE6 policy 0, then the domain-5 OFF request, then the GPU clocks and
    PLL_GPU0 off."""
    on, off = step_table(elf, 'on_steps'), step_table(elf, 'off_steps')
    on_domain = [i for i, s in enumerate(on) if s[0] == KIND_DOMAIN]
    off_domain = [i for i, s in enumerate(off) if s[0] == KIND_DOMAIN]
    require(len(on_domain) == 1 and on[on_domain[0]][4] == 1, 'ON has no single domain-5 ON step')
    require(len(off_domain) == 1 and off[off_domain[0]][4] == 0, 'OFF has no single domain-5 OFF step')
    require(on[-1][1] == BANK_CORE and on[-1][2] == 8 and on[-1][4] == 8, 'ON does not end with the CORE6 status poll')
    require(not any(s[1] == BANK_CORE and s[2] in CORE6_DELAY_ROW for s in on + off),
            'A step touches the CORE6 delay row, which no reference writes.')
    pwcr = [i for i, s in enumerate(on) if s[:3] == (KIND_UPDATE, BANK_CORE, 0x20) and s[4] == 0]
    policy8 = [i for i, s in enumerate(on) if s[:3] == (KIND_UPDATE, BANK_CORE, 0) and s[4] == 8]
    locks = [i for i, s in enumerate(on) if s[0] == KIND_LOCK and s[2] == PLL_GPU0]
    ccu_on = [i for i, s in enumerate(on) if s[1] == BANK_CCU and s[0] in (KIND_UPDATE, KIND_PLL)]
    require(len(pwcr) == 1 and ccu_on and pwcr[0] < min(ccu_on), 'ON does not write CORE6 PWCR 0 before the GPU clocks.')
    require(len(policy8) == 1 and len(locks) == 2 and PLL_1008 in on and
            max(locks) < on.index(PLL_1008) < on_domain[0] and max(ccu_on) < on_domain[0] < policy8[0],
            'ON does not set PLL_GPU0 to 1008 MHz and open the GPU clocks before domain 5 ON and CORE6 policy 8.')
    # A TOP5 OFF closed the GPU AHB master gate (board-session-20261025-7,
    # s11-gpo-run), and the MBUS gate is treated the same way: ON may only record them
    # before the domain-5 ON request and must require both after it.
    gates = [(i, s) for i, s in enumerate(on) if s[1] == BANK_CCU and s[2] in MASTER_GATES]
    require(all(s[0] == KIND_NOTE for i, s in gates if i < on_domain[0]) and
            sorted(s[2] for i, s in gates if on_domain[0] < i < policy8[0] and s[0] == KIND_CHECK) ==
            sorted(MASTER_GATES), 'ON does not require the GPU master gates after domain 5 ON alone.')
    policy0 = [i for i, s in enumerate(off) if s[:3] == (KIND_UPDATE, BANK_CORE, 0) and s[4] == 0]
    ccu_off = [i for i, s in enumerate(off) if s[1] == BANK_CCU and s[0] == KIND_UPDATE]
    pll_off = [i for i, s in enumerate(off) if s[:3] == (KIND_UPDATE, BANK_CCU, PLL_GPU0)]
    off_core = [i for i, s in enumerate(off) if s[1] == BANK_CORE]
    require(len(policy0) == 1 and ccu_off and len(pll_off) == 4 and max(off_core) < off_domain[0] < min(ccu_off),
            'OFF does not write CORE6 policy 0, then domain 5 OFF, then close the GPU clocks and PLL_GPU0.')
    return {'cycles': cycles, 'on_steps': len(on), 'off_steps': len(off), 'on_pwcr_step': pwcr[0],
            'on_pll_1008_step': on.index(PLL_1008), 'on_domain_step': on_domain[0], 'on_policy8_step': policy8[0],
            'on_gate_checks': [i for i, s in gates if s[0] == KIND_CHECK],
            'on_last_step': 'CORE6 status poll for 8', 'off_policy0_step': policy0[0],
            'off_domain_step': off_domain[0], 'off_first_clock_step': min(ccu_off)}


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
    for item in record['new_objects'] + record.get('added_objects', []):
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
                # The OFF comes only after every access, both restores included.
                require('a733_gpu_platform_off_begin' not in case['calls'] or rows == list(range(len(PROBE_STEPS))),
                        'A host-check case powered the GPU OFF before both words were restored.')
            value['probe_steps'] = [[hex(a), hex(v), k] for a, v, k in PROBE_STEPS]
        fws = 'fws_rows' in names or 'fws_access' in names
        require(fws == (variant in FWS_VARIANTS) and
                ('fws_stop_run' in names) == (variant in ('fw-stop', 'fw-job', 'fw-session')) and
                ('job_backing' in names) == (variant in ('fw-job', 'fw-session')),
                'The image\'s firmware-start or firmware-stop client does not match its variant.')
        if variant in FWS_VARIANTS:
            rows = table(output / 'image.elf', 'fws_rows', 12 * len(FWS_ROWS))
            require([struct.unpack_from('<3I', rows, 12 * i) for i in range(len(FWS_ROWS))] == FWS_ROWS,
                    'The fws_rows table is not the eight firmware-start accesses in their order.')
            firmware = embedded_firmware(output / 'image.elf')
            require(hashlib.sha256(firmware).hexdigest() == FIRMWARE_SHA256 == record['firmware']['sha256'],
                    'The image does not embed the pinned open firmware.')
            # The stop variant also kicks the kernel CCB, clears the firmware
            # interrupt and clears the thread-0 DM associations.
            allowed = FWS_GPU_WRITES | ({0xb00, 0x8b0, 0xb40, 0xb30} if variant != 'fw-start' else set())
            for case in checked['cases']:
                writes = [(a, v) for w, a, v in case['accesses'] if w]
                require(all(a in (0x0a0000b0, 0x01880014) or a - 0x01800000 in allowed for a, _ in writes),
                        'A host-check case wrote a word outside the secure configuration and the start registers.')
                kicks = [i for i, (w, a, v) in enumerate(case['accesses']) if w and a == 0x01800b00]
                # After the first kick, SOFT_RESET is written only as the last
                # step (164) of the stop without acknowledgements: the full
                # mask, low word first, upstream's final pvr_fw_stop reset.
                late = [(a, v, case['steps'][i]) for i, (w, a, v) in enumerate(case['accesses'])
                        if kicks and i > kicks[0] and w and a in (0x01800100, 0x01800104)]
                # Or as fws_shutdown's forced reset before the OFF (step 199).
                require(late in ([], [(0x01800100, 0xfffffc3d, 164), (0x01800104, 0xffefffff, 164)],
                                 [(0x01800100, 0xfffffc3d, 199), (0x01800104, 0xffefffff, 199)]),
                        'A host-check case wrote SOFT_RESET after the firmware started taking commands, other than '
                        'as the last step of the stop without acknowledgements or the forced reset.')
                # A stop's own SOFT_RESET, with or without a KICK before it.
                stop_reset = any(w and a == 0x01800100 and case['steps'][i] in (164, 199)
                                 for i, (w, a, v) in enumerate(case['accesses']))
                started = any(1 <= step <= 29 for step in case['steps'])
                secure = [(a, v) for a, v in writes if a in (0x0a0000b0, 0x01880014)]
                require(secure in ([], [(0x0a0000b0, 2), (0x01880014, 0)]),
                        'A host-check case wrote the secure configuration other than once, in the vendor order.')
                if variant == 'fw-start':
                    require(not {'a733_gpu_platform_off_begin', 'bb_power_park'} & set(case['calls']),
                            'A host-check case powered the GPU off.')
                else:
                    require('a733_gpu_platform_off_begin' not in case['calls'] or [107, 1, 1] in case['commands'] or
                            stop_reset or not started,
                            'A host-check case powered the GPU off after the firmware start without the power-off '
                            'request or a stop\'s soft reset.')
            value['fws_rows'] = [[hex(a), hex(v), k] for a, v, k in FWS_ROWS]
            value['firmware_sha256'] = FIRMWARE_SHA256
        if variant == 'identity':
            for case in checked['cases']:
                reads = [[hex(a), 4] for a in (0x01800024, 0x01800020)] * 2
                require(case['gpu_reads'] == reads[:len(case['gpu_reads'])],
                        'A host-check case read a GPU address other than the identity words in order.')
        require(variant in FWS_VARIANTS or all(not case['gpu_writes'] for case in checked['cases']),
                'A host-check case wrote the GPU window.')
        if variant == 'power-only':
            require(all(not case['gpu_reads'] for case in checked['cases']), 'A host-check case read the GPU window.')
    return value

