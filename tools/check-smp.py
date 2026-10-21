#!/usr/bin/env python3
"""Check the two-core FreeRTOS emulator targets, or the eight-core soak, and retain evidence."""
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
parser.add_argument("--trials", type=int, default=1)
parser.add_argument("--case", choices=("kernel", "context", "runtime", "cleanup", "linear", "soak8"), default="kernel")
parser.add_argument("--evidence", type=Path,
                    default=Path.home() / ".cache/sunstoneos/evidence/smp-check.json")
args = parser.parse_args()
if not 1 <= args.trials <= 10:
    parser.error("trials must be between 1 and 10")
emulator = shutil.which(args.qemu)
if emulator is None:
    sys.exit(f"Emulator not found: {args.qemu}")
command = [emulator, "-machine", "virt,gic-version=3,secure=off,virtualization=off",
           "-cpu", "cortex-a55", "-smp", "8" if args.case == "soak8" else "2", "-m", "128M", "-nographic",
           "-monitor", "none", "-semihosting-config", "enable=on,target=native",
           "-device", f"loader,file={args.image.resolve()},cpu-num=0"]
identity = {"image_sha256": digest(args.image), "qemu_sha256": digest(emulator),
            "checker_sha256": digest(__file__), "command": command, "trials": args.trials,
            "case": args.case}
libraries = subprocess.run(["ldd", emulator], check=True, capture_output=True, text=True).stdout
identity["libraries"] = {word: digest(word) for word in libraries.split()
                         if word.startswith("/") and Path(word).is_file()}
try:
    previous = json.loads(args.evidence.read_text())
except (OSError, ValueError):
    previous = {}
if previous.get("passed") and previous.get("identity") == identity:
    print("Reusing successful SMP evidence: image, emulator, libraries and checker match.")
    sys.exit(0)
trials = []
for index in range(args.trials):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30 if args.case == "soak8" else 10)
        output, code = result.stdout + result.stderr, result.returncode
    except subprocess.TimeoutExpired as error:
        output = ((error.stdout or b"") + (error.stderr or b"")).decode(errors="replace")
        output += "\nFAIL: emulator exceeded its host deadline.\n"
        code = 124
    marker = {"kernel": "PASS SMP: 256 cross-core queue exchanges",
              "context": "PASS SMP context: 64 task migrations",
              "runtime": "PASS SMP runtime: concurrent allocation",
              "cleanup": "PASS SMP cleanup: two migrated tasks reclaimed",
              "linear": "PASS SMP linear: serial parity and output guards",
              "soak8": "PASS SMP8 soak: 64-lap eight-core queue ring"}[args.case]
    passed = code == 0 and marker in output and "FAIL" not in output
    trials.append({"passed": passed, "exit_code": code, "output": output})
    print(f"Trial {index + 1}: {output}", end="")
    if not passed:
        break
passed = len(trials) == args.trials and all(trial["passed"] for trial in trials)
args.evidence.parent.mkdir(parents=True, exist_ok=True)
args.evidence.write_text(json.dumps({"identity": identity, "passed": passed,
                                   "trials": trials}, indent=2) + "\n")
sys.exit(0 if passed else 1)
