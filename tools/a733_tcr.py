"""Read the EL1 translation-control values from include/a733_cpu_coherence.h.

SINGLE_CORE is the value of the cached single-core images, which walk the EL1
tables through the caches. EIGHT_CORE is the value of the cached eight-core
images, which keep noncacheable walks until a Cortex-A76 walk measurement
admits the cached value. Checks of frozen, digest-pinned images keep the value
those images were built with.
"""
from pathlib import Path
import re

HEADER = Path(__file__).resolve().parents[1] / 'include/a733_cpu_coherence.h'
SELECTION = '''#if defined(SUNSTONE_CACHED_SINGLE_CORE) || SUNSTONE_A733_CACHEABLE_TABLE_WALKS
#define A733_CPU_COHERENCE_TCR A733_CPU_COHERENCE_TCR_CACHED_WALK
#else
#define A733_CPU_COHERENCE_TCR A733_CPU_COHERENCE_TCR_UNCACHED_WALK
#endif
'''


def _profiles(header):
    text = Path(header).read_text()
    values = []
    for name in ('CACHED_WALK', 'UNCACHED_WALK'):
        found = re.findall(rf'^#define A733_CPU_COHERENCE_TCR_{name} A733_CPU_COHERENCE_U64\((0x[0-9a-fA-F]+)\)$',
                           text, re.M)
        if len(found) != 1:
            raise ValueError(f'{header}: expected one A733_CPU_COHERENCE_TCR_{name} definition, found {len(found)}')
        values.append(int(found[0], 16))
    default = re.findall(r'^#define SUNSTONE_A733_CACHEABLE_TABLE_WALKS ([01])$', text, re.M)
    if text.count(SELECTION) != 1 or len(default) != 1 or len(re.findall(r'^#define A733_CPU_COHERENCE_TCR ', text, re.M)) != 2:
        raise ValueError(f'{header}: unrecognized A733_CPU_COHERENCE_TCR selection')
    return values, default[0] == '1'


def read(header=HEADER):
    """Return the single-core and eight-core defaults of the source header."""
    values, cached = _profiles(header)
    return values[0], values[0] if cached else values[1]


(CACHED_WALK, UNCACHED_WALK), _cached = _profiles(HEADER)
SINGLE_CORE, EIGHT_CORE = CACHED_WALK, CACHED_WALK if _cached else UNCACHED_WALK


def for_image(path):
    """Read the effective profile exported by the image, including build overrides."""
    from elftools.elf.elffile import ELFFile
    with Path(path).open('rb') as stream:
        table = ELFFile(stream).get_section_by_name('.symtab')
        found = table.get_symbol_by_name('a733_cpu_coherence_tcr') if table else None
        if not found or len(found) != 1 or found[0]['st_shndx'] != 'SHN_ABS':
            raise ValueError(f'{path}: missing absolute EL1 walk profile')
        value = found[0]['st_value']
    if value not in (CACHED_WALK, UNCACHED_WALK):
        raise ValueError(f'{path}: unrecognized EL1 walk profile {value:#x}')
    return value
