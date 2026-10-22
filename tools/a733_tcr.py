"""Read the EL1 translation-control value from include/a733_cpu_coherence.h.

Host checks that model the current images compare TCR_EL1 with TCR. Checks of
frozen, digest-pinned images keep the value those images were built with.
"""
from pathlib import Path
import re

HEADER = Path(__file__).resolve().parents[1] / 'include/a733_cpu_coherence.h'
DEFINITION = re.compile(r'^#define A733_CPU_COHERENCE_TCR A733_CPU_COHERENCE_U64\((0x[0-9a-fA-F]+)\)$', re.M)


def read(header=HEADER):
    found = DEFINITION.findall(Path(header).read_text())
    if len(found) != 1:
        raise ValueError(f'{header}: expected one A733_CPU_COHERENCE_TCR definition, found {len(found)}')
    return int(found[0], 16)


TCR = read()
