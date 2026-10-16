#!/usr/bin/env python3
"""Compile one host codec check with a pinned source CRC reference."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REFERENCES = {
    "aicsdio.c": "f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c",
    "aic_bsp_driver.c": "43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e",
    "aic_bsp_driver.h": "9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3",
    "aic_bsp_txrxif.c": "e8ed777e6c7c26c77e0914f48a89ffde4f6bdaf798445b04489abb7c399d3573",
    "aic_bsp_export.h": "10c913d5c5ab6d5f48e70be8a710ffe99416496f1718f12bc2dd901b380c00a5",
    "aic_bsp_8800d80.c": "fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-dir", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    if evidence == ROOT or ROOT in evidence.parents:
        parser.error("evidence must be outside the source tree")
    evidence.mkdir(parents=True, exist_ok=True)
    texts = {}
    for name, expected in REFERENCES.items():
        path = args.reference_dir / name
        if digest(path) != expected:
            raise SystemExit(f"reference hash mismatch: {name}")
        texts[name] = path.read_text()
    match = re.search(r"uint8_t crc8_ponl_107\(.*?\n}\n", texts["aicsdio.c"], re.S)
    if not match:
        raise SystemExit("pinned CRC implementation not found")
    # Preserve the file's original leading notice with the extracted reference.
    notice = texts["aicsdio.c"].split("#include", 1)[0]
    reference = evidence / "reference-crc.c"
    reference.write_text(notice + "\n#include <stdint.h>\n" +
                         match.group().replace("crc8_ponl_107", "reference_header_crc", 1))
    sources = [ROOT / "checks/aic8800-sdio-codec.c", ROOT / "drivers/net/aic8800_sdio_codec.c"]
    executable = evidence / "radio-codec-check"
    command = [args.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include"),
               *map(str, sources), str(reference), "-o", str(executable)]
    built = subprocess.run(command, text=True, capture_output=True)
    (evidence / "build.log").write_text(built.stdout + built.stderr)
    built.check_returncode()
    run = subprocess.run([str(executable)], text=True, capture_output=True)
    (evidence / "run.log").write_text(run.stdout + run.stderr)
    owned = sources + [ROOT / "include/aic8800_sdio_codec.h", Path(__file__).resolve()]
    result = {"result": "passed" if run.returncode == 0 else "failed",
              "command": command, "exit_code": run.returncode,
              "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
              "sources": {str(p.relative_to(ROOT)): digest(p) for p in owned},
              "references": REFERENCES, "executable_sha256": digest(executable),
              "stdout": run.stdout, "stderr": run.stderr}
    (evidence / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"result": result["result"], "stdout": run.stdout, "evidence": str(evidence)}))
    run.check_returncode()


if __name__ == "__main__":
    main()
