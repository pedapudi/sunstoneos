#!/usr/bin/env python3
"""Build and run the runtime receive check for one aggregate over 4,096 bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["checks/radio-transport-aggregate.c", "drivers/net/aic8800_dispatch.c", "drivers/net/aic8800_runtime.c",
           "drivers/net/aic8800_wifi.c", "drivers/net/aic8800_security.c", "drivers/net/aic8800_data.c",
           "drivers/net/aic8800_firmware.c", "drivers/net/aic8800_sdio_transport.c", "drivers/net/aic8800_sdio_codec.c",
           "drivers/storage/storage_digest.c", "drivers/storage/sunxi_sdio.c", "drivers/storage/sdio_card.c"]
INPUTS = ["include/aic8800_dispatch.h", "include/aic8800_sdio_transport.h", "include/sunxi_sdio.h", "include/sdio_card.h",
          "checks/aic8800-runtime.c", "checks/aic8800-sdio-transport.c", "checks/sdio-controller.c"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    out = args.evidence.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error("evidence must be outside the source tree")
    out.mkdir(parents=True, exist_ok=True)
    executable = out / "radio-transport-aggregate-check"
    command = [args.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include"),
               *(str(ROOT / p) for p in SOURCES), "-o", str(executable)]
    build = subprocess.run(command, text=True, capture_output=True)
    (out / "build.log").write_text(build.stdout + build.stderr)
    build.check_returncode()
    run = subprocess.run([str(executable)], text=True, capture_output=True)
    (out / "run.log").write_text(run.stdout + run.stderr)
    result = {"result": "passed" if run.returncode == 0 else "failed", "command": command,
              "exit_code": run.returncode, "stdout": run.stdout, "stderr": run.stderr,
              "sources": {p: digest(ROOT / p) for p in SOURCES + INPUTS},
              "runner_sha256": digest(Path(__file__).resolve()), "executable_sha256": digest(executable)}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"result": result["result"], "stdout": run.stdout, "evidence": str(out)}))
    run.check_returncode()


if __name__ == "__main__":
    main()
