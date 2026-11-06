"""The regions the OS runtime gives compiled NPU graphs, computed on the host with the OS's own decoder and layout.

regions(paths) builds tools/npu_graph_layout.c with drivers/npu/nbg_model.c, nbg_assembly.c, nbg_relocation.c and
nbg_checksum.c (cached under ~/.cache/sunstoneos/host-tools by the digest of those sources) and returns, for each graph, its
read, write and constant region bytes: the input allocations; the memory pool and the output allocations; the descriptors,
coefficients and dynamic input. A graph registered after admission must fit regions of the declared tensor_bytes for its
reads and for its writes (drivers/npu/a733_npu_runtime.c), so the write region, which holds the pool, is part of that bound.
"""
import hashlib
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))
CLANG = Path(os.environ.get('SUNSTONE_HOST_CLANG', '/usr/bin/clang-21'))
SOURCES = ('tools/npu_graph_layout.c', 'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_relocation.c',
           'drivers/npu/nbg_checksum.c')
HEADERS = ('include/nbg_model.h', 'include/nbg_assembly.h')


def tool():
    digest = hashlib.sha256()
    for name in SOURCES + HEADERS:
        digest.update((ROOT / name).read_bytes() if (ROOT / name).is_file() else b'')
    binary = CACHE / 'host-tools' / ('npu-graph-layout-' + digest.hexdigest()[:16])
    if not binary.is_file():
        binary.parent.mkdir(parents=True, exist_ok=True)
        partial = binary.with_suffix('.partial')
        subprocess.run([str(CLANG), '-std=gnu11', '-O1', '-w', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'drivers/npu'),
                        *[str(ROOT / s) for s in SOURCES], '-o', str(partial)], check=True, capture_output=True)
        partial.replace(binary)
    return binary


def regions(paths):
    """[{'read': bytes, 'write': bytes, 'constant': bytes}] for each graph file, in order."""
    if not paths:
        return []
    done = subprocess.run([str(tool()), *map(str, paths)], capture_output=True, text=True)
    if done.returncode:
        raise ValueError('the OS layout refuses a graph: ' + done.stderr.strip())
    rows = []
    for line in done.stdout.splitlines():
        words = line.split()
        rows.append({words[i]: int(words[i + 1]) for i in range(0, len(words), 2)})
    if len(rows) != len(paths):
        raise ValueError('the layout tool answered %d graphs of %d' % (len(rows), len(paths)))
    return rows
