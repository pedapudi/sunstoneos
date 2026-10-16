#!/usr/bin/env python3
"""Exercise the FreeRTOS foundation on QEMU and retain reusable evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("image", type=Path)
parser.add_argument("--qemu", default="qemu-system-aarch64")
parser.add_argument("--evidence", type=Path,
                    default=Path.home() / ".cache/sunstoneos/evidence/kernel-check.json")
args = parser.parse_args()
emulator = shutil.which(args.qemu)
if emulator is None:
    sys.exit(f"Emulator not found: {args.qemu}")
command = [emulator, "-machine", "virt,gic-version=3,secure=off,virtualization=off",
           "-cpu", "cortex-a55", "-smp", "1", "-m", "128M", "-nographic",
           "-monitor", "none", "-semihosting-config", "enable=on,target=native",
           "-kernel", str(args.image.resolve())]
identity = {"image_sha256": digest(args.image), "qemu_sha256": digest(emulator),
            "checker_sha256": digest(__file__), "command": command}
# QEMU uses dynamically linked host libraries. Include their content identities
# so an emulator dependency update invalidates the cached result.
libraries = subprocess.run(["ldd", emulator], check=True, capture_output=True, text=True).stdout
identity["libraries"] = {word: digest(word) for word in libraries.split()
                         if word.startswith("/") and Path(word).is_file()}
try:
    previous = json.loads(args.evidence.read_text())
except (OSError, ValueError):
    previous = {}
if previous.get("passed") and previous.get("identity") == identity:
    print("Reusing the successful kernel check: image, emulator, libraries and checker match.")
    print(previous["output"], end="")
    sys.exit(0)
try:
    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
    output = result.stdout + result.stderr
    code = result.returncode
except subprocess.TimeoutExpired as error:
    output = ((error.stdout or b"") + (error.stderr or b"")).decode(errors="replace")
    output += "\nFAIL: emulator exceeded ten seconds.\n"
    code = 124
passed = code == 0 and "PASS kernel:" in output and "FAIL" not in output
args.evidence.parent.mkdir(parents=True, exist_ok=True)
args.evidence.write_text(json.dumps({"identity": identity, "passed": passed,
                                   "exit_code": code, "output": output}, indent=2) + "\n")
print(output, end="")
sys.exit(0 if passed else 1)
