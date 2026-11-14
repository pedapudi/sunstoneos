"""The source files an image recipe of tools/gate_images.py compiles, read from CMake's own build graph.

checks/check-index.py uses this to cover the sources of the model images that a check builds, so that a change to a file
a model links selects the check without a hand-kept list. The graph comes from the CMake file API: the recipe is configured
once with the same host script the gate uses (~/.cache/sunstoneos/bin/cfg.sh) and the code model's target sources and
dependencies are read back. The result is cached by the content of every CMake file the configure can read, the recipe's
flags and targets, so a change to a source file never repeats the configure and a change to a CMake file does.

Images whose target is an integrated OS image (sunstone-a733-os*) are not expanded: every affected gate builds the supervised OS
baseline, and the OS sources would otherwise select every check that names an OS image for any driver change.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_images

CACHE = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))
GRAPHS = CACHE / 'index-graph'
OS_TARGET_PREFIX = 'sunstone-a733-os'
SOURCE_SUFFIXES = ('.c', '.cc', '.cpp', '.S', '.s')


def flags(recipe, source):
    return [value.replace('{source}', str(source)).replace('{cache}', str(CACHE)) for value in recipe['configure']]


def key(cmake_texts, recipe):
    digest = hashlib.sha256()
    for name in sorted(cmake_texts):
        digest.update(name.encode() + b'\0' + cmake_texts[name] + b'\0')
    digest.update(json.dumps([recipe['configure'], recipe['target']], sort_keys=True).encode())
    return digest.hexdigest()[:24]


def read_graph(build, source, targets):
    """The repository-relative source paths of the targets and every target they depend on."""
    reply = build / '.cmake/api/v1/reply'
    index = json.loads(next(reply.glob('index-*.json')).read_text())
    model = next(r for r in index['objects'] if r['kind'] == 'codemodel')
    code_model = json.loads((reply / model['jsonFile']).read_text())
    project_root = Path(code_model['paths']['source'])     # a standalone project's source paths are relative to it
    configuration = code_model['configurations'][0]
    by_id = {t['id']: json.loads((reply / t['jsonFile']).read_text()) for t in configuration['targets']}
    by_name = {t['name']: tid for tid, t in by_id.items()}
    missing = [t for t in targets if t not in by_name]
    if missing:
        raise ValueError('target not in the build graph: ' + ', '.join(missing))
    seen, stack, paths = set(), [by_name[t] for t in targets], set()
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        target = by_id[current]
        if target['name'].startswith(OS_TARGET_PREFIX):
            return None
        for entry in target.get('sources', ()):
            path = Path(os.path.normpath(project_root / entry['path']))
            if path.is_relative_to(source) and path.suffix in SOURCE_SUFFIXES:
                paths.add(path.relative_to(source).as_posix())
        stack.extend(d['id'] for d in target.get('dependencies', ()))
    return sorted(paths)


def configure_command(source, recipe, build, cache=CACHE):
    """The command that configures a recipe in build: cfg.sh for the top-level project, cmake alone for a standalone project."""
    if 'project' in recipe:
        extra = [value.replace('{source}', str(source)) for value in gate_images.project_flags(cache)]
        return ['cmake', '-S', str(source / recipe['project']), '-B', str(build), *extra, *flags(recipe, source)]
    python = cache / 'venv/bin/python'
    return [str(cache / 'bin/cfg.sh'), str(source), str(build), '-DPython3_EXECUTABLE=' + str(python),
            '-DSUNSTONE_EXTERNAL_APPLICATIONS=', '-DSUNSTONE_A733_OS=ON', *flags(recipe, source)]


def configure(source, recipe, build):
    query = build / '.cmake/api/v1/query'
    query.mkdir(parents=True, exist_ok=True)
    (query / 'codemodel-v2').write_text('')
    environment = dict(os.environ, PATH=str(CACHE / 'host/usr/lib/llvm-21/bin') + os.pathsep + os.environ.get('PATH', ''))
    subprocess.run(configure_command(source, recipe, build), check=True, capture_output=True, text=True, env=environment)


def image_sources(source, recipe, cmake_texts, extract=None):
    """The compiled source paths of a recipe, or None when its target is an OS image.

    source is the checkout the paths are relative to; extract, when the checkout is a committed revision rather than the
    working tree, is a function that writes that revision to a directory and returns it."""
    targets = [recipe['target']] if isinstance(recipe['target'], str) else list(recipe['target'])
    if all(t.startswith(OS_TARGET_PREFIX) for t in targets):
        return None
    name = key(cmake_texts, recipe)
    cached = GRAPHS / (name + '.json')
    if cached.is_file():
        return json.loads(cached.read_text())['sources']
    GRAPHS.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=GRAPHS, prefix='work-') as work:
        tree = Path(extract(Path(work) / 'tree')) if extract else source
        build = Path(work) / 'build'
        configure(tree, recipe, build)
        paths = read_graph(build, tree, targets)
    if paths is None:
        return None
    temporary = cached.with_suffix('.tmp%d' % os.getpid())
    temporary.write_text(json.dumps({'sources': paths}, indent=1))
    temporary.replace(cached)
    return paths
