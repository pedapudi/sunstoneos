#!/usr/bin/env python3
"""Build the OS image, the data regions and the session of an application that runs compiled NPU graphs.

The OS image links no graph: the application registers each compiled graph at run time (ss_npu_register_graph) from an input
region. This tool makes everything a board run of such an application needs from three inputs, so that a run is reproducible
from the repository and these inputs:

  --app DIR      the application's source directory: the manifest template (manifest.json, or --manifest), host.py (the result decoder) and its CMake
                 application file. Its repository stays with its owner; the tool only reads it.
  --graphs DIR   artifacts.json (name -> {model, inputs, references} files) and list.json ({"npu_models": [names]}): the compiled
                 graphs in the order the application registers them. They come from the application's own graph pipeline.
  --data PATH    the application's second input region, as a file or a directory that holds vectors.region.

It writes, under --out:

  pack/models.region   the compiled graphs, led by an index page (tools/npu-external-pack.py): "NMDL", version, count, then the
                       offset and length of each graph
  pack/vectors.region  a copy of --data
  manifest.json        the application's manifest with both regions' addresses, sizes and SHA-256 filled in and the NPU
                       declaration derived from the graphs: slots and graphs are the count, graph_bytes the largest graph
                       rounded up to whole pages, and tensor_bytes the largest region the OS layout gives a graph's reads
                       (its inputs) or writes (its memory pool and outputs), from the OS's own decoder and layout
                       (tools/npu_graph_layout.py). The same bound serves a graph sealed at admission and one registered later
  image/               the OS image (cfg.sh and CMake, as the integration gate builds), unless --no-build
  session/             tools/session.py build for that image, unless --no-build
  decoder-frozen.py    with --freeze-decoder: the decoder bound to this image (see below); the session and the submit line use it
  recipe.txt           the commands, to repeat the build and to submit

An application whose decoder must be bound to the image (its host.py offers --freeze-image ELF --decoder-output FILE, such as
apps/cpu-npu-concurrency: the decode needs the mode and the linked reference) is built with --freeze-decoder 'ARGS', the
arguments that bind it (for example '--mode 1'). The tool then runs host.py --freeze-image after the build, builds the session
from the frozen decoder and puts it on the submit line. It refuses such an application without the option, because the unfrozen
decoder fails at the runner's decode (host-error at restore-rails) after the board has run.

Example:

  tools/npu-app-image.py --app ~/git/mimus-worktrees/npu-encoder-port-1103/sunstoneos/apps/npu-encoder \\
      --graphs work/tE16 --data work/data/enc512/vectors.region --out build/encoder
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PAGE = 4096
CACHE = Path(os.environ.get('SUNSTONE_CACHE', Path.home() / '.cache/sunstoneos'))


def align(n):
    return -(-n // PAGE) * PAGE


def region_entry(manifest, name):
    for r in manifest['regions']:
        if r['name'] == name:
            return r
    raise SystemExit('the application manifest has no region named ' + name)


def graph_bounds(artifacts, names):
    """(graph_bytes, tensor_bytes): the largest graph in whole pages, and the largest read or write region of any graph by the
    OS layout. The write region holds the graph's memory pool as well as its outputs: a graph registered after admission must
    fit regions of tensor_bytes (drivers/npu/a733_npu_runtime.c), and a sealed graph's inputs and outputs fit inside them."""
    sys.path.insert(0, str(ROOT / 'tools'))
    import npu_graph_layout
    paths = [Path(artifacts[name]['model']) for name in names]
    layout = npu_graph_layout.regions(paths)
    graph = max(p.stat().st_size for p in paths)
    tensor = max(max(r['read'], r['write']) for r in layout)
    return align(graph), align(tensor)


def pack(args, out):
    pack_dir = out / 'pack'
    scratch = out / 'pack-scratch'
    subprocess.run([sys.executable, str(ROOT / 'tools/npu-external-pack.py'), '--artifacts', str(args.graphs / 'artifacts.json'),
                    '--models', str(args.graphs / 'list.json'), '--models-address', hex(args.models_address),
                    '--vectors-address', hex(args.vectors_address), '--output', str(scratch)], check=True, capture_output=True)
    pack_dir.mkdir()
    shutil.move(str(scratch / 'models.region'), pack_dir / 'models.region')
    shutil.copy2(args.data, pack_dir / 'vectors.region')
    shutil.rmtree(scratch)
    return pack_dir / 'models.region', pack_dir / 'vectors.region'


