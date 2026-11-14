#!/usr/bin/env python3
"""Check that build-id-inputs.txt reproduces the build id and exposes a changed input.

The check runs the identity function of cmake/sunstone-build-identity.cmake in
CMake script mode on two small files, rebuilds the identity from the record it
writes, and requires its SHA-256 to equal the identity's own. A record with one
digest changed must give a different hash. With --build DIRECTORY the check also
verifies the build-id.txt and build-id-inputs.txt of a configured image.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['cmake/sunstone-build-identity.cmake', 'cmake/sunstone-image.cmake']
DIGEST = re.compile(r'^[0-9a-f]{64}  ')
SCRIPT = '''include("{root}/cmake/sunstone-build-identity.cmake")
set(identity "header-1;name;Compiler;1.0;a;b")
set(identity_text "header-1;name;Compiler;1.0;a;b")
string(APPEND identity ";C_FLAGS=-O2;-g")
string(APPEND identity_text "\\nC_FLAGS=-O2;-g")
sunstone_identity_digests(identity identity_text "{work}/one.c" "{work}/two.h")
string(SHA256 build_id "${{identity}}")
file(WRITE "{work}/build-id.txt" "${{build_id}}\\n")
file(WRITE "{work}/build-id-inputs.txt" "${{identity_text}}\\n")
'''


def rebuild(record):
    """The identity that a record describes: text components joined by ';', then the digests."""
    lines = record.rstrip("\n").split("\n")
    text = [x for x in lines if not DIGEST.match(x)]
    digests = [x[:64] for x in lines if DIGEST.match(x)]
    assert lines[:len(text)] == text, "text lines must come before the digest lines"
    return ";".join(text) + "".join(digests)


def sha256(text):
    return hashlib.sha256(text.encode()).hexdigest()


def verify(directory, label):
    recorded = (directory / "build-id.txt").read_text().strip()
    record = (directory / "build-id-inputs.txt").read_text()
    assert sha256(rebuild(record)) == recorded, f"{label}: the record does not reproduce the build id"
    lines = record.rstrip("\n").split("\n")
    digest = next(i for i, x in enumerate(lines) if DIGEST.match(x))
    lines[digest] = ("0" if lines[digest][0] != "0" else "1") + lines[digest][1:]
    assert sha256(rebuild("\n".join(lines))) != recorded, f"{label}: a changed digest left the hash unchanged"
    return recorded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, action="append", default=[], help="configured image directory to verify")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="build-id-inputs-") as name:
        work = Path(name)
        (work / "one.c").write_text("int one;\n")
        (work / "two.h").write_text("#define TWO 2\n")
        (work / "run.cmake").write_text(SCRIPT.format(root=ROOT, work=work))
        subprocess.run(["cmake", "-P", str(work / "run.cmake")], check=True)
        record = (work / "build-id-inputs.txt").read_text()
        assert record.count("\n") == 4 and f"  {work}/one.c" in record, record
        verify(work, "fixture")
        # Changing an input file changes the recorded digest and the id.
        before = (work / "build-id.txt").read_text()
        (work / "two.h").write_text("#define TWO 3\n")
        subprocess.run(["cmake", "-P", str(work / "run.cmake")], check=True)
        assert (work / "build-id.txt").read_text() != before
    for directory in args.build:
        verify(directory, str(directory))
    print("build-id-inputs checks passed")


if __name__ == "__main__":
    main()
