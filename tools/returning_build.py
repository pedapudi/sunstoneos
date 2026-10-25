"""Shared steps of the returning and finite image builders.

The returning and finite builders compile new units against the current tree
and link them with objects accepted from earlier evidence. Those objects keep
the structure layouts and constants of the headers they were compiled with. A
header that has changed since then gives the image two different layouts for
one type, so the builders refuse to link until the reused objects are rebuilt.

ReturningPlatform provides the platform those builders link against: either
the accepted objects of the PMIC provider probe evidence, or the same units
built from the current tree by the central CMake build.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def depfile_inputs(depfile):
    """Return the files a make-style dependency file lists: the translation unit, then its headers."""
    text = Path(depfile).read_text().replace('\\\n', ' ')
    return shlex.split(text.split(':', 1)[1])


def depfile_headers(depfile):
    """Return the files a make-style dependency file lists after its translation unit."""
    return depfile_inputs(depfile)[1:]


def evidence_root(path):
    """Return the nearest ancestor holding the evidence snapshot manifest."""
    for parent in Path(path).parents:
        if (parent / 'snapshot/manifest.json').is_file():
            return parent
    raise SystemExit(f'No evidence snapshot records the inputs of {path}')


def snapshot_records(evidence):
    """Return the snapshot's original repository and its recorded file digests."""
    manifest = json.loads((evidence / 'snapshot/manifest.json').read_text())
    records = {}
    for item in list(manifest['sources'].values()) + manifest['artifacts']:
        records[os.path.normpath(item['original'])] = (item['sha256'], evidence / 'snapshot' / item['snapshot'])
    return manifest['repository'], records


def object_closure(obj, original=None):
    """Header closure of one reused object from its dependency file and evidence snapshot.

    Assembly objects built without a dependency file are accepted only when the
    snapshot copy of their source includes no other file.
    """
    candidates = [Path(str(x) + '.d') for x in (obj, original) if x]
    depfile = next((x for x in candidates if x.is_file()), None)
    repository, records = snapshot_records(evidence_root(depfile or original or obj))
    closure = {'object': str(obj), 'roots': [repository], 'headers': {}}
    if depfile:
        for header in depfile_headers(depfile):
            closure['headers'][header] = records.get(os.path.normpath(header), (None,))[0]
        return closure
    stem = str(original or obj)[:-len('.obj')]
    sources = [(name, value) for name, value in records.items()
               if stem.endswith(name) or stem.endswith('/' + os.path.relpath(name, repository))]
    if not stem.endswith(('.S', '.s')) or len(sources) != 1:
        raise SystemExit(f'No dependency record for reused object {obj}')
    text = sources[0][1][1].read_text()
    if '#include' in text or '.include' in text:
        raise SystemExit(f'No dependency record for reused assembly object {obj}')
    return closure


def image_object_closures(image_objects, anchor):
    """Header closures recorded per object in an accepted image-objects.json, keyed by object digest.

    Each original worktree root is found from a recorded path ending in anchor,
    a repository-relative source path.
    """
    entries = json.loads(Path(image_objects).read_text())
    paths = [entry['source'] for entry in entries]
    paths += [name for entry in entries for name in entry.get('dependency_hashes') or {}]
    roots = sorted({name[:-len(anchor) - 1] for name in paths if name.endswith('/' + anchor)})
    closures = {}
    for entry in entries:
        if entry.get('dependency_hashes') is not None:
            headers = dict(entry['dependency_hashes'])
        else:
            headers = {x['original']: x['frozen']['sha256'] for x in entry['frozen_dependencies']}
        source = _current(entry['source'], roots)
        headers = {name: value for name, value in headers.items() if _current(name, roots) != source}
        closures[entry['sha256']] = {'object': entry['object'], 'roots': roots, 'headers': headers}
    return closures


def _current(path, roots, repository=ROOT):
    path = os.path.normpath(path)
    for root in roots:
        if path.startswith(root.rstrip('/') + '/'):
            return str(repository / os.path.relpath(path, root))
    return path


