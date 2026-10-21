#!/usr/bin/env python3
"""Preserve selected evidence and inputs before another build overwrites them."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import shlex
from pathlib import Path
import subprocess


def git(root, *arguments):
    return subprocess.check_output(["git", "-C", str(root), *arguments], text=True)


def copy_file(source, destination):
    """Hash the copied bytes; refuse a source that changes during the copy."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        before = os.fstat(incoming.fileno())
        while block := incoming.read(1024 * 1024):
            outgoing.write(block)
            digest.update(block)
        after = os.fstat(incoming.fileno())
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    if identity(before) != identity(after) or identity(after) != identity(source.stat()):
        raise RuntimeError(f"Source changed during snapshot: {source}")
    return {"original": str(source), "bytes": before.st_size,
            "sha256": digest.hexdigest()}


def collect_dependencies(build,target):
    """Recover every object actually linked into this image, including ASM."""
    import re
    build=Path(build).resolve()
    cmake_root=next((p for p in (build,*build.parents) if (p/'CMakeCache.txt').is_file()),build)
    link=build/f'CMakeFiles/{target}.dir/link.txt'
    objects=[(build/name).resolve() for name in shlex.split(link.read_text()) if name.endswith(('.obj','.o'))]
    commands=json.loads((cmake_root/'compile_commands.json').read_text())
    # CMake's output field can be top-build-relative even when directory is a
    # subdirectory. The actual compiler -o argument uses the recorded cwd.
    by_object={}
    for command in commands:
        args=command.get('arguments') or shlex.split(command['command'])
        output=args[args.index('-o')+1] if '-o' in args else command['output']
        by_object[(Path(command['directory'])/output).resolve()]=command
    commands=by_object
    errors=[];dependencies={};actual=[];records={};asm={}
    def internal_record(obj):
        directory=next(p for p in obj.parents if p.name.endswith('.dir') and p.parent.name=='CMakeFiles')
        return directory/'depend.internal'
    for obj in objects:
        record=internal_record(obj)
        if record in asm:continue
        entries={};current=None
        if record.is_file():
            for line in record.read_text().splitlines():
                if not line or line.startswith('#'):continue
                if not line.startswith(' '):current=(cmake_root/line).resolve();entries[current]=[]
                elif current is not None:entries[current].append((cmake_root/line.strip()).resolve())
        asm[record]=entries
    for obj in objects:
        command=commands.get(obj)
        if not obj.is_file() or command is None:
            errors.append('Missing linked object/command: '+str(obj));continue
        actual.append(command);cwd=Path(command['directory'])
        source=(cwd/command['file']).resolve();dep=Path(str(obj)+'.d')
        if dep.is_file():
            names=shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]);paths=[(cwd/n).resolve() for n in names]
        else:
            dep=internal_record(obj)
            paths=asm[dep].get(obj,[])
            if source.suffix!='.S' or not paths:
                errors.append('Missing compiler dependency record: '+str(obj));continue
        if source not in paths:errors.append('Dependency record omits source: '+str(source))
        # The generated fixed assembly embeds two immutable binary inputs.
        if source.suffix=='.S':
            paths+= [(cwd/name).resolve() for name in re.findall(r'^\s*\.incbin\s+"([^"]+)"',source.read_text(),re.M)]
        for path in paths:
            if not path.is_file():errors.append('Missing dependency: '+str(path))
            else:dependencies[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
        records[str(obj)]={'record':str(dep),'record_sha256':hashlib.sha256(dep.read_bytes()).hexdigest(),'source':str(source)}
    if not objects or not dependencies:errors.append('Empty linked-object/dependency discovery')
    if len(records)!=len(objects):errors.append('Incomplete linked-object dependency coverage')
    if errors:raise RuntimeError(target+' dependency setup failed:\n'+'\n'.join(errors))
    return dependencies,actual,records

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True,
                        help="New directory outside the repository; never overwritten")
    parser.add_argument("--source", action="append", default=[],
                        help="Repository-relative source/configuration/fixture file; repeat as needed")
    parser.add_argument("--artifact", type=Path, action="append", default=[],
                        help="Executable, image, log or result file to preserve; repeat as needed")
    parser.add_argument("--tool", type=Path, action="append", default=[],
                        help="Tool executable to preserve and hash; does not execute it")
    parser.add_argument("--note", required=True,
                        help="Describe the recorded check and any evidence limitations")
    args = parser.parse_args()
    root = Path(git(args.repository.resolve(), "rev-parse", "--show-toplevel").strip()).resolve()
    output = args.output.resolve()
    if output == root or root in output.parents:
        parser.error("Evidence output must be outside the repository")
    if not args.source or not args.artifact:
        parser.error("Supply at least one --source and one --artifact")

    sources = []
    for name in args.source:
        path = (root / name).resolve()
        if root not in path.parents or not path.is_file():
            parser.error(f"Source must resolve to a file inside the repository: {name}")
        sources.append(path)
    artifacts = [p.resolve() for p in args.artifact]
    tools = [p.resolve() for p in args.tool]
    for path in artifacts + tools:
        if not path.is_file():
            parser.error(f"Not a file: {path}")
        if path == output or output in path.parents:
            parser.error("Snapshot inputs cannot be inside its output directory")

    revision = git(root, "rev-parse", "HEAD").strip()
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all")
    output.mkdir(parents=True, exist_ok=False)
    records = {"sources": {}, "artifacts": [], "tools": []}
    for path in dict.fromkeys(sources):
        relative = path.relative_to(root)
        destination = output / "sources" / relative
        row = copy_file(path, destination)
        row["snapshot"] = str(destination.relative_to(output))
        records["sources"][str(relative)] = row
    for category, paths in [("artifacts", artifacts), ("tools", tools)]:
        for index, path in enumerate(dict.fromkeys(paths)):
            destination = output / category / str(index) / path.name
            row = copy_file(path, destination)
            row["snapshot"] = str(destination.relative_to(output))
            records[category].append(row)
    if git(root, "rev-parse", "HEAD").strip() != revision:
        raise RuntimeError("Repository revision changed during snapshot; no manifest published")
    manifest = {
        "schema": 1,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "repository": str(root), "revision": revision,
        "worktree_status_before_copy": status,
        "note": args.note,
        "scope": "Selected file preservation only; does not establish execution, passing checks, or complete dependency coverage",
        **records,
    }
    with (output / "manifest.json").open("x") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    print(output / "manifest.json")


if __name__ == "__main__":
    main()
