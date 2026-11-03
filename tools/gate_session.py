#!/usr/bin/env python3
"""Write the session bytes the runner would send for a built OS image, for the gate's host checks. No device access.

Usage: gate_session.py IMAGE.elf OUTPUT.bin

An NPU image takes its NPU extent from checks/data/npu-session-template (a queue entry's typed session), and the extent is made
to describe this image: the client model and the catalogue digest are read from the image's own os_npu_client_model and
os_npu_catalogue_digest, and the pool from its linker symbols (apps/os/host.py bind_npu_pool). The declaration is the synchronous
wrapper's: one slot, one job in flight, one graph for each catalogue entry (os_npu_model_count).

The session is built by tools/board_images.py exactly as `board.py check` builds it: the same preparation, the same typed
extents, one placeholder byte for each INPUT region. Two inputs stand in for a live read of the board, as in `submit`'s
pre-flight (board_images.offline_words): the captured board words of the evidence cache, and, for an image with the NPU
profile, a modeled NPU-on entry state (the gates open and PCK4 policy and status ON). Neither enters the session bytes beyond the
derived thermal fields and the entry check, which is why a host check can use them. A machine without the evidence capture
cannot build the session and the gate then fails the step, naming it.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import board
import board_images as images


NPU_MAGIC = 0x415337334f4e5031
CLIENT_MODEL, CATALOGUE, DECLARATION = 52, 80, 272          # offsets in struct os_npu_session (apps/os/npu-session.h)


def symbol_bytes(image, name):
    from elftools.elf.elffile import ELFFile
    with Path(image).open('rb') as stream:
        elf = ELFFile(stream)
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if symbol.name == name and symbol['st_shndx'] != 'SHN_UNDEF':
                section = elf.get_section(symbol['st_shndx'])
                offset = symbol['st_value'] - section['sh_addr']
                return section.data()[offset:offset + symbol['st_size']]
    raise SystemExit(f'{image} has no {name}')


def npu_template(image, scratch):
    """A template directory whose session carries this image's NPU client model and catalogue digest."""
    import shutil
    import struct
    raw = bytearray((ROOT / 'checks/data/npu-session-template/session.bin').read_bytes())
    at = raw.find(struct.pack('<Q', NPU_MAGIC))
    if at < 0:
        raise SystemExit('the NPU session template has no NPU extent')
    raw[at + CLIENT_MODEL:at + CLIENT_MODEL + 4] = symbol_bytes(image, 'os_npu_client_model')[:4]
    raw[at + CATALOGUE:at + CATALOGUE + 32] = symbol_bytes(image, 'os_npu_catalogue_digest')
    # The synchronous wrapper has one slot and one job in flight, and the image needs one graph for each catalogue entry.
    models = int.from_bytes(symbol_bytes(image, 'os_npu_model_count'), 'little')
    struct.pack_into('<6I', raw, at + DECLARATION, 1, 1, models, 0, 0, 0)
    directory = Path(scratch) / 'template'
    directory.mkdir()
    (directory / 'session.bin').write_bytes(bytes(raw))
    return directory


def storage_read_session(image, scratch):
    """The modeled session of the supervised SD0 read image (checks/storage-read-fixture.py, profile 2, the profile it runs)."""
    spec = importlib.util.spec_from_file_location('gate_storage_read_fixture', ROOT / 'checks/storage-read-fixture.py')
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    out = Path(scratch) / 'storage-read-fixture'
    # storage-read images named -pN run the fixture at SD profile N (the image directory carries the name); the default is profile 2.
    named = re.search(r'storage-read-p([0-3])', str(image))
    fixture.produce(image, out, False, int(named.group(1)) if named else 2)
    return (out / 'session-template.bin').read_bytes()


def session_for(image, scratch):
    if 'storage_read_consumer' in images.symbols(image):
        return storage_read_session(image, scratch)
    words = images.offline_words()
    if words is None:
        raise SystemExit('the captured board words are not on this machine (board_images.offline_words)')
    words = dict(words)
    for address, mask in images.NPU_ENTRY_GATES.items():
        words[hex(address)] = words.get(hex(address), 0) | mask
    for address in images.NPU_PCK4:
        words[hex(address)] = 8
    parser = argparse.ArgumentParser()
    board.run_options(parser, False)
    parser.add_argument('--evidence', type=Path)
    host = images.load('gate_session_records', ROOT / 'apps/os/host.py')
    manifest = host.image_manifest(image)['manifest']
    values = [str(image), '--port', '3-1', '--evidence', str(Path(scratch) / 'run'), '--kind', 'cpu-return', '--preserve-dram']
    if 'os_npu_client_model' in images.symbols(image):
        # The NPU extents come from a typed session template: a session of a queue entry for an NPU image, kept in the repository.
        values += ['--template', str(npu_template(image, scratch))]
    for number, _ in enumerate(host.regions(manifest, (1,))):
        path = Path(scratch) / f'input-{number}.bin'
        path.write_bytes(b'x')
        values += ['--input', str(path)]
    args = parser.parse_args(values)
    keep = images.policy
    images.policy = lambda *a, **k: None          # a write-capable image needs the coordinator's record; the session does not
    try:
        images.prepare(args, images.inspect(image, 'cpu-return'), {'words': words}, Path(scratch) / 'bundle')
    finally:
        images.policy = keep
    return (Path(scratch) / 'bundle/session.bin').read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('image', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gate-session-') as scratch:
        data = session_for(args.image.resolve(), scratch)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f'{args.output}: {len(data)} bytes')


if __name__ == '__main__':
    main()
