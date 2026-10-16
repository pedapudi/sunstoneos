#!/usr/bin/env python3
"""Prepare an Orange Pi model declaration and a RAM input bundle without changing tensors."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import wave
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mimus', type=Path, required=True)
parser.add_argument('--model-dir', help='Source all-int8 model; defaults to the mimus P4 reference')
parser.add_argument('--wav', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, str(args.mimus / 'tools'))
sys.path.insert(0, str(args.mimus / 'train'))
import models
import model_format as mf
source = models.resolve('p4', args.model_dir)
audio = args.wav or Path(models.BENCH_HOST)
with wave.open(str(audio), 'rb') as stream:
    if (stream.getnchannels(), stream.getsampwidth(), stream.getframerate(), stream.getcomptype()) != (1, 2, 16000, 'NONE'):
        sys.exit('Audio must be uncompressed 16 kHz mono PCM16.')
    samples = stream.getnframes()
args.output.mkdir(parents=True, exist_ok=False)
target = mf.TARGETS['orangepi-zero3w']
base_hash = mf.rewrite(str(Path(source.path) / 'base.mimus'), str(args.output / 'base.mimus'), target)
mf.rewrite(str(Path(source.path) / 'overlay.mimus'), str(args.output / 'overlay.mimus'), target, overlay_of=base_hash)
inputs = []
identities = {}
for name in ('base.mimus', 'overlay.mimus'):
    original = (Path(source.path) / name).read_bytes()
    data = (args.output / name).read_bytes()
    if original[256:] != data[256:]:
        sys.exit(f'{name}: model body changed; do not execute this bundle.')
    inputs.append(data)
    identities[name] = dict(source_sha256=hashlib.sha256(original).hexdigest(),
                           sha256=hashlib.sha256(data).hexdigest(), body_unchanged=True,
                           bytes=len(data), content_hash=data[48:80].hex())
inputs.append(audio.read_bytes())
bundle = bytearray(128)
struct.pack_into('<8sQQQIII', bundle, 0, b'SUNMIM01', *map(len, inputs),
                 *(zlib.crc32(data) for data in inputs))
for data in inputs:
    bundle.extend(data)
    bundle.extend(b'\0' * (-len(bundle) % 64))
if len(bundle) > 32 * 1024 * 1024:
    sys.exit('Bundle exceeds its reserved RAM range.')
(args.output / 'inputs.bin').write_bytes(bundle)
record = dict(model=identities, audio=str(audio.resolve()), samples=samples,
              audio_sha256=hashlib.sha256(inputs[2]).hexdigest(),
              bundle_sha256=hashlib.sha256(bundle).hexdigest(), bundle_bytes=len(bundle),
              mimus_base_revision=subprocess.check_output(['git','-C',str(args.mimus),'rev-parse','HEAD'],text=True).strip(),
              load_address='0x43000000', target=target)
(args.output / 'inputs.json').write_text(json.dumps(record,indent=2)+'\n')
print(args.output / 'inputs.json')