def _other_checkout(path, repository):
    """Return the git checkout holding path when it is not the current repository."""
    for parent in Path(path).parents:
        if parent == repository:
            return None
        if (parent / '.git').exists():
            return parent
    return None


def require_unchanged_headers(closures, compiler, repository=ROOT):
    """Refuse the build unless each reused object's headers match the current tree.

    Headers under the compiler's resource directory are part of the pinned
    compiler when no digest was recorded for them. A header left in another git
    checkout, such as a worktree no recorded root maps to the current tree, would
    be compared against that checkout and is refused. Return the checked headers
    and their digests for the build's input record.
    """
    resource = subprocess.check_output([compiler, '-print-resource-dir'], text=True).strip()
    checked = {}
    changed = {}
    for closure in closures:
        for original, recorded in closure['headers'].items():
            path = _current(original, closure['roots'], repository)
            checkout = _other_checkout(path, Path(repository))
            if checkout:
                changed.setdefault((path, f'in other checkout {checkout}'), set()).add(Path(closure['object']).name)
                continue
            current = digest(path) if Path(path).is_file() else None
            if recorded is None and current and path.startswith(resource.rstrip('/') + '/'):
                recorded = current
            if recorded is None or current != recorded:
                reason = 'unrecorded' if recorded is None else 'missing' if current is None else 'changed'
                changed.setdefault((path, reason), set()).add(Path(closure['object']).name)
            else:
                checked[path] = current
    if changed:
        lines = [f'{path} ({reason}; compiled into {", ".join(sorted(objects))})'
                 for (path, reason), objects in sorted(changed.items())]
        raise SystemExit('Refusing to link reused objects whose headers differ from the current tree:\n  '
                         + '\n  '.join(lines) + '\nRebuild those objects from current source instead.')
    return checked


def save(evidence, name, value):
    (Path(evidence) / name).write_text(json.dumps(value, indent=2) + '\n')


def run_logged(commands, evidence, cwd=None):
    """Run each command in order, writing its output to build-<index>.log."""
    for index, command in enumerate(commands):
        with (Path(evidence) / f'build-{index}.log').open('w') as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)


def copy_closure(evidence, depfiles, extra=(), directory='compiled-inputs'):
    """Copy each file named by the dependency files, plus extra, under directory.

    Return the files' digests and snapshot paths keyed by original path.
    """
    inputs = {Path(x) for x in extra}
    for depfile in depfiles:
        inputs.update(Path(x) for x in depfile_inputs(depfile))
    closure = {}
    for index, source in enumerate(sorted(inputs)):
        dest = Path(evidence) / directory / str(index) / source.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
        closure[str(source)] = {'sha256': digest(source), 'snapshot': str(dest.relative_to(evidence))}
    return closure


# The PMIC provider probe compiled these units with its own FreeRTOS
# configuration, checks/pmic-provider-probe/FreeRTOSConfig.h. Builders name
# them by their accepted object file names.
PROVIDER_UNITS = {
    '01-shared_locks': 'platform/a733/shared_locks.c',
    '02-counter_clock': 'platform/a733/counter_clock.c',
    '03-a733_clock_leases': 'drivers/clock/a733_clock_leases.c',
    '04-a733_rail_leases': 'drivers/power/a733_rail_leases.c',
    '05-a733_pmic': 'drivers/power/a733_pmic.c',
    '06-a733_pmic_native': 'drivers/power/a733_pmic_native.c',
    '07-a733_pmic_consumer': 'drivers/power/a733_pmic_consumer.c',
    '08-a733_pins': 'drivers/pinctrl/a733_pins.c',
    '09-a733_pin_tables': 'drivers/pinctrl/a733_pin_tables.c',
    '10-a733_pins_native': 'drivers/pinctrl/a733_pins_native.c',
    '11-a733_gpu_resources': 'drivers/gpu/a733_gpu_resources.c',
    '12-a733_gpu_resources_native': 'drivers/gpu/a733_gpu_resources_native.c',
    '13-tasks': 'FREERTOS/tasks.c',
    '14-queue': 'FREERTOS/queue.c',
}
# Units of the central a733-power-probe target that are not part of the
# returning platform: the probe itself and the kernel objects the provider
# configuration recompiles.
PROBE_ONLY = ('/checks/power-probe.c.obj', '/tasks.c.obj', '/queue.c.obj')
# Sources of the returning platform objects in the central a733-power-probe
# target, in link order. The kernel-check image that runs on the board links
# the same list, so a change to it must be reviewed here, not absorbed.
CENTRAL_PLATFORM = ('platform/a733/monitor.S', 'platform/a733/monitor.c', 'platform/a733/platform.c',
                    'platform/a733/cpu_coherence.S', 'arch/aarch64/cache.S', 'arch/aarch64/start.S',
                    'arch/aarch64/vectors.S', 'runtime/memory.c', 'drivers/i2c/a733_i2c.c',
                    'checks/fp-context.S', 'FREERTOS/list.c', 'freertos-port.c',
                    'FREERTOS/portable/GCC/ARM_AARCH64_SRE/portASM.S')


