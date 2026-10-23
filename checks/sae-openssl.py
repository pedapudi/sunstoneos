#!/usr/bin/env python3
"""Authenticate the pinned OpenSSL source; build its minimal host library and run the SAE cases."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("rsn_recipe", ROOT / "checks/aic8800-supplicant.py")
recipe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipe)
LOCK = json.loads((ROOT / "sources.lock.json").read_text())["openssl"]
# crypto_openssl.c replaces these internal files, as in cmake/openssl.cmake.
INTERNAL = {"crypto/" + n for n in ["sha1.c", "sha1-internal.c", "sha256.c", "sha256-internal.c", "md5.c",
    "md5-internal.c", "aes-internal.c", "aes-internal-enc.c", "aes-internal-dec.c", "aes-wrap.c",
    "aes-unwrap.c", "aes-omac1.c"]}
SAE = ["crypto/crypto_openssl.c", "crypto/dh_groups.c", "crypto/sha256-kdf.c", "crypto/sha384-kdf.c",
    "crypto/sha512-kdf.c", "crypto/sha384-prf.c", "crypto/sha512-prf.c", "common/sae.c", "common/dragonfly.c"]
NATIVE = recipe.NATIVE + ["drivers/net/aic8800_sae.c", "drivers/net/aic8800_openssl.c"]
def digest(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def verify(tree):
    """Compare every archive file with the extracted tree."""
    archive = tree.parent / (tree.name + ".tar.gz")
    assert digest(archive) == LOCK["sha256"], "OpenSSL archive hash differs from sources.lock.json"
    count = 0
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            if member.isfile():
                path = Path(member.name).relative_to(tree.name)
                assert (tree / path).read_bytes() == tar.extractfile(member).read(), str(path)
                count += 1
    return count
def recipe_values():
    """The Configure options and target exactly as cmake/openssl.cmake writes them."""
    text = (ROOT / "cmake/openssl.cmake").read_text()
    options = re.search(r"set\(SUNSTONE_OPENSSL_OPTIONS\s+(.*?)\)", text, re.S).group(1).split()
    defines = re.search(r"set\(SUNSTONE_OPENSSL_DEFINES\s+(.*?)\)", text, re.S).group(1).split()
    conf = re.search(r'sunstone\.conf" CONTENT \[\[\n(.*?)\]\]', text, re.S).group(1)
    name = re.search(r'"(sunstone-[^"]+)" =>', conf).group(1)
    return options, defines, conf, name
def host_library(tree, out, cc):
    """Build libcrypto.a for the host once per identity."""
    options, defines, conf, name = recipe_values()
    build = out / "openssl-host"; build.mkdir(parents=True, exist_ok=True)
    cflags = " ".join(["-O2", "-ffunction-sections", "-fdata-sections"] + defines)
    version = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    identity = "\n".join([LOCK["revision"], LOCK["sha256"], conf, " ".join(options), cc, version, cflags]) + "\n"
    library = build / "tree/libcrypto.a"
    if (build / "identity.txt").is_file() and (build / "identity.txt").read_text() == identity and library.is_file():
        return library, identity, []
    subprocess.run(["rm", "-rf", str(build / "tree")], check=True)
    (build / "tree").mkdir()
    (build / "sunstone.conf").write_text(conf)
    commands = [["perl", str(tree / "Configure"), f"--config={build / 'sunstone.conf'}", name, f"CC={cc}",
                 f"CFLAGS={cflags}"] + options,
                ["make", "-j" + os.environ.get("SUNSTONE_OPENSSL_JOBS", "4"), "build_generated", "libcrypto.a"]]
    log = []
    for cmd in commands:
        run = subprocess.run(cmd, cwd=build / "tree", capture_output=True, text=True)
        log.append({"command": cmd, "exit_code": run.returncode, "stderr": run.stderr[-4000:]})
        if run.returncode:
            raise SystemExit(json.dumps(log[-1], indent=2))
    (build / "identity.txt").write_text(identity)
    return library, identity, log
def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--verify-source", type=Path)
    p.add_argument("--openssl", type=Path); p.add_argument("--upstream", type=Path)
    p.add_argument("--evidence", type=Path); p.add_argument("--cc", default="cc")
    a = p.parse_args()
    if a.verify_source:
        print(f"Pinned OpenSSL source matches ({verify(a.verify_source.resolve())} files)")
        return
    assert a.openssl and a.upstream and a.evidence
    tree = a.openssl.resolve(); up = a.upstream.resolve(); out = a.evidence.resolve(); out.mkdir(parents=True, exist_ok=True)
    result = {"openssl": {"revision": LOCK["revision"], "archive_sha256": LOCK["sha256"], "files": verify(tree)},
              "rsn_archive_sha256": recipe.ARCHIVE_SHA256, "commands": []}
    assert digest(up.parent / (up.name + ".tar.gz")) == recipe.ARCHIVE_SHA256
    library, identity, log = host_library(tree, out, a.cc)
    result.update(openssl_identity=identity, openssl_build=log, openssl_library_sha256=digest(library))
    flags = ["-std=c11", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-DSUNSTONE_AIC8800_RSN", "-DSUNSTONE_AIC8800_SAE", "-I", str(ROOT / "include"),
             "-I", str(up / "src"), "-I", str(up / "src/utils"), "-I", str(library.parent / "include"), "-I", str(tree / "include")]
    external = [n for n in recipe.EXTERNAL if n not in INTERNAL] + SAE
    sources = [ROOT / n for n in NATIVE + ["checks/sae-openssl.c"]] + [up / "src" / n for n in external]
    objects = []; run = None
    for source in sources:
        upstream = source.is_relative_to(up)
        obj = out / (str(source.relative_to(up if upstream else ROOT)).replace("/", "_") + ".o")
        cmd = [a.cc] + flags
        cmd += ["-include", str(ROOT / "include/aic8800_supplicant_port.h"), "-Wno-unused-parameter"] if upstream else \
               ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
        cmd += ["-c", str(source), "-o", str(obj)]
        run = subprocess.run(cmd, capture_output=True, text=True)
        result["commands"].append({"command": cmd, "exit_code": run.returncode, "stderr": run.stderr})
        if run.returncode: break
        objects.append(str(obj))
    runs = {}
    if not run.returncode:
        exe = out / "sae-check"
        cmd = [a.cc, "-fsanitize=address,undefined", "-Wl,--gc-sections"] + objects + [str(library), "-o", str(exe)]
        run = subprocess.run(cmd, capture_output=True, text=True)
        result["commands"].append({"command": cmd, "exit_code": run.returncode, "stderr": run.stderr})
    if not run.returncode:
        result["executable_sha256"] = digest(exe)
        # Faulted owners and the library's global state are kept on purpose.
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        # OpenSSL mixes object addresses into its DRBG as additional input, so
        # the repeat runs disable address randomization.
        fixed = ["setarch", "-R", str(exe)]
        for name, cmd in [("entropy", [str(exe), "entropy"]), ("all", [str(exe), "all"]),
                          ("repeat-a", fixed + ["repeat", "7"]), ("repeat-b", fixed + ["repeat", "7"]),
                          ("repeat-c", fixed + ["repeat", "8"])]:
            run = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=600)
            runs[name] = {"command": cmd, "exit_code": run.returncode, "stdout": run.stdout, "stderr": run.stderr[-4000:]}
            if run.returncode: break
        if not run.returncode:
            # Without a time input the commit depends only on the entropy stream:
            # equal streams give equal commits and a different one differs.
            same = runs["repeat-a"]["stdout"] == runs["repeat-b"]["stdout"]
            differ = runs["repeat-a"]["stdout"] != runs["repeat-c"]["stdout"]
            result["repeatable_commit"] = same and differ
            if not result["repeatable_commit"]: run = subprocess.CompletedProcess([], 1, "", "commit repeatability failed")
    result["runs"] = runs
    names = NATIVE + ["checks/sae-openssl.c", "checks/sae-openssl.py", "cmake/openssl.cmake", "sources.lock.json"]
    result["source_sha256"] = {n: digest(ROOT / n) for n in names}
    result["source_sha256"].update({str(n.relative_to(ROOT)): digest(n) for n in ROOT.glob("include/*.h")})
    result.update(exit_code=run.returncode, result="passed" if not run.returncode else "failed")
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    for name, value in runs.items(): print(f"== {name}\n{value['stdout']}{value['stderr'][-2000:] if value['exit_code'] else ''}")
    if run.returncode and not runs: print(result["commands"][-1]["stderr"][-6000:])
    print(result["result"])
    raise SystemExit(run.returncode != 0)
if __name__ == "__main__": main()
