#!/usr/bin/env python3
"""Build one integrated native AIC8800 SDIO bootstrap transport check."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REFERENCES = {
    "aicsdio.c": "f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c",
    "aicsdio.h": "af520385376df1843a6e24c01cc7128a151b148fea18d73c0d20f1a345bc6571",
    "aic_bsp_driver.c": "43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e",
    "aic_bsp_driver.h": "9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3",
    "sdio_io.c": "2f9db1b81f01d359faa689fb7609c04d6a9fdd7d833d16e201f74d7201e51e44",
}
SOURCES = ["drivers/net/aic8800_firmware.c", "drivers/net/aic8800_runtime.c", "drivers/net/aic8800_wifi.c", "drivers/net/aic8800_data.c", "checks/aic8800-sdio-transport.c", "drivers/net/aic8800_sdio_transport.c", "drivers/storage/sunxi_sdio.c",
           "drivers/storage/sdio_card.c", "drivers/net/aic8800_sdio_codec.c"]
HEADERS = ["include/aic8800_firmware.h", "include/aic8800_runtime.h", "include/aic8800_wifi.h", "include/aic8800_data.h", "include/sunxi_sdio.h", "include/sdio_card.h", "include/aic8800_sdio_codec.h", "include/aic8800_sdio_transport.h", "checks/sdio-controller.c"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-dir", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--case", choices=["all", "bootstrap"], default="all")
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    if evidence == ROOT or ROOT in evidence.parents:
        parser.error("evidence must be outside the source tree")
    for name, expected in REFERENCES.items():
        if digest(args.reference_dir / name) != expected:
            raise SystemExit(f"reference hash mismatch: {name}")
    evidence.mkdir(parents=True, exist_ok=True)
    executable = evidence / "aic8800-sdio-transport-check"
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
