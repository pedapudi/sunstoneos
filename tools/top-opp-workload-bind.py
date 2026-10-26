#!/usr/bin/env python3
"""Bind a refrozen returning CPU diagnostic to the board's current USB instance.

Usage: cpu-prepare.py --bundle BUNDLE --port PORT --survey CURRENT_SURVEY
                      --root-review REVIEW --continuity RECORD [--continuity RECORD]...
                      --output NEW_DIR
No device access. "Bundle 26" below is the rails-and-boot-words read-back
image, bundle 31 in the tree since bringup's GPU and clock-audit rows took 25
to 29; set step6-set-5 ran it as bundle 26 with the frozen copy of this
script from 5a334288. This script binds the top-operating-point deadline profile
(docs/designs/cpu-top-opp.md, step 6) and is otherwise the refrozen
cpu-prepare-22501205.py. A frozen workload bundle carries a byte-identical
copy as cpu-prepare.py, with binding.json beside it: the source archive R,
and bundle 26's frozen row directory with the SHA-256 of its uploaded
probe.bin and of its image.elf. The script binds port 3-3 only: port 3-1's
speed bin, oscillator and supplies have not been read. The last image among
the continuity records must be bundle 26's pass, the read-back of DCDC5 at
1000 mV, DCDC3 at 1050 mV and the boot clock words after a ROM return. Its
run record names the uploaded probe.bin, as every run record does; the
script takes that hash from bundle 26's frozen artifacts.json and requires
the file, the list and binding.json to agree, and the list's image.elf to be
the pinned ELF, so the two hash kinds cannot be exchanged. Every record must
belong to this port's current lifetime: all of them and the survey in one
session directory, each launch starting on the USB instance of the record
before it, and the survey on the instance that bundle 26's run returned to.
BUNDLE holds build/ (overlay and manifest), host/software.json
and template/ (the admission records and prepared bundle of the refrozen native
image 6fd7af86, built from work/gpu-vendor-default-1025 at cc97b957
with the EL3 security writes). The profile comes from the build
manifest's schema: the all-core pre-scheduler, first-task, task-switch and
task-workload profiles, the return-deadline and quiet workload profiles, and
the CPU0 cache, local-interrupt, interrupt and context profiles. The script
checks the build, software evidence and frozen image with the frozen sources'
own loaders, then writes rationale, recovery, DDR, platform and admission
records for the survey's instance, copies the prepared bundle with a fresh
reservation, epoch, generation and lease, and prints the bundle-run.py command.
The coordinator writes REVIEW after reading root-review-draft.json; CONTINUITY
lists every record of this powered lifetime from the DDR initialization
onwards, in order."""
import argparse
import hashlib
import importlib.util
import json
import secrets
import shlex
import shutil
from pathlib import Path

BINDING = json.loads((Path(__file__).resolve().parent / "binding.json").read_text())
R = Path(BINDING["source"])
H = Path.home() / ".cache/sunstoneos"
p = argparse.ArgumentParser(description=__doc__)
for name in ("bundle", "survey", "root_review", "output"):
    p.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
p.add_argument("--port", required=True, choices=("3-1", "3-3"))
p.add_argument("--continuity", type=Path, action="append", required=True)
a = p.parse_args()
if a.port != "3-3":
    raise SystemExit("Refused: port 3-1's speed bin, oscillator and CPU supplies have not been read; "
                     "the top-point workload binds port 3-3 only")
F, P = a.bundle.resolve(), a.output.resolve()
sha = lambda x: hashlib.sha256(Path(x).read_bytes()).hexdigest()
ref = lambda x: {"path": str(Path(x).resolve()), "sha256": sha(x)}
read = lambda x: json.loads(Path(x).read_text())
def save(x, v): Path(x).write_text(json.dumps(v, indent=2) + "\n")
def require(condition, message):
    """A refusal that python -O cannot remove."""
    if not condition:
        raise SystemExit(f"Refused: {message}")
def bundle26_probe():
    """The probe.bin SHA-256 that bundle 26's run record must name, from its frozen row."""
    row = Path(BINDING["bundle26"]["row"])
    listed = {Path(x["path"]).relative_to(row / "bundle").as_posix(): x["sha256"] for x in read(row / "artifacts.json")}
    probe, elf = BINDING["bundle26"]["probe_sha256"], BINDING["bundle26"]["elf_sha256"]
    require(listed.get("probe.bin") == probe == sha(row / "bundle/probe.bin"),
            "Bundle 26's frozen probe.bin differs")
    require(listed.get("image.elf") == elf == sha(row / "bundle/image.elf"),
            "Bundle 26's frozen image.elf differs")
    require(probe != elf, "Bundle 26's pinned probe.bin and image.elf hashes are equal")
    return probe
