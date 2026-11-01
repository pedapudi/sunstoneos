"""Exact final-retirement review and copied report for returning GPU images."""
import struct

CONDITIONS = {'output-lending-retired', 'firmware-off-isolation-and-resources',
              'gpu-worker-and-pmic-sample-parked', 'spi95-restoration',
              'local-pools-and-read-only-cache-retirement'}
MAGIC, BYTES = 0x3152475033333741, 64
CONSTRUCTOR_STAGES = (
    None, 'previous GPU-loss record', 'owner identities', 'queue client binding',
    'native platform constructor', 'platform constructor', 'PMIC lifetime',
    'PMIC binding', 'performance selection', 'firmware memory', 'queue memory',
    'native backend binding', 'service construction', 'application client binding',
    'thermal client binding')
BISECT_STAGES = (None, 'constructor', 'firmware', 'jobs', 'layer')


def validate(review, config, evidence, require):
    value = review.get('gpu_retirement')
    require(isinstance(value, dict) and value.get('configuration') == config and
            isinstance(value.get('conditions'), dict) and set(value['conditions']) == CONDITIONS,
            'Exact GPU final-retirement review required.')
    for item in value['conditions'].values():
        evidence(item)


def decode(raw, epoch, generation, require, allow_untouched=False):
    require(len(raw) == BYTES, 'Incomplete GPU retirement report.')
    magic, version, size, actual, gen, jobs, flags, uv = struct.unpack('<8Q', raw)
    if version == 2:
        require(allow_untouched and (magic, size, actual, gen, jobs, flags) ==
                (MAGIC, BYTES, epoch, generation, 0, 0) and 0 <= uv <= 14,
                'GPU untouched return is incomplete or not admitted by this image.')
        # Reason zero covers skipped or successful construction without OPEN.
        if uv == 0:
            return {'jobs': 0, 'gpu_untouched': True, 'device_never_opened': True,
                    'qualification_passed': False}
        return {'jobs': 0, 'gpu_untouched': True, 'constructor_refusal': uv,
                'constructor_stage': CONSTRUCTOR_STAGES[uv],
                'qualification_passed': False}
    if version == 3:
        # A bisect image (SUNSTONE_A733_OS_GPU_BISECT) stopped GPU use at a
        # build-time limit. Only those images write version 3; they also carry
        # the untouched-return marker that admits version 2.
        stage, limit = uv >> 32, uv & 0xffffffff
        require(allow_untouched and (magic, size, actual, gen) == (MAGIC, BYTES, epoch, generation) and
                1 <= stage <= 4 and flags == (0 if stage == 1 else 63) and
                (stage != 1 or jobs == 0) and jobs < 0xffffffff,
                'GPU bisect retirement is incomplete or belongs to another session.')
        return {'jobs': jobs, 'gpu_bisect': {'stage': BISECT_STAGES[stage], 'limit': limit},
                'gpu_off': flags == 63, 'gpu_never_powered': stage == 1,
                'worker_parked': flags == 63, 'qualification_passed': False}
    require((magic, version, size, actual, gen, flags, uv) ==
            (MAGIC, 1, BYTES, epoch, generation, 63, 960000) and 0 <= jobs < 0xffffffff,
            'GPU retirement is incomplete or belongs to another session.')
    return {'jobs': jobs, 'gpu_off': True, 'worker_parked': True,
            'pmic_sample_retired': True, 'dcdc4_uv': uv}