class ReturningPlatform:
    """The returning platform objects, compile flags and tools of one image build.

    With a baseline evidence directory, the platform is the twelve accepted
    monitor, boot and kernel objects plus the named accepted provider objects
    of the PMIC provider probe. The baseline's source snapshot and host tools
    are copied into the evidence directory and each reused object's headers
    must match the current tree. New units compile with the baseline's
    recorded flags, mapped from the recording checkout to this repository.

    With a central build directory, the units come from the current tree and
    no earlier evidence: the platform objects are those of the
    a733-power-probe target, which since the EL1 table flush also include
    platform/a733/cpu_coherence.S (CENTRAL_PLATFORM), and the provider units compile with that
    target's flags, the provider probe's FreeRTOS configuration and -Werror,
    as the provider probe built them.
    """

    def __init__(self, evidence, baseline=None, build=None):
        if (baseline is None) == (build is None):
            raise SystemExit('Select either an accepted baseline or a central build directory')
        self.evidence = Path(evidence)
        self.objects_dir = self.evidence / 'build/objects'
        self.objects_dir.mkdir(parents=True, exist_ok=True)
        self.commands = []
        self.objects = []
        self.reused = []
        self.headers = {}
        self.baseline = baseline and Path(baseline)
        self.build = build and Path(build)
        if self.baseline:
            self._accepted_baseline()
        else:
            self._central_build()

    def _accepted_baseline(self):
        base, e = self.baseline, self.evidence
        # Preserve the baseline source records, unchanged host tools and
        # runtime-hashed assembly inputs. These files provide provenance, not
        # physical admission.
        shutil.copytree(base / 'snapshot', e / 'baseline-snapshot', dirs_exist_ok=True)
        shutil.copytree(base / 'host', e / 'host', dirs_exist_ok=True)
        self.accepted = json.loads((base / 'input-identities.json').read_text())
        manifest = json.loads((e / 'baseline-snapshot/manifest.json').read_text())
        for item in list(manifest['sources'].values()) + manifest['artifacts']:
            assert digest(e / 'baseline-snapshot' / item['snapshot']) == item['sha256'], item['snapshot']
        old = json.loads((base / 'commands.json').read_text())
        recorded = old[0][:old[0].index('-MMD')]
        self.flags = [x.replace(manifest['repository'], str(ROOT)) for x in recorded]
        self.compiler, self.linker, self.objcopy = old[-2][0], old[-2][2], old[-1][0]

    def _central_build(self):
        build = self.build
        cache = (build / 'CMakeCache.txt').read_text()
        if f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n' not in cache:
            raise SystemExit(f'{build} was not configured from {ROOT}; its flags would name another checkout')
        if not (build / 'compile_commands.json').is_file():
            raise SystemExit(f'Configure {build} with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
        subprocess.run(['cmake', '--build', str(build), '--target', 'a733-power-probe'],
                       stdout=subprocess.DEVNULL, check=True)
        entries = [x for x in json.loads((build / 'compile_commands.json').read_text())
                   if '/a733-power-probe.dir/' in x['output']]
        probe = shlex.split(next(x for x in entries if x['file'].endswith('/checks/power-probe.c'))['command'])
        flags = []
        skip = 0
        for value in probe:
            if skip:
                skip -= 1
            elif value in ('-o', '-c', '-MT', '-MF'):
                skip = 1
            elif value != '-MD':
                flags.append(value)
        first = next(i for i, x in enumerate(flags) if x.startswith('-I'))
        self.flags = flags[:first] + [f'-I{ROOT}/checks/pmic-provider-probe'] + flags[first:] + ['-Werror']
        self.compiler = self.flags[0]
        link = shlex.split((build / 'CMakeFiles/a733-power-probe.dir/link.txt').read_text())
        self.linker = next(x for x in link if x.startswith('--ld-path='))
        self.objcopy = str(Path(self.compiler).resolve().parent / 'llvm-objcopy')
        self.platform_objects = [build / x for x in link if x.endswith('.obj') and not x.endswith(PROBE_ONLY)]
        self.freertos = next(Path(x['file']).parent for x in entries if x['file'].endswith('/tasks.c'))
        kernel = str(self.freertos).lstrip('/')
        names = [str(x.relative_to(build / 'CMakeFiles/a733-power-probe.dir'))[:-len('.obj')] for x in self.platform_objects]
        names = tuple('FREERTOS' + x[len(kernel):] if x.startswith(kernel + '/') else x for x in names)
        if names != CENTRAL_PLATFORM:
            raise SystemExit(f'The a733-power-probe platform objects changed: {names}')

    def replace_platform_object(self, unit, obj):
        """Link obj in place of the central build's object of one CENTRAL_PLATFORM unit."""
        if not self.build:
            raise SystemExit('Only a central platform build can replace a platform object')
        self.platform_objects[CENTRAL_PLATFORM.index(unit)] = Path(obj)

    def compile(self, sources, flags=(), linked=True, prefix='new'):
        """Compile new units with the platform flags; return their object paths."""
        objects = []
        for index, source in enumerate(sources):
            obj = self.objects_dir / f'{prefix}-{index}.obj'
            self.commands.append(self.flags + list(flags) + ['-MD', '-MF', str(obj) + '.d', '-c', str(source), '-o', str(obj)])
            objects.append(obj)
            if linked:
                self.objects.append(obj)
        return objects

    def reuse(self, names, extra=(), closures=None):
        """Add the platform objects and the named provider objects to the link.

        extra lists further accepted object records. closures maps an accepted
        object digest to its recorded header closure when the object has no
        dependency file of its own.
        """
        if self.build:
            for obj in self.platform_objects:
                self._copy(obj, digest(obj))
            units = [PROVIDER_UNITS[name] for name in names]
            self.provider_objects = self.compile([self.freertos / x[9:] if x.startswith('FREERTOS/') else ROOT / x
                                                  for x in units], prefix='provider')
            # Further accepted objects, such as the TinyUSB objects that exist
            # only in evidence, still need headers that match the current tree.
            self._reuse_accepted(list(extra), closures)
            return
        base = self.baseline
        items = json.loads((base / 'reused-objects.json').read_text())
        for name in names:
            source = base / 'build/objects' / (name + '.obj')
            pin = next(x for x in self.accepted['new_objects'] if x['path'] == str(source))
            items.append({'path': str(source), 'sha256': pin['sha256'], 'original': str(source)})
        self._reuse_accepted(items + list(extra), closures)

    def _reuse_accepted(self, items, closures):
        # Accepted objects keep the layouts of the headers they were compiled with.
        self.headers = require_unchanged_headers(
            [(closures or {}).get(x['sha256']) or object_closure(x['path'], x.get('original')) for x in items],
            self.compiler)
        for item in items:
            self._copy(Path(item['path']), item['sha256'])

    def _copy(self, source, expected):
        assert digest(source) == expected, source
        dest = self.objects_dir / f'reused-{len(self.reused)}.obj'
        shutil.copy2(source, dest)
        self.objects.append(dest)
        self.reused.append({'original': str(source), 'path': str(dest), 'sha256': digest(dest)})

    def link(self, name='probe', options=(), objects=()):
        """Append the link command of build/<name>.elf; return the command."""
        b = self.evidence / 'build'
        command = [self.compiler, '--target=aarch64-none-elf', self.linker, '-nostdlib', '-static',
                   '-Wl,--gc-sections', *options, f'-Wl,-T,{ROOT}/platform/a733/link.ld',
                   f'-Wl,-Map,{b}/{name}.map', *map(str, self.objects + list(objects)), '-o', str(b / f'{name}.elf')]
        self.commands.append(command)
        return command

    def binary(self, name='probe'):
        self.commands.append([self.objcopy, '-O', 'binary', str(self.evidence / f'build/{name}.elf'),
                              str(self.evidence / f'{name}.bin')])

    def run(self):
        """Record the commands and reused objects, then build in order."""
        save(self.evidence, 'commands.json', self.commands)
        save(self.evidence, 'reused-objects.json', self.reused)
        run_logged(self.commands, self.evidence)

    def record(self, script, extra=None):
        """Write input-identities.json: compiled closure, tools, reused headers and outputs."""
        e = self.evidence
        closure = copy_closure(e, sorted(self.objects_dir.glob('new-*.obj.d')),
                               [ROOT / 'platform/a733/link.ld', Path(script).resolve()])
        tools = {}
        for command in self.commands:
            for value in command[:3]:
                path = Path(value.split('=', 1)[-1])
                if path.is_absolute() and path.is_file() and (value.startswith('--ld-path=') or value == command[0]):
                    if self.baseline:
                        assert digest(path) == self.accepted['tools'][str(path)]['sha256'], path
                    tools[str(path)] = {'sha256': digest(path), 'resolved': str(path.resolve())}
        record = {'repository_revision_at_build': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  'new_compiler_inputs': closure, 'toolchain': tools, 'reused_object_headers': self.headers}
        if self.baseline:
            record.update(baseline_input_records_sha256=digest(self.baseline / 'input-identities.json'),
                          baseline_snapshot_manifest_sha256=digest(self.baseline / 'snapshot/manifest.json'))
            shutil.copy2(self.baseline / 'input-identities.json', e / 'baseline-input-identities.json')
        else:
            record['platform_build'] = {'directory': str(self.build),
                                        'platform_objects': {str(x): digest(x) for x in self.platform_objects},
                                        'provider_closure': copy_closure(e, [Path(str(x) + '.d') for x in self.provider_objects],
                                                                         directory='provider-inputs')}
        record.update(elf_sha256=digest(e / 'build/probe.elf'), binary_sha256=digest(e / 'probe.bin'))
        record.update(extra or {})
        record['physical_access'] = False
        save(e, 'input-identities.json', record)

    def copy_helpers(self, app, names=('check.py', 'README.md')):
        """Copy the app's checks and the ELF layout reader, checks/harness.py, beside the image."""
        for name in names:
            source = ROOT / 'apps' / app / name
            if source.exists():
                shutil.copy2(source, self.evidence / name)
        shutil.copy2(ROOT / 'checks/harness.py', self.evidence / 'layout-helper.py')


def platform_arguments(parser):
    """Add the platform selection arguments shared by the returning builders."""
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--baseline', type=Path,
                       help='accepted PMIC provider probe evidence (the default)')
    group.add_argument('--platform-build', type=Path,
                       help='central CMake build directory whose a733-power-probe target supplies the platform')


def platform_from(args, evidence):
    if args.platform_build:
        return ReturningPlatform(evidence, build=args.platform_build.resolve())
    baseline = args.baseline or Path.home() / '.cache/sunstoneos/evidence/pmic-provider-probe-20261018'
    return ReturningPlatform(evidence, baseline=baseline.resolve())
