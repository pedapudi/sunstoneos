#!/usr/bin/env python3
"""Build and run only the live GPU/NPU shared-owner composition cases.

Cases 0-7 run in one ASan/UBSan binary on one thread. Cases 8 and 9 run the
GPU retirement and the NPU retirement on real threads (MODEL_THREADS, see
checks/accelerator-shared-owner.c) in three binaries: ASan/UBSan,
ThreadSanitizer over drivers, harness and models, and ThreadSanitizer over
drivers and harness only (MODEL_DRIVER_RACES, which removes the ordering
edges of the model lock). The ThreadSanitizer runs must exit 0 with no report.
Each threaded case runs --runs times from --seed and must overlap the two
retirements in a minimum number of runs. Two mutants of
drivers/gpu/a733_gpu_power.c, written into the output directory, must fail
case 8 with their stated invariant violation:
  pck-gate-at-release  the last step of the GPU power-off sequence, before
                       the controller's release callback and the release of
                       the GPU lease, closes the shared PCK clock gate
                       (R_CCU+0x1ac bit 0) while the NPU holds its PCK lease;
  broad-ahb-mask       the GPU retirement closes its AHB gate with the vendor
                       helper's field mask 0x010000ff on CCU+0x5c0, which
                       also clears the NPU AHB gate (bit 6) during the NPU job.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("output", type=Path)
parser.add_argument("--compiler", default="/usr/bin/clang-21")
parser.add_argument("--case", type=int, choices=range(10), action="append", dest="cases")
parser.add_argument("--runs", type=int, default=200, help="runs of each threaded case")
parser.add_argument("--seed", type=int, default=20261114, help="seed of the threaded cases' delays")
parser.add_argument("--no-mutants", action="store_true", help="skip the two mutants")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
GPU_POWER = "drivers/gpu/a733_gpu_power.c"
sources = [
    "checks/accelerator-shared-owner.c",
    "drivers/npu/a733_npu_startup.c", "drivers/npu/a733_npu_platform.c",
    "drivers/power/a733_pmic.c", "drivers/power/a733_rail_leases.c",
    "drivers/clock/a733_clock_leases.c", "drivers/gpu/a733_gpu_resources.c",
    "drivers/pinctrl/a733_pins.c", "drivers/pinctrl/a733_pin_tables.c",
    "drivers/power/a733_power_domains.c", "drivers/power/a733_pmic_consumer.c",
    GPU_POWER, "drivers/clock/a733_cpu_clock.c", "drivers/thermal/a733_thermal.c",
]
common = [args.compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
          "-fno-omit-frame-pointer", "-Iinclude", "-Ichecks", "-Ichecks/npu-startup-host"]
ASAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
THREADS = ["-D_GNU_SOURCE", "-DMODEL_THREADS", "-pthread"]
# Each mutant replaces one line of the off sequence with that line and one
# added step; the line must occur exactly once.
MUTANTS = {
    "pck-gate-at-release": (
        "    {NOTE, A733_GPU_CCU, 0x05e0, 0, A733_GPU_NOTE_MBUS_OFF_END},\n",
        "    {UPDATE, A733_GPU_R_CCU, 0x01ac, BIT(0), 0},\n",
        "PCK clock gate R_CCU+0x1ac bit 0 clear while the NPU job is in flight"),
    "broad-ahb-mask": (
        "    {NOTE, A733_GPU_CCU, 0x05e0, 0, A733_GPU_NOTE_MBUS_TOP5_OFF},\n",
        "    {UPDATE, A733_GPU_CCU, 0x05c0, 0x010000ff, 0},\n",
        "NPU AHB gate CCU+0x5c0 bit 6 clear while the NPU job is in flight"),
}
# Overlap minimums per threaded case, as a fraction of the runs. Case 8
# overlaps by construction in every run; the minimum applies to the runs
# with an invariant sample inside the GPU retirement. In case 9 it applies
# to the runs whose local stop call lies inside the GPU retirement.
MINIMUM = {8: 0.75, 9: 0.5}
cases = args.cases if args.cases is not None else [0, 1, 2, 3, 4, 8, 9]
serial_cases = [c for c in cases if c < 8]
threaded_cases = [c for c in cases if c >= 8]

builds = {}
if serial_cases:
    builds["composition"] = common + ASAN + sources
if threaded_cases:
    builds["threads-asan"] = common + ASAN + THREADS + sources
    builds["threads-tsan"] = common + ["-fsanitize=thread"] + THREADS + sources
    builds["threads-tsan-drivers"] = common + ["-fsanitize=thread", "-DMODEL_DRIVER_RACES"] + THREADS + sources
mutant_sources = {}
if threaded_cases and not args.no_mutants:
    original = (root / GPU_POWER).read_text()
    for name, (line, added, _) in MUTANTS.items():
        assert original.count(line) == 1, (name, line)
        directory = output / "mutants" / name
        directory.mkdir(parents=True)
        mutated = directory / "a733_gpu_power.c"
        mutated.write_text(original.replace(line, line + added))
        mutant_sources[name] = mutated
        builds[f"mutant-{name}"] = (common + ASAN + THREADS + ["-DMODEL_MUTANT_GPU_WRITES"] +
            [str(mutated) if s == GPU_POWER else s for s in sources])
(output / "compile.json").write_text(json.dumps(builds, indent=2) + "\n")
jobs = {}
for name, command in builds.items():
    log = (output / f"compile-{name}.log").open("w")
    jobs[name] = (subprocess.Popen(command + ["-o", str(output / name)], cwd=root, stdout=log,
                                   stderr=subprocess.STDOUT), log)
for name, (job, log) in jobs.items():
    job.wait()
    log.close()
    assert job.returncode == 0, f"compile of {name} failed; see compile-{name}.log"
# Include the inherited model and all project headers in the evidence identity.
# This is a conservative source inventory, not a claim they are all linked.
inputs = [root / p for p in sources] + list((root / "include").glob("*.h"))
inputs += [root / "checks" / p for p in ("npu-startup-host.c", "power-domains-model.h", "a733-pmic.c")]
inputs += list((root / "checks/npu-startup-host").glob("*.h")) + [Path(__file__).resolve()]
identities = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
identities.update({str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in mutant_sources.values()})
(output / "sources.json").write_text(json.dumps(identities, indent=2) + "\n")
results = []


def record(result):
    results.append(result)
    print(json.dumps(result), flush=True)
    (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")


def run(binary, arguments, log_name, env=None):
    """The binary's exit status and log. A threaded model that the host did not schedule exits with the inconclusive status (its own hang
    guard); it is run once more and a second stop is inconclusive (harness.KillInconclusive), not a failure."""
    def once():
        with (output / log_name).open("w") as log:
            done = subprocess.run([str(output / binary), *map(str, arguments)], cwd=root, stdout=log,
                                  stderr=subprocess.STDOUT, env=env, timeout=harness.host_kill_seconds("accelerator-shared-owner"))
        return done.returncode, (output / log_name).read_text()
    return harness.until_booted(once, lambda result: result[1], lambda result: result[0])


for case in serial_cases:
    code, text = run("composition", [case], f"case-{case}.log")
    assert code == 0, f"case {case} failed; see case-{case}.log"
    record(json.loads(text))
tsan_env = dict(os.environ, TSAN_OPTIONS="halt_on_error=1:exitcode=66")
for case in threaded_cases:
    minimum = int(MINIMUM[case] * args.runs)
    for build in ("threads-asan", "threads-tsan", "threads-tsan-drivers"):
        log_name = f"case-{case}-{build}.log"
        code, text = run(build, [case, args.runs, args.seed, minimum], log_name,
                         tsan_env if build.startswith("threads-tsan") else None)
        assert code == 0, f"case {case} ({build}) failed with {code}; see {log_name}"
        assert "WARNING: ThreadSanitizer" not in text, f"ThreadSanitizer report; see {log_name}"
        lines = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
        assert len(lines) == 1 and lines[0]["pass"] is True, log_name
        record(dict(lines[0], build=build))
    if case != 8 or args.no_mutants:
        continue
    for name, (_, _, expected) in MUTANTS.items():
        log_name = f"mutant-{name}.log"
        code, text = run(f"mutant-{name}", [8, args.runs, args.seed, minimum], log_name)
        violations = [json.loads(line) for line in text.splitlines() if line.startswith('{"case":8,"run":')]
        assert code != 0, f"mutant {name} passed; see {log_name}"
        assert len(violations) == 1 and violations[0]["violation"] == expected, \
            f"mutant {name} failed for another reason; see {log_name}"
        record({"mutant": name, "case": 8, "exit": code, "run": violations[0]["run"],
                "violation": violations[0]["violation"], "value": violations[0]["value"], "failed": True})

# The claim "the retirement starts during the NPU job" is stated in the model's progress. A host that runs the whole GPU retirement between
# two looks of the NPU thread leaves the GPU thread's active flag clear again; the NPU thread waits, beating, until the retirement is done,
# which is that interleaving on purpose. The model must pass, and the model with the old wait on the active flag alone, built beside it,
# must fail with the violation of that wait.
if 9 in threaded_cases:
    hold = [9, 4, args.seed, 0, "hold-npu"]
    code, text = run("threads-asan", hold, "claim-hold-npu.log")
    assert code == 0, "the claim failed with the NPU thread held until the GPU retirement was done; see claim-hold-npu.log"
    old_build = common + ASAN + THREADS + ["-DSS_OLD_CLAIM_DEADLINE"] + sources
    log = (output / "compile-claim-old-deadline.log").open("w")
    subprocess.run(old_build + ["-o", str(output / "claim-old-deadline")], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
    log.close()
    code, text = run("claim-old-deadline", hold, "claim-old-deadline.log")
    violations = [json.loads(line) for line in text.splitlines() if line.startswith('{"case":9,"run":')]
    assert code != 0 and len(violations) == 1 and \
        violations[0]["violation"] == "deadline: the GPU retirement did not start during the NPU job", \
        "the old wait on the active flag did not fail the held run; see claim-old-deadline.log"
    record({"claim": "retirement starts during the NPU job", "held_run": "passes", "old_wait": "fails", "violation": violations[0]["violation"]})