def module(n, path):
    s = importlib.util.spec_from_file_location(n, path); m = importlib.util.module_from_spec(s); s.loader.exec_module(m); return m

runner = module("cpu_runner", R / "tools/run-native-startup-probe.py")
s = runner.supervised
manifest_raw = (F / "build/manifest.json").read_bytes(); manifest = json.loads(manifest_raw)
schema = manifest["schema"]
deadline = "workload_bisect" in manifest
cpu0 = schema == "a733-primary-cache-probe-v1"
hosts = {"a733-all-core-pre-scheduler-probe-v1": "all-core-pre-scheduler-host.py",
         "a733-all-core-first-task-probe-v1": "all-core-first-task-host.py",
         "a733-all-core-task-switch-probe-v1": "all-core-task-switch-host.py",
         "a733-all-core-task-workload-probe-v1": "workload-bisect-host.py" if deadline else "all-core-task-workload-host.py",
         "a733-primary-cache-probe-v1": "primary-cache-host.py"}
require(schema in hosts, schema)
host = module("cpu_host", R / "tools" / hosts[schema])
if deadline:
    quiet = manifest["workload_bisect"]["quiet_background"]
    what = ("the complete returning workload: 32 floating-point rounds per core, tick and SGI handlers, background tasks"
            + (" waiting in WFE between increments" if quiet else "") + ", with a CPU0 return deadline")
    if "top_opp" in manifest:
        what += (", after CPU0 relocks CPU-B to 2002 MHz, CPU-L to 1794 MHz and the DSU to 1274 MHz before any secondary"
                 " starts, and lowers CPU-B to 1014 MHz again after every secondary is OFF, before the return")
elif cpu0:
    what = ("CPU0 production IRQ register save and restore with owned-page cleaning" if manifest.get("context") else
            "CPU0 timer PPI 30 and self SGI 0 delivery with bounded drain" if manifest.get("interrupts") else
            "CPU0 production local GIC setup, cached EL1 entry and awake restoration")
else:
    what = {"a733-all-core-pre-scheduler-probe-v1": "production pre-scheduler on all eight cores: power-up, cached EL1 entry, production barrier and 256 atomic increments",
            "a733-all-core-first-task-probe-v1": "first FreeRTOS task-context entry on all eight cores, after the pre-release admission",
            "a733-all-core-task-switch-probe-v1": "native idle yields and worker selection on all eight cores, stopping before worker bodies",
            "a733-all-core-task-workload-probe-v1": "the complete returning workload: 32 floating-point rounds per core, tick and SGI handlers, background tasks"}[schema]
T = F / "template"
s.validate_bundle(T / "prepared")
host.load(F / "build", T / "prepared/image.elf", manifest)
host.validate_software(read(F / "host/software.json"), hashlib.sha256(manifest_raw).hexdigest())
survey = read(a.survey)
identity, node = survey["identity"], survey["node"]
require(survey["port"] == a.port and survey["complete"] and survey["sram_restored"] and survey["rom_usable"],
        "The survey is of another port, incomplete, or left SRAM or the ROM unusable")
require(all(survey["result"]["launcher_gates"].values()),
        "Launcher gates fail; run the cache-off helper and a new survey")
runner.caller_interval(survey)
for x in a.continuity:
    require(read(x).get("complete") is True, f"Continuity record incomplete: {x}")
if "top_opp" in manifest:
    # The relock needs DCDC3 1050 mV and DCDC5 1000 mV, which no part of this
    # launch reads: bundle 26 read them after a ROM return, as the last image
    # before it. Captures and reads without a kernel log may follow it.
    images = [Path(x) for x in a.continuity if (Path(x).parent / "kernel.log").is_file()]
    text = (images[-1].parent / "kernel.log").read_text(errors="replace") if images else ""
    require("COMPLETE board bundle 31: DCDC5 1000 mV and DCDC3 1050 mV kept across the ROM return, read only" in text
            and "SUPPLY dcdc5_uv=1000000 dcdc3_uv=1050000" in text,
            "The last image of the continuity records is not bundle 26's pass")
    b26 = read(images[-1])
    require(b26.get("image_sha256") == bundle26_probe() and b26.get("port") == a.port and b26.get("complete") is True and
            b26.get("transport_failed") is False,
            "The bundle 26 record names another image or port, or did not complete")
    # One lifetime of this port: one session directory, and an unbroken chain
    # of USB instances from the first record that names one to the survey.
    home = Path(a.survey).resolve().parent.parent
    require(all(Path(x).resolve().parent.parent == home for x in a.continuity),
            "A continuity record lies outside the survey's session directory")
    current = None
    for x in a.continuity:
        r = read(x)
        require(r.get("port", a.port) == a.port, f"Continuity record of another port: {x}")
        if "initial_identity" in r:
            start = (r["initial_node"], r["initial_identity"])
            require(current in (None, start), f"Launch not on the instance of the record before it: {x}")
            current = (r["returned_node"], r["returned_identity"])
        elif "identity" in r and "node" in r:
            here = (r["node"], r["identity"])
            require(current in (None, here), f"Record on another USB instance: {x}")
            current = here
    require((b26["returned_node"], b26["returned_identity"]) == (node, identity) == current,
            "The survey is not on the instance that bundle 26's run returned to")