def write_manifest(args, out, models, vectors, count, bounds):
    manifest = json.loads((args.app / args.manifest).read_text())
    for name, path, address in (('models', models, args.models_address), ('vectors', vectors, args.vectors_address)):
        r = region_entry(manifest, name)
        data = path.read_bytes()
        r.update(bytes=len(data), address=hex(address), sha256=hashlib.sha256(data).hexdigest())
    npu = manifest.setdefault('npu', {})
    npu.update(slots=count, graphs=count, graph_bytes=bounds[0], tensor_bytes=bounds[1])
    npu.setdefault('in_flight', args.queue_depth)
    npu.setdefault('links', 0)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--app', type=Path, required=True)
    p.add_argument('--graphs', type=Path, required=True)
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--models-address', type=lambda x: int(x, 0), default=0x44000000)
    p.add_argument('--vectors-address', type=lambda x: int(x, 0), default=0x45200000)
    p.add_argument('--manifest', default='manifest.json', help='the manifest template in --app')
    p.add_argument('--define', action='append', default=[], metavar='NAME=VALUE', help='an extra CMake cache entry for the image')
    p.add_argument('--freeze-decoder', metavar='ARGS', help="the arguments that bind the application's decoder to the image (host.py --freeze-image)")
    p.add_argument('--queue-depth', type=int, default=64)
    p.add_argument('--manifest-variable', default='SUNSTONE_NPU_ENCODER_MANIFEST',
                   help='the CMake cache variable through which the application takes its manifest')
    p.add_argument('--cfg', type=Path, default=CACHE / 'bin/cfg.sh', help='the host configuration script (cfg.sh SRC BUILD args...)')
    p.add_argument('--no-build', action='store_true', help='write the regions and the manifest only')
    args = p.parse_args()
    args.app, args.graphs, args.out = args.app.resolve(), args.graphs.resolve(), args.out.resolve()
    if args.data.is_dir():
        args.data = args.data / 'vectors.region'
    if not args.data.is_file():
        raise SystemExit('no vectors region at ' + str(args.data))
    artifacts = json.loads((args.graphs / 'artifacts.json').read_text())
    names = json.loads((args.graphs / 'list.json').read_text())['npu_models']
    args.out.mkdir(parents=True, exist_ok=False)
    models, vectors = pack(args, args.out)
    bounds = graph_bounds(artifacts, names)
    manifest = write_manifest(args, args.out, models, vectors, len(names), bounds)
    name = manifest['name']
    freezable = '--freeze-image' in (args.app / 'host.py').read_text()
    if freezable and not args.freeze_decoder:
        raise SystemExit('the application decoder binds to the image (host.py --freeze-image): pass --freeze-decoder with its arguments, for example "--mode 1"')
    if args.freeze_decoder and not freezable:
        raise SystemExit('--freeze-decoder: the application decoder has no --freeze-image')
    image, session = args.out / 'image', args.out / 'session'
    configure = [str(args.cfg), str(ROOT), str(image),
                 '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_MIMUS_BOARD_APPLICATION=ON', '-DSUNSTONE_APPLICATION=' + name,
                 '-DSUNSTONE_APPLICATION_%s_ROOT=%s' % (name, args.app),
                 '-DSUNSTONE_A733_OS_CPU_RETURN=ON', '-DSUNSTONE_A733_OS_PERF_BOOT=high',
                 '-DSUNSTONE_MIMUS_WORKERS=2',
                 '-D%s=%s' % (args.manifest_variable, args.out / 'manifest.json'),
                 *('-D' + d for d in args.define)]
    build = ['cmake', '--build', str(image), '--target', 'sunstone-a733-os', '-j4']
    elf = image / 'sunstone-a733-os.elf'
    decoder = args.out / 'decoder-frozen.py' if args.freeze_decoder else args.app / 'host.py'
    freeze = [sys.executable, '-B', str(args.app / 'host.py'), '--freeze-image', str(elf), '--decoder-output', str(decoder),
              *shlex.split(args.freeze_decoder or '')]
    make_session = [sys.executable, '-B', str(ROOT / 'tools/session.py'), 'build', str(elf), '--out', str(session),
                    '--manifest', str(args.out / 'manifest.json'), '--input', str(models), '--input', str(vectors),
                    '--decoder', str(decoder)]
    if manifest.get('cpu', {}).get('clock_grants'):
        # The session grants the CPU clock when the manifest declares it (the CPU-return admission takes a grant without
        # the performance service: --perf is for other images).
        make_session.append('--clock-grant')
    submit = [sys.executable, '-B', str(ROOT / 'tools/board.py'), 'submit', str(elf), '--kind', 'npu', '--port', 'PORT',
              '--template', str(session / 'template'), '--decoder', str(decoder if args.freeze_decoder else session / 'decoder.py'), '--restore-rails',
              '--input', str(models), '--input', str(vectors), '--hypothesis', 'TEXT', '--wait']
    (args.out / 'recipe.txt').write_text('\n'.join(' '.join(c) for c in (configure, build, *([freeze] if args.freeze_decoder else []), make_session, submit)) + '\n')
    print('%d graphs; models region %d bytes, graph_bytes %d, tensor_bytes %d' % (len(names), models.stat().st_size, *bounds))
    if args.no_build:
        return
    subprocess.run(configure, check=True)
    subprocess.run(build, check=True)
    if args.freeze_decoder:
        subprocess.run(freeze, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(make_session, check=True)
    print('image', elf)
    print('submit: see', args.out / 'recipe.txt')


if __name__ == '__main__':
    main()
