#!/usr/bin/env python3
"""Check that repeated run directories preserve earlier evidence."""
import importlib.util
from pathlib import Path
import tempfile

import harness

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('host_evidence', ROOT / 'tools/host_evidence.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)

with tempfile.TemporaryDirectory(prefix='sunstone-evidence-check-') as temporary:
    requested = Path(temporary) / 'runs'
    first = evidence.unique_directory(requested)
    assert first == requested
    (first / 'session.json').write_text('earlier result\n')
    second = evidence.unique_directory(requested)
    third = evidence.unique_directory(requested)
    assert second.parent == requested and third.parent == requested and second != third
    assert (first / 'session.json').read_text() == 'earlier result\n'
    assert not list(second.iterdir()) and not list(third.iterdir())
    artifact = evidence.new_directory(Path(temporary) / 'bundle')
    (artifact / 'grant.bin').write_bytes(b'version one')
    try:
        evidence.new_directory(artifact)
    except FileExistsError:
        pass
    else:
        raise AssertionError('A consumed artifact path cannot redirect into a child.')
    assert list(artifact.iterdir()) == [artifact / 'grant.bin']
    assert (artifact / 'grant.bin').read_bytes() == b'version one'
    file = Path(temporary) / 'file'
    file.write_text('preserve\n')
    try:
        evidence.unique_directory(file)
    except FileExistsError:
        pass
    else:
        raise AssertionError('A regular file cannot contain evidence.')
    assert file.read_text() == 'preserve\n'

print('PASS: repeated run directories preserve evidence and receive unique paths.')
