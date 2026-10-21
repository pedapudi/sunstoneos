#!/usr/bin/env python3
"""Build the patched sunxi-fel recovery tool from the locked sunxi-tools archive.

The script only compiles. It never runs the tool it builds, and it never opens a
USB device. Pass header and library locations as make variables when the host
has no libusb-1.0 or libfdt development package, for example
LIBUSB_CFLAGS=-I<dir>/usr/include/libusb-1.0.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
# The SRAM and status patches came first, the execute-argument fix afterward,
# and the bounded receive follows the status patch it modifies.
PATCHES = ("sunxi-a733-sram.patch", "sunxi-fel-status.patch",
           "sunxi-execute-arguments.patch", "sunxi-fel-bounded-read.patch")
# Four-patch tool recorded by the model-readback evidence and by FEL_HASH in
# tools/check-a733-smp.py and related checks. The earliest DDR and AArch64
# probes used an earlier build without the bounded receive.
RECORDED = "4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True, help="new directory for the source tree and record")
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/sunstoneos/sources")
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--allow-different", action="store_true",
                        help="exit 0 even when the tool differs from the recorded one")
    parser.add_argument("make_vars", nargs="*", metavar="NAME=VALUE", help="extra make variables")
    args = parser.parse_args()
    source = json.loads((ROOT / "sources.lock.json").read_text())["sunxi-tools"]
    archive = args.cache / source["archive"]
    if digest(archive) != source["sha256"]:
        raise SystemExit(f"{archive} differs from the sunxi-tools pin; fetch it with tools/fetch-sources.py")
    args.output.mkdir(parents=True)
    with tarfile.open(archive) as bundle:
        bundle.extractall(args.output, filter="data")
    tree = args.output / source["directory"]
    for name in PATCHES:
        # Fuzz would let a patch land on changed context; offsets alone are allowed.
        subprocess.run(["patch", "-p0", "--fuzz=0", "--batch", "--no-backup-if-mismatch",
                        "-i", str(ROOT / "patches" / name)], cwd=tree, check=True)
    command = ["make", "-C", str(tree), f"CC={args.cc}", *args.make_vars, "sunxi-fel"]
    with (args.output / "build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    tool = tree / "sunxi-fel"
    compiler = subprocess.run([args.cc, "--version"], capture_output=True, text=True, check=True).stdout
    record = {"archive": {"path": str(archive), "sha256": source["sha256"], "revision": source["revision"]},
              "patches": [{"name": name, "sha256": digest(ROOT / "patches" / name)} for name in PATCHES],
              "command": command, "compiler": compiler.splitlines()[0],
              "sunxi_fel": {"path": str(tool), "sha256": digest(tool)},
              "recorded_sha256": RECORDED, "matches_recorded": digest(tool) == RECORDED,
              "executed": False}
    (args.output / "build.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"{tool} {record['sunxi_fel']['sha256']} "
          f"({'matches' if record['matches_recorded'] else 'differs from'} the recorded tool)")
    if not record["matches_recorded"] and not args.allow_different:
        raise SystemExit(f"{tool} differs from the recorded tool {RECORDED}; "
                         "pass --allow-different to accept it")


if __name__ == "__main__":
    main()
