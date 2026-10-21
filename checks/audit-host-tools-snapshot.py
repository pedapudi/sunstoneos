#!/usr/bin/env python3
"""Check linked-dependency collection over archives and linker inputs with a fixture tree.

The fixture is a synthetic CMake build directory; no compiler or board runs.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audit_snapshot', ROOT / 'tools/evidence-snapshot.py')
snapshot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snapshot)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def fixture(root):
    build, source, external = root / 'build', root / 'source', root / 'toolchain'
    write(build / 'CMakeCache.txt', '# fixture\n')
    commands = []
    for target, name in [('app', 'main.c'), ('inner', 'inner.cc')]:
        src = write(source / name, f'/* {name} */\n')
        header = write(source / (name + '.h'), '/* header */\n')
        obj = build / f'CMakeFiles/{target}.dir/{name}.obj'
        write(obj, 'object')
        write(Path(str(obj) + '.d'), f'{obj.name}: {src} {header}\n')
        commands.append({'directory': str(build), 'file': str(src),
                         'command': f'clang -c {src} -o CMakeFiles/{target}.dir/{name}.obj'})
    write(build / 'compile_commands.json', json.dumps(commands))
    write(build / 'libinner.a', 'archive')
    write(build / 'CMakeFiles/inner.dir/link.txt',
          '/usr/bin/llvm-ar-21 qc libinner.a "CMakeFiles/inner.dir/inner.cc.obj"\n/usr/bin/llvm-ranlib-21 libinner.a\n')
    libc = write(external / 'libc.a', 'prebuilt')
    script = write(build / 'image/link.ld', 'SECTIONS {}\n')
    line = (f'/usr/bin/clang++-21 --target=aarch64-none-elf --ld-path=/usr/bin/ld.lld -Wl,--gc-sections -nostdlib '
            f'-static -Wl,--defsym=__heap=4096 -Wl,-T,{script} -Wl,--wrap=_impure_ptr -Wl,-Map,{build}/image.map '
            f'"CMakeFiles/app.dir/main.c.obj" -o app.elf libinner.a {libc}')
    link = write(build / 'CMakeFiles/app.dir/link.txt', line + '\n')
    return build, source, script, libc, link


def refuses(build, text):
    try:
        snapshot.collect_dependencies(build, 'app')
    except RuntimeError as error:
        assert text in str(error), error
        return
    raise AssertionError('Accepted: ' + text)


def main():
    with tempfile.TemporaryDirectory(prefix='audit-host-tools-snapshot-') as directory:
        build, source, script, libc, link = fixture(Path(directory).resolve())
        dependencies, commands, records = snapshot.collect_dependencies(build, 'app')
        expected = [source / 'main.c', source / 'inner.cc', source / 'inner.cc.h', script, libc, link,
                    build / 'libinner.a', build / 'CMakeFiles/inner.dir/link.txt']
        for path in expected:
            assert dependencies[str(path)] == hashlib.sha256(path.read_bytes()).hexdigest(), path
        assert len(commands) == len(records) == 2
        assert str(build / 'CMakeFiles/inner.dir/inner.cc.obj') in records
        original = link.read_text()
        link.write_text(original.replace(' -o app.elf', ' -lunreviewed -o app.elf'))
        refuses(build, 'Unhandled link token')
        link.write_text(original.replace(' libinner.a', ' libmissing.a'))
        refuses(build, 'In-tree archive has no CMake record')
        link.write_text(original + 'objcopy app.elf app.bin\n')
        refuses(build, 'Unhandled link record commands')
        link.write_text(original)
        (source / 'inner.cc').unlink()
        refuses(build, 'Missing dependency')
    print('PASS: in-tree archive members, external archives, linker script and link records are covered; '
          'unknown link tokens and missing records refuse.')


if __name__ == '__main__':
    main()