require(P.name.endswith("-prepare"), "Name the output <stem>-prepare")
review = read(a.root_review)
require(review.get("status", "").startswith("Coordinator reviewed"),
        "Root review record must state the coordinator's review")
# Every check above runs before the output exists. A failure after this point
# leaves a partial <stem>-prepare; a rerun first moves it to the trash.
P.mkdir(parents=True, exist_ok=False)
proof = [a.survey, a.root_review, F / "build/manifest.json", F / "host/software.json", *a.continuity]
if cpu0:
    ownership = (f"The coordinator reserves 0x41000000..0x42200000 and port {a.port}. CPU0 runs alone: the diagnostic releases no "
                 "secondary. The runner saves and restores affected DRAM; the returned ROM owns live SRAM.")
    cleaning = "Each CPU0 diagnostic cleans the Normal-memory lines it dirtied before cache disable."
elif deadline:
    ownership = (f"The coordinator reserves 0x41000000..0x42200000 and port {a.port}. The coordinator must establish from current captured "
                 "state and continuity that secondaries are OFF and idle and their redistributors are asleep and quiet. A completed run "
                 "restores the saved DRAM. A deadline return leaves the secondaries running or parked: the runner reads the record and the "
                 "owned memory, restores nothing, keeps ownership and requires a power removal. The returned ROM owns live SRAM.")
    cleaning = "The diagnostic cleans the owned writable range on all eight cores after every writer stops."
else:
    ownership = (f"The coordinator reserves 0x41000000..0x42200000 and port {a.port}. The coordinator must establish from current captured "
                 "state and continuity that secondaries are OFF and idle and their redistributors are asleep and quiet. The runner saves "
                 "and restores affected DRAM; the returned ROM owns live SRAM.")
    cleaning = "Each core cleans the Normal-memory lines it owns after every writer stops."
conditions = {
    "source-backed-cpu-dsu-transition": f"Coordinator reviewed {what}." + (" CPU0 relocks the three CPU PLLs with the CPU clock driver's sequence and refuses before any write on any difference; DCDC3 1050 mV and DCDC5 1000 mV were set by bundle 25 and read back by bundle 26 in this lifetime, the last image before this one, on the USB instance chain that ends at this survey; the relock reads the oscillator, the speed bin and both rails over S_TWI0 itself and refuses before any clock write unless they match." if "top_opp" in manifest else "") + f" The native image is the refrozen 6fd7af86 built from work/gpu-vendor-default-1025 at cc97b957, which makes the EL3 security writes; the overlay and native payload segments match the instruction-check build, and host evidence was generated from the frozen sources this script loads.",
    "cold-cache-and-dirty-alias-exclusion": "DDR was initialized cold in this powered lifetime (first continuity record). The current cache-off helper and survey establish M, C and I off. " + cleaning,
    "dual-pas-dram": "Secure and Nonsecure DDR access continue in this powered lifetime; no full-memory or DMA reachability claim.",
    "exclusive-cpus-memory-dma": ownership,
    "remote-writers-excluded": "Only the trusted ROM, cold DDR preparation and the steps in the continuity records ran in this lifetime. No auxiliary firmware or peripheral DMA workload was loaded.",
    "usb-clock-power-reset-ownership": "ROM PIO remains the only USB transport; no native USB, PMIC or supply application is constructed. All target shutdown and interrupt restoration precede the RMR return. Stop on the first transport uncertainty.",
    "usb-device-mapping": "The overlay never resumes full-image application startup. Native device tasks are unreachable; the host owns ROM PIO through the queue and port lock.",
    "physical-counter-continuity": "The current survey establishes a monotonic counter. Startup uses the 24 MHz counter and bounded absolute deadlines. No performance claim."}
limits = ["Timing, coherence and interrupt delivery are modeled in the instruction checks; this attempt is the physical test.",
          "One attempt; remove every power source after a non-return or other uncertain failure. Preserve both microSD cards."]
