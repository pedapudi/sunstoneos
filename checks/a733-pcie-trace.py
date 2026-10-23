#!/usr/bin/env python3
"""Run the PCIe host models and compare write traces with an earlier revision.

The core, endpoint-test, general function and NVMe models run against the
current tree. Only the Gen1 trace driver also builds against --baseline-rev.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import io

ROOT = Path(__file__).resolve().parents[1]
UNITS = ["drivers/pci/a733_pcie.c", "drivers/pci/a733_pcie_phy.c", "drivers/pci/a733_pcie_epf_test.c",
         "drivers/power/a733_pmic.c", "drivers/power/a733_rail_leases.c",
         "drivers/clock/a733_clock_leases.c", "drivers/pinctrl/a733_pins.c",
         "drivers/pinctrl/a733_pin_tables.c", "drivers/gpu/a733_gpu_resources.c",
         "runtime/device_memory_pool.c"]
EPF_MODES = ["nominal", "wide", "other-bar", "high-half", "oversize", "deadline", "missing-msi",
             "status-error", "extra-msi", "data-error", "guard-error", "source-error", "link-loss",
             "gen2", "gen2-retrain"]
TRACE_MODES = ["core", "nominal", "wide"]
# The function layer and NVMe driver postdate the trace baseline.
NVME_UNITS = UNITS + ["drivers/pci/a733_pcie_function.c", "drivers/pci/a733_pcie_nvme.c"]
NVME_MODES = ["poll", "msix", "intx", "gen2", "forged", "inherited", "fatal", "identify-error",
              "timeout", "extra-vector", "extra-intx", "shutdown-stuck", "suspend", "suspend-keep",
              "multifunction", "bridge", "warm-sibling", "large-bar", "assigned-bar", "table-outside",
              "class", "stop-config", "stop-test", "stop-link-stuck"]


def block(text, pattern):
    start = re.search(pattern, text).start()
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True, help="New output directory")
parser.add_argument("--baseline-rev", help="Git revision whose Gen1 write traces must match")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
compiler = Path(shutil.which("clang-21")).resolve()
commands = []


def run(command):
    result = subprocess.run([str(x) for x in command], text=True, capture_output=True)
    commands.append({"command": [str(x) for x in command], "exit": result.returncode,
                     "stdout": result.stdout[-4000:], "stderr": result.stderr[-4000:]})
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    if result.returncode:
        raise RuntimeError(" ".join(str(x) for x in command) + "\n" + result.stdout[-4000:] + result.stderr[-4000:])
    return result.stdout


def build(root, check, name, units=UNITS):
    """Compile one host model with the native address filter of the same root."""
    native = (root / "drivers/pci/a733_pcie_native.c").read_text()
    generated = out / (name + "-generated")
    generated.mkdir()
    address = generated / "address.c"
    address.write_text('#include "a733_pcie_native.h"\n' + block(native, r"int a733_pcie_native_address\(") + "\n")
    # The trace driver reuses the endpoint check's static model under another entry point.
    epf = (root / "checks/a733-pcie-epf.c").read_text()
    assert epf.count("int main(int argc,char **argv)") == 1
    (generated / "a733-pcie-epf-entry.c").write_text(
        epf.replace("int main(int argc,char **argv)", "int epf_main(int argc,char **argv)"))
    target = out / name
    run([compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-I" + str(root / "include"),
         "-I" + str(generated), "-I" + str(root / "checks"),
         root / check, *[root / u for u in units], address, "-o", target])
    return target


results = {}
core = build(ROOT, "checks/a733-pcie.c", "core")
run([core]); results["core"] = "PASS"
epf = build(ROOT, "checks/a733-pcie-epf.c", "epf")
for mode in EPF_MODES:
    run([epf, mode]); results["epf-" + mode] = "PASS"
nvme = build(ROOT, "checks/a733-pcie-nvme.c", "nvme", NVME_UNITS)
for mode in NVME_MODES:
    run([nvme, mode]); results["nvme-" + mode] = "PASS"
trace = build(ROOT, "checks/a733-pcie-trace.c", "trace")
current = {mode: run([trace, mode]) for mode in TRACE_MODES}
for mode, text in current.items():
    (out / ("trace-" + mode + ".txt")).write_text(text)
if args.baseline_rev:
    # Build the unchanged trace driver against the earlier sources.
    archive = subprocess.run(["git", "-C", str(ROOT), "archive", args.baseline_rev],
                             capture_output=True, check=True).stdout
    baseline = out / "baseline"
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(baseline, filter="data")
    shutil.copy2(ROOT / "checks/a733-pcie-trace.c", baseline / "checks/a733-pcie-trace.c")
    before = build(baseline, "checks/a733-pcie-trace.c", "trace-baseline")
    for mode in TRACE_MODES:
        text = run([before, mode])
        (out / ("trace-baseline-" + mode + ".txt")).write_text(text)
        assert text == current[mode], "Gen1 write trace differs: " + mode
        results["trace-" + mode] = {"writes": len(text.splitlines()),
                                    "sha256": hashlib.sha256(text.encode()).hexdigest(),
                                    "identical_to": args.baseline_rev}
(out / "results.json").write_text(json.dumps({"status": "PASS", "results": results,
    "limitations": ["controller, endpoint, DMA, MSI and scheduler responses are modeled",
                    "the NVMe controller, PCI PM, INTx/MSI-X delivery and link loss are modeled",
                    "no physical link, speed change or electrical qualification"]}, indent=2) + "\n")
print(json.dumps(results))
