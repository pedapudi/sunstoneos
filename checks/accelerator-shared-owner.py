#!/usr/bin/env python3
"""Build and run only the live GPU/NPU shared-owner composition cases."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--compiler", default="/usr/bin/clang-21")
parser.add_argument("--case", type=int, choices=range(8), action="append", dest="cases")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
sources = [
    "checks/accelerator-shared-owner.c",
    "drivers/npu/a733_npu_startup.c", "drivers/npu/a733_npu_platform.c",
    "drivers/power/a733_pmic.c", "drivers/power/a733_rail_leases.c",
    "drivers/clock/a733_clock_leases.c", "drivers/gpu/a733_gpu_resources.c",
    "drivers/pinctrl/a733_pins.c", "drivers/pinctrl/a733_pin_tables.c",
    "drivers/power/a733_power_domains.c", "drivers/power/a733_pmic_consumer.c",
    "drivers/gpu/a733_gpu_power.c", "drivers/clock/a733_cpu_clock.c", "drivers/thermal/a733_thermal.c",
]
command = [args.compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
           "-fno-omit-frame-pointer", "-Iinclude", "-Ichecks", "-Ichecks/npu-startup-host",
           *sources, "-o", str(output / "composition")]
(output / "compile.json").write_text(json.dumps(command, indent=2) + "\n")
with (output / "compile.log").open("w") as log:
    subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
# Include the inherited model and all project headers in the evidence identity.
# This is a conservative source inventory, not a claim they are all linked.
inputs = [root / p for p in sources] + list((root / "include").glob("*.h"))
inputs += [root / "checks" / p for p in ("npu-startup-host.c", "power-domains-model.h", "a733-pmic.c")]
inputs += list((root / "checks/npu-startup-host").glob("*.h")) + [Path(__file__).resolve()]
identities = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
(output / "sources.json").write_text(json.dumps(identities, indent=2) + "\n")
cases = args.cases if args.cases is not None else range(5)
results = []
for case in cases:
    command = [str(output / "composition"), str(case)]
    with (output / f"case-{case}.log").open("w") as log:
        subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = json.loads((output / f"case-{case}.log").read_text())
    results.append(result)
    print(json.dumps(result), flush=True)
    (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
