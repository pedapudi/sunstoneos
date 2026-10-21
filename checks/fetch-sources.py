#!/usr/bin/env python3
"""Check tools/fetch-sources.py against a local file:// lock in a scratch tree."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="scratch directory for the copied tool and cache")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="fetch-sources-", dir=args.work))
    tree, upstream, cache = work / "tree", work / "upstream", work / "cache"
    (tree / "tools").mkdir(parents=True)
    upstream.mkdir()
    shutil.copy2(ROOT / "tools/fetch-sources.py", tree / "tools/fetch-sources.py")

    # Two archives whose names differ only in suffix, as the i2s .c/.h pins do.
    lock = {}
    for name, data in (("pair-c", b"int x;\n"), ("pair-h", b"extern int x;\n")):
        suffix = name[-1]
        (upstream / f"x.{suffix}").write_bytes(data)
        lock[name] = {"url": (upstream / f"x.{suffix}").as_uri(), "archive": f"pair/x.{suffix}",
                      "sha256": digest(data)}
    files = {"src/a.c": b"int a;\n", "src/a.h": b"#define A 1\n", "README": b"unpinned\n"}
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as bundle:
        for path, data in files.items():
            member = tarfile.TarInfo(f"tree-1/{path}")
            member.size = len(data)
            bundle.addfile(member, io.BytesIO(data))
    (upstream / "tree.tar.gz").write_bytes(buffer.getvalue())
    lock["tree"] = {"url": (upstream / "tree.tar.gz").as_uri(), "archive": "tree.tar.gz",
                    "directory": "tree-1", "sha256": digest(buffer.getvalue()),
                    "files": {path: digest(files[path]) for path in ("src/a.c", "src/a.h")}}
    lock["bad"] = {"url": (upstream / "x.c").as_uri(), "archive": "bad/x.c", "sha256": "0" * 64}
    (tree / "sources.lock.json").write_text(json.dumps(lock, indent=2) + "\n")

    def fetch(*names, extra=()):
        return subprocess.run([sys.executable, str(tree / "tools/fetch-sources.py"), *names,
                               "--cache", str(cache), *extra], capture_output=True, text=True)

    def start(name):
        return subprocess.Popen([sys.executable, str(tree / "tools/fetch-sources.py"), name,
                                 "--cache", str(cache)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # Concurrent fetches of colliding stems must not exchange bytes.
    for _ in range(4):
        shutil.rmtree(cache / "pair", ignore_errors=True)
        runs = [start("pair-c"), start("pair-h")]
        assert all(run.wait() == 0 for run in runs), [run.stderr.read() for run in runs]
        assert (cache / "pair/x.c").read_bytes() == b"int x;\n"
        assert (cache / "pair/x.h").read_bytes() == b"extern int x;\n"
        assert sorted(x.name for x in (cache / "pair").iterdir()) == ["x.c", "x.h"]

    # A failed download leaves neither an archive nor a temporary file.
    result = fetch("bad")
    assert result.returncode != 0 and "Hash mismatch" in result.stderr, result
    assert list((cache / "bad").iterdir()) == []

    # Extraction leaves a complete tree and a completion stamp.
    result = fetch("tree")
    directory = cache / "tree-1"
    assert result.returncode == 0 and result.stdout.strip() == str(directory), result
    assert (cache / "tree-1.sha256").read_text().strip() == lock["tree"]["sha256"]
    assert sorted(x.name for x in cache.iterdir() if x.name.startswith(".")) == []

    # An interrupted earlier extraction (tree without stamp, files missing) is repaired.
    (cache / "tree-1.sha256").unlink()
    (directory / "src/a.h").unlink()
    result = fetch("tree")
    assert result.returncode == 0, result
    assert (directory / "src/a.h").read_bytes() == files["src/a.h"]
    kept = [x for x in cache.iterdir() if x.name.startswith("tree-1.unverified-")]
    assert len(kept) == 1 and not (kept[0] / "src/a.h").exists(), kept

    # A deleted tree whose stamp remains is extracted again.
    shutil.rmtree(directory)
    for _ in range(2):
        result = fetch("tree")
        assert result.returncode == 0, result
        assert all((directory / path).read_bytes() == data for path, data in files.items())

    # An edited pinned file is refused, by a fetch and by --verify.
    (directory / "src/a.c").write_bytes(b"int a = 1;\n")
    result = fetch("tree")
    assert result.returncode != 0 and "src/a.c" in result.stderr, result
    result = fetch("tree", extra=("--verify", str(directory)))
    assert result.returncode != 0 and "src/a.c" in result.stderr, result
    (directory / "src/a.c").write_bytes(files["src/a.c"])
    result = fetch("tree", extra=("--verify", str(directory)))
    assert result.returncode == 0, result
    assert result.stdout.split() == [str(directory / path) for path in lock["tree"]["files"]], result

    # Unpinned files may differ; only the compiled files are pinned.
    (directory / "README").write_bytes(b"local note\n")
    assert fetch("tree").returncode == 0
    print(f"fetch-sources checks passed in {work}")


if __name__ == "__main__":
    main()
