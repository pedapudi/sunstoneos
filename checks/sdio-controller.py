#!/usr/bin/env python3
"""Build one focused native SDIO controller/card model check."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REFERENCES = {
    "bsp/drivers/mmc/sunxi-mmc.c": "52c7f92db77290437b7818f3c3a92a2956c1ab637214794e56220d0dde99ce16",
    "bsp/drivers/mmc/sunxi-mmc.h": "0945ee0ea5e47082c2ff159e578380671f86f4cd797f4fb3a853bc6f8cde0da3",
    "bsp/drivers/mmc/sunxi-mmc-v5p3x.c": "348a6893ae256b2df712813e998ab2b6a13341dd8c57f6fcfb31656fe735d94c",
    "drivers/mmc/core/sdio_ops.c": "23e6cd52c802263e69e95eacc4054a9afcd751da746ef6fa53f8312f93a7aa29",
    "drivers/mmc/core/sdio_cis.c": "5ff82f36a6c15fb9bb8836ec54bb53c44a0be9210e898a0db647bc702716d118",
    "include/linux/mmc/mmc.h": "9762e69cbbadb447abe1ba191ad2fc883d645c808024dfd810c12436ad4a695b",
    "include/linux/mmc/sdio.h": "c2657f975c3f38c5c1071c63c7638913cc227296794010d3745d15e1fe414cbe",
}
SOURCES = ["checks/sdio-controller.c", "drivers/storage/sunxi_sdio.c",
           "drivers/storage/sdio_card.c", "drivers/net/aic8800_sdio_codec.c"]
HEADERS = ["include/sunxi_sdio.h", "include/sdio_card.h", "include/aic8800_sdio_codec.h"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-dir", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--case", choices=["all", "select-io", "cis-shape", "reinit"], default="all")
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    if evidence == ROOT or ROOT in evidence.parents:
        parser.error("evidence must be outside the source tree")
    for name, expected in REFERENCES.items():
        if digest(args.reference_dir / name) != expected:
            raise SystemExit(f"reference hash mismatch: {name}")
    evidence.mkdir(parents=True, exist_ok=True)
    executable = evidence / "sdio-controller-check"
    command = [args.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include"),
               *(str(ROOT / p) for p in SOURCES), "-o", str(executable)]
    build = subprocess.run(command, text=True, capture_output=True)
    (evidence / "build.log").write_text(build.stdout + build.stderr)
    build.check_returncode()
    run_command = [str(executable)] + ([args.case] if args.case != "all" else [])
    run = subprocess.run(run_command, text=True, capture_output=True)
    (evidence / "run.log").write_text(run.stdout + run.stderr)
    result = {"result": "passed" if run.returncode == 0 else "failed", "command": command,
              "exit_code": run.returncode, "run_command": run_command, "case": args.case,
              "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
              "sources": {p: digest(ROOT / p) for p in SOURCES + HEADERS},
              "runner_sha256": digest(Path(__file__).resolve()), "references": REFERENCES,
              "executable_sha256": digest(executable), "stdout": run.stdout, "stderr": run.stderr}
    (evidence / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"result": result["result"], "stdout": run.stdout, "evidence": str(evidence)}))
    run.check_returncode()


if __name__ == "__main__":
    main()