def geometry(image):
    """Bind GPU receipt capture and diagnostic policy to immutable ELF data."""
    from pathlib import Path
    from elftools.elf.elffile import ELFFile
    def require(ok, message):
        if not ok:
            raise ValueError(message)
    with Path(image).open('rb') as stream:
        elf = ELFFile(stream)
        table = elf.get_section_by_name('.symtab')
        require(table is not None, 'GPU return image has no symbols.')
        names = ('os_gpu_return_report', 'os_gpu_untouched_return_version',
                 'os_gpu_bisect_configuration', 'os_gpu_performance_configuration')
        symbols = {}
        for symbol in table.iter_symbols():
            if symbol.name in names and symbol['st_shndx'] != 'SHN_UNDEF':
                require(symbol.name not in symbols, 'Ambiguous GPU return symbol.')
                symbols[symbol.name] = symbol
        if not symbols:
            return None
        require('os_gpu_return_report' in symbols, 'GPU return report missing.')
        def object_data(name, size, writable):
            symbol = symbols[name]
            index, address = symbol['st_shndx'], int(symbol['st_value'])
            require(type(index) is int and symbol['st_info']['type'] == 'STT_OBJECT' and
                    symbol['st_info']['bind'] == 'STB_GLOBAL' and symbol['st_size'] == size,
                    'GPU return object geometry differs: ' + name)
            section = elf.get_section(index)
            require(section['sh_flags'] & 3 == (3 if writable else 2) and
                    not section['sh_flags'] & 4 and
                    section['sh_addr'] <= address < address + size <= section['sh_addr'] + section['sh_size'],
                    'GPU return object allocation differs: ' + name)
            require(any(segment['p_type'] == 'PT_LOAD' and not segment['p_flags'] & 1 and
                        bool(segment['p_flags'] & 2) == writable and
                        segment['p_vaddr'] <= address < address + size <=
                        segment['p_vaddr'] + (segment['p_memsz'] if writable else segment['p_filesz'])
                        for segment in elf.iter_segments()), 'GPU return object has no matching load segment.')
            if writable:
                require(section['sh_type'] == 'SHT_NOBITS' and address % 8 == 0 and
                        0x41200000 <= address < address + size <= 0x41c00000,
                        'GPU return report is outside writable image BSS.')
                return address
            require(section['sh_type'] == 'SHT_PROGBITS', 'GPU capability marker has no immutable bytes.')
            offset = address - section['sh_addr']
            return section.data()[offset:offset + size]
        result = {'address': object_data('os_gpu_return_report', BYTES, True),
                  'bytes': BYTES, 'allow_untouched': False}
        if 'os_gpu_untouched_return_version' in symbols:
            require(struct.unpack('<I', object_data('os_gpu_untouched_return_version', 4, False)) == (2,),
                    'GPU untouched capability marker differs.')
            result['allow_untouched'] = True
        if 'os_gpu_bisect_configuration' in symbols:
            version, stage, limit = struct.unpack('<3I', object_data('os_gpu_bisect_configuration', 12, False))
            require(result['allow_untouched'] and version == 1 and 1 <= stage <= 4 and
                    (limit == 0 if stage <= 2 else limit > 0 if stage == 3 else limit < 255),
                    'GPU diagnostic capability marker differs.')
            result['bisect'] = {'stage': stage, 'limit': limit}
        if 'os_gpu_performance_configuration' in symbols:
            config = struct.unpack('<5I', object_data('os_gpu_performance_configuration', 20, False))
            require(config == (1, 1008000000, 960000, 600000000, 800000),
                    'GPU performance capability marker differs.')
            result['performance'] = list(config)
        return result


def decode_captured(raw, layout, epoch, generation):
    """Decode a copied receipt under the exact frozen image's capabilities."""
    def require(ok, message):
        if not ok:
            raise ValueError(message)
    result = decode(raw, epoch, generation, require, allow_untouched=layout['allow_untouched'])
    version = struct.unpack_from('<Q', raw, 8)[0]
    diagnostic = layout.get('bisect')
    if version == 3:
        packed = struct.unpack_from('<Q', raw, 56)[0]
        require(diagnostic == {'stage': packed >> 32, 'limit': packed & 0xffffffff},
                'GPU diagnostic receipt differs from the image stage or limit.')
        # This word is the final queue sequence; refused submissions may leave
        # gaps, so a jobs-stage success cap does not bound its value.
        require(diagnostic['stage'] > 2 or result['jobs'] == 0,
                'GPU constructor/firmware diagnostic submitted jobs.')
    elif version == 1:
        require(diagnostic is None, 'GPU diagnostic image returned a normal GPU receipt.')
    result['qualification_passed'] = False
    result['completed_gpu_work'] = version == 1 and result['jobs'] > 0
    return result
