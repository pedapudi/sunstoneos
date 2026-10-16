#!/usr/bin/env python3
"""Authenticate external RSN files and optionally compile/link the native closure."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("rsn_recipe", ROOT / "checks/aic8800-supplicant.py")
recipe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipe)
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def verify(up):
    archive = up.parent / (up.name + ".tar.gz")
    assert digest(archive) == recipe.ARCHIVE_SHA256
    hashes = {}
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            path = Path(member.name).relative_to(up.name)
            if member.isfile() and (path.parts[0] == "src" or str(path) in ["COPYING", "wpa_supplicant/README"]):
                data = tar.extractfile(member).read()
                assert (up / path).read_bytes() == data, str(path)
                hashes[str(path)] = hashlib.sha256(data).hexdigest()
    # Inspect the actual archive's relevant mitigation checks; the version
    # string alone is not a security claim. MLO operation is excluded here.
    wpa = (up / "src/rsn_supp/wpa.c").read_text()
    cache = (up / "src/rsn_supp/pmksa_cache.c").read_text()
    common = (up / "src/common/ieee802_11_common.c").read_text()
    assert "sm->network_ctx, sm->key_mgmt" in wpa
    assert "(!akmp || akmp == entry->akmp)" in cache
    assert "network_ctx == entry->network_ctx" in cache
    fn = common[common.index("int get_basic_mle_link_id("):]
    assert "link_id >= MAX_NUM_MLD_LINKS" in fn
    assert "link_id_pos + EHT_ML_LINK_ID_LEN > common_info_limit" in fn
    return hashes

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--upstream", type=Path, required=True)
    p.add_argument("--evidence", type=Path)
    p.add_argument("--native", action="store_true")
    p.add_argument("--resume", action="store_true", help="Reuse matching compiled objects after an incomplete build")
    p.add_argument("--cc", default="clang")
    p.add_argument("--linker", type=Path, help="AArch64-capable linker used by clang")
    a = p.parse_args(); up = a.upstream.resolve(); hashes = verify(up)
    if not a.native:
        print("Pinned RSN release source and mitigation checks match")
        return
    assert a.evidence
    out = a.evidence.resolve(); out.mkdir(parents=True, exist_ok=True)
    previous = json.loads((out / "results.json").read_text()) if a.resume else {}
    result = {"upstream_sha256": hashes, "archive_sha256": recipe.ARCHIVE_SHA256, "commands": [], "native_sha256": {}}
    flags = ["--target=aarch64-none-elf", "-std=c11", "-O2", "-ffreestanding", "-fno-builtin",
             "-fno-stack-protector", "-mgeneral-regs-only", "-ffunction-sections", "-fdata-sections",
             "-DSUNSTONE_AIC8800_RSN", "-I", str(ROOT / "include"), "-I", str(up / "src"), "-I", str(up / "src/utils")]
    sources = [ROOT / n for n in recipe.NATIVE + ["runtime/memory.c", "runtime/memmove.c", "checks/aic8800-security-native.c"]]
    sources += [up / "src" / n for n in recipe.EXTERNAL]
    objects = []
    for source in sources:
        external = source.is_relative_to(up)
        obj = out / (str(source.relative_to(up if external else ROOT)).replace("/", "_") + ".o")
        cmd = [a.cc] + flags
        if source.parent == ROOT / "runtime": cmd += ["-I", str(ROOT / "runtime/include")]
        if external: cmd += ["-include", str(ROOT / "include/aic8800_supplicant_port.h")]
        else: cmd += ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
        cmd += ["-c", str(source), "-o", str(obj)]
        prior = next((c for c in previous.get("commands", []) if c["command"] == cmd and c["exit_code"] == 0), None)
        expected_source = previous.get("upstream_sha256" if external else "native_sha256", {}).get(str(source.relative_to(up if external else ROOT)))
        unchanged_headers = previous.get("headers_sha256") == {str(p.relative_to(ROOT)): digest(p) for p in (ROOT / "include").glob("*.h")}
        reused = bool(prior and unchanged_headers and expected_source == digest(source) and obj.exists() and previous.get("objects_sha256", {}).get(obj.name) == digest(obj))
        run = subprocess.CompletedProcess(cmd, 0, "", "") if reused else subprocess.run(cmd, capture_output=True, text=True)
        result["commands"].append({"command": cmd, "exit_code": run.returncode, "stderr": run.stderr, "reused": reused})
        if not external: result["native_sha256"][str(source.relative_to(ROOT))] = digest(source)
        if run.returncode: break
        objects.append(str(obj))
    if not run.returncode:
        elf = out / "security.elf"
        cmd = [a.cc] + (["--ld-path=" + str(a.linker)] if a.linker else []) + ["--target=aarch64-none-elf", "-nostdlib", "-Wl,--gc-sections", "-Wl,-e,security_link_entry"] + objects + ["-o", str(elf)]
        run = subprocess.run(cmd, capture_output=True, text=True)
        result["commands"].append({"command": cmd, "exit_code": run.returncode, "stderr": run.stderr})
        if not run.returncode: result["elf_sha256"] = digest(elf)
    result["headers_sha256"] = {str(p.relative_to(ROOT)): digest(p) for p in (ROOT / "include").glob("*.h")}
    result["objects_sha256"] = {Path(p).name: digest(Path(p)) for p in objects}
    result.update(result="passed" if not run.returncode else "failed", exit_code=run.returncode)
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"], run.stderr)
    raise SystemExit(run.returncode != 0)
if __name__ == "__main__": main()
