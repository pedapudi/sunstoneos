#!/usr/bin/env python3
"""Run a bounded SMP recognition session and compare its recorded host reference."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', type=Path, required=True)
parser.add_argument('--bundle', type=Path, required=True)
parser.add_argument('--reference', type=Path, required=True)
parser.add_argument('--evidence', type=Path, required=True)
parser.add_argument('--qemu', default='qemu-system-aarch64')
parser.add_argument('--cores', type=int, choices=(2, 8), default=2)
parser.add_argument('--paced', action='store_true')
args = parser.parse_args()
emulator = shutil.which(args.qemu)
if not emulator:
    sys.exit(f'Emulator not found: {args.qemu}')
reference = json.loads(args.reference.read_text())
bundle_hash = digest(args.bundle)
if reference['exit'] != 0 or reference['bundle_sha256'] != bundle_hash:
    sys.exit('A successful host reference for this exact bundle is required.')
if bool(reference.get('paced')) != args.paced:
    sys.exit('Reference mode differs from the requested mode.')
checker = Path(__file__).resolve().parents[1] / 'checks/mimus-parity.py'
command = [emulator, '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
           '-cpu', 'cortex-a55', '-smp', str(args.cores), '-m', '128M', '-nographic',
           '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
           '-device', f'loader,file={args.bundle.resolve()},addr=0x43000000,force-raw=on',
           '-device', f'loader,file={args.image.resolve()},cpu-num=0']
identity = dict(command=command, image_sha256=digest(args.image), bundle_sha256=bundle_hash,
                reference_sha256=digest(args.reference), checker_sha256=digest(checker),
                runner_sha256=digest(__file__), qemu_sha256=digest(emulator), paced=args.paced,
                timeout_seconds=180)
libraries = subprocess.run(['ldd', emulator], check=True, capture_output=True, text=True).stdout
identity['libraries'] = {word: digest(word) for word in libraries.split()
                         if word.startswith('/') and Path(word).is_file()}
args.evidence.mkdir(parents=True, exist_ok=True)
result_path = args.evidence / 'model.json'
parity_path = args.evidence / 'parity.json'
try:
    previous = json.loads(result_path.read_text())
    parity = json.loads(parity_path.read_text())
except (OSError, ValueError):
    previous, parity = {}, {}
if (previous.get('identity') == identity and parity.get('passed') and
        parity.get('candidate_sha256') == digest(result_path)):
    print('Reusing successful recognition evidence: all execution and comparison inputs match.')
    sys.exit(0)
try:
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    code, stdout, stderr = result.returncode, result.stdout, result.stderr
except subprocess.TimeoutExpired as error:
    code = 124
    stdout = (error.stdout or b'').decode(errors='replace')
    stderr = (error.stderr or b'').decode(errors='replace') + '\nFAIL: host deadline exceeded.\n'
record = dict(identity=identity, image_sha256=identity['image_sha256'], bundle_sha256=bundle_hash,
              paced=args.paced, exit=code, stdout=stdout, stderr=stderr)
result_path.write_text(json.dumps(record, indent=2) + '\n')
print(stdout + stderr, end='', flush=True)
if code or 'FAIL' in stdout + stderr or 'ERROR ' in stdout + stderr:
    sys.exit('Recognition failed; see model.json.')
workers = re.findall(rf'^WORKERS count={args.cores} jobs=([0-9]+) core_mask={(1 << args.cores) - 1:x}$', stdout + stderr, re.M)
if len(workers) != 1 or int(workers[0]) == 0:
    sys.exit('No completed inference work on every selected core; see model.json.')
result = subprocess.run([sys.executable, str(checker), '--reference', str(args.reference),
                         '--candidate', str(result_path), '--output', str(parity_path)])
sys.exit(result.returncode)