old = {n: read(T / n) for n in ("rationale.json", "recovery.json", "ddr.json", "platform.json", "admission.json")}
rationale = dict(old["rationale.json"], identity=identity, port=a.port, conditions=conditions,
                 evidence=[ref(x) for x in proof], limits=limits)
save(P / "rationale.json", rationale)
save(P / "recovery.json", dict(old["recovery.json"], identity=identity, port=a.port, node=node,
     scope=f"One returning diagnostic ({what}) on port {a.port}; remove every power source after uncertainty.",
     evidence=[ref(a.survey), ref(P / "rationale.json")] + [ref(x) for x in a.continuity]))
save(P / "ddr.json", dict(old["ddr.json"], identity=identity, port=a.port,
     evidence=[ref(x) for x in proof] + [ref(P / "rationale.json")]))
save(P / "platform.json", dict(old["platform.json"], identity=identity, port=a.port,
     conditions={k: ref(P / "rationale.json") for k in old["platform.json"]["conditions"]}))
admission = dict(old["admission.json"], identity=identity, port=a.port, reservation=secrets.randbits(63) or 1,
                 ddr=ref(P / "ddr.json"), platform=ref(P / "platform.json"))
admission["recovery"] = dict(admission["recovery"], evidence=[ref(P / "recovery.json"), ref(P / "rationale.json")])
save(P / "admission.json", admission)
s.validate_admission(admission, admission["image_sha256"], identity, a.port)
# The frozen template is read-only: copy contents only, so that every copied
# file and directory is new and writable, then replace the admission record.
B = P / "prepared"
for x in sorted((T / "prepared").rglob("*")):
    y = B / x.relative_to(T / "prepared")
    if x.is_dir():
        y.mkdir(parents=True, exist_ok=False)
    else:
        y.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(x, y)
shutil.copyfile(P / "admission.json", B / "admission.json")
m = read(B / "prepared.json")
segments, address, build_id = s.image_parts(B / "image.elf")
epoch, generation, lease = (secrets.randbits(63) or 1 for _ in range(3))
ah = s.digest(B / "admission.json")
request, contract = s.request_bytes(address, build_id, epoch, generation, lease, admission["reservation"], bytes.fromhex(ah))
(B / "request.bin").write_bytes(request); (B / "contract.bin").write_bytes(contract)
m.update(epoch=epoch, generation=generation, lease=lease, reservation=admission["reservation"], admission_sha256=ah)
m["files"] = {n: s.digest(B / n) for n in m["files"]}
save(B / "prepared.json", m)
prepared = s.validate_bundle(B)
require(prepared["image_sha256"] == host.probe.NATIVE and prepared["launcher_sha256"] == runner.LAUNCH,
        "The prepared bundle names another image or launcher")
# The run records beside the preparation, as <stem>-run for an output
# <stem>-prepare, so that a later returning image names it as a warm return.
run = P.parent / (P.name[:-len("-prepare")] + "-run")
expected = ("CPU0 completes the diagnostic and restores its controls; saved DRAM is restored and the ROM answers." if cpu0 else
            "All eight cores complete the diagnostic, clean their owned memory and stop; CPU0 restores its controls, seven secondaries reach OFF, saved DRAM is restored and the ROM answers."
            + (" Otherwise CPU0 returns through its deadline with a record and the board is kept for power removal." if deadline else ""))
command = [str(H / "venv/bin/python"), "-B", str(H / "evidence/cpu-diagnosis-20261024/common/bundle-run.py"),
           a.port, str(run), "300", f"Board {a.port}: returning diagnostic, {what}", expected,
           "--prerequisite", f"supervised-ram-recovery={P / 'recovery.json'}",
           "--prerequisite", f"native-startup-software={F / 'host/software.json'}",
           "--artifact", str(F / "build/manifest.json"), "--artifact", str(a.survey.resolve()), "--artifact", str(B / "prepared.json"),
           "--artifact", str(P / "rationale.json"), "--artifact", str(a.root_review.resolve()),
           "--artifact", str(R / "tools/run-native-startup-probe.py"), "--artifact", str(R / "tools" / hosts[schema]),
           "--", str(H / "venv/bin/python"), "-B", str(R / "tools/run-native-startup-probe.py"), "run",
           "--build", str(F / "build"), "--bundle", str(B), "--software", str(F / "host/software.json"),
           "--survey", str(a.survey.resolve()), "--database", "{database}",
           "--fel", str(H / "build/sunxi-a733/sunxi-fel"), "--evidence", str(run), "--claim", "{claim}", "--owner", "{owner}"]
save(P / "command.json", command)
save(P / "offline-preparation.json", {"bundle_validated": True, "admission_validated": True, "software_validated": True,
     "request_regenerated": True, "identity": identity, "node": node, "port": a.port, "physical_access": False})
print(shlex.join(command))
