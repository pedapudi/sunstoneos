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
SELECTION = '''#ifdef SUNSTONE_CACHED_SINGLE_CORE
#define A733_CPU_COHERENCE_TCR A733_CPU_COHERENCE_TCR_CACHED_WALK
#else
#define A733_CPU_COHERENCE_TCR A733_CPU_COHERENCE_TCR_UNCACHED_WALK
#endif
'''


def read(header=HEADER):
    """Return (single-core value, eight-core value)."""
    text = Path(header).read_text()
    values = []
    for name in ('CACHED_WALK', 'UNCACHED_WALK'):
        found = re.findall(rf'^#define A733_CPU_COHERENCE_TCR_{name} A733_CPU_COHERENCE_U64\((0x[0-9a-fA-F]+)\)$',
                           text, re.M)
        if len(found) != 1:
            raise ValueError(f'{header}: expected one A733_CPU_COHERENCE_TCR_{name} definition, found {len(found)}')
        values.append(int(found[0], 16))
    if text.count(SELECTION) != 1 or len(re.findall(r'^#define A733_CPU_COHERENCE_TCR ', text, re.M)) != 2:
        raise ValueError(f'{header}: A733_CPU_COHERENCE_TCR must be selected by SUNSTONE_CACHED_SINGLE_CORE alone')
    return tuple(values)


SINGLE_CORE, EIGHT_CORE = read()
