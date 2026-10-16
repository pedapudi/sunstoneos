#!/usr/bin/env python3
"""Compare binding with pinned source and check bounded native receive ownership."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REFERENCES = {
    "aic8800_bsp/aic_bsp_verify.c": "76ccd57c0ddb2f2c5b074575f709d4816a0dd7a0fbb3ba9cfa763d69d1b76ed1",
    "aic8800_bsp/aic_bsp_driver.c": "43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e",
    "aic8800_bsp/aic_bsp_driver.h": "9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3",
    "aic8800_bsp/aic_bsp_8800d80.c": "fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8",
    "aic8800_fdrv/aicwf_txrxif.c": "2c22a400aa25e38fa908d0f43313081811aa8b988641464555ed1643585e8ce4",
    "aic8800_fdrv/aicwf_txrxif.h": "d70fb2f1382b5612d1efccb04fb59a7a56183765d05b731637c2c241587ad69a",
}
SOURCES = ["checks/aic8800-runtime.c", "drivers/net/aic8800_runtime.c",
           "drivers/net/aic8800_firmware.c", "drivers/net/aic8800_sdio_transport.c",
           "drivers/net/aic8800_sdio_codec.c", "drivers/storage/sunxi_sdio.c", "drivers/storage/sdio_card.c"]
INCLUDES = ["include/aic8800_runtime.h", "include/aic8800_firmware.h", "include/aic8800_sdio_transport.h",
            "include/aic8800_sdio_codec.h", "include/sdio_card.h", "include/sunxi_sdio.h",
            "checks/aic8800-sdio-transport.c", "checks/sdio-controller.c"]
SEEDS = [bytes.fromhex(x) for x in ["00000000", "ffffffff", "12345678", "00010203",
                                     "deadbeef", "80017ffe", "5500aaff", "7f808182"]]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def maj(a, b, c):
    return (a & b) ^ (a & c) ^ (b & c)


def choose(a, b, c):
    return (a & b) ^ ((~a & 255) & c)


def expanded(data):
    h = hashlib.sha256(data).digest()
    return b"".join(h[8*i:8*i+8] + data[i:i+1] for i in range(4))


def confuse(data):
    m = expanded(data)
    head = bytes(choose(*m[i:i+3]) for i in range(0, 36, 3))
    return head + bytes(maj(*head[i*3:i*3+3]) ^ data[i] for i in range(4))


def expected():
    rows = []
    for seed in SEEDS:
        m = expanded(seed)
        mixed = bytes(maj(*m[i:i+3]) for i in range(0, 36, 3))
        encrypted = bytes(choose(*mixed[i:i+3]) for i in range(0, 12, 3))
        rows.append(confuse(seed) + confuse(encrypted))
    return b"".join(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--reference-dir", type=Path, required=True, help="Pinned parent of aic8800_bsp and aic8800_fdrv")
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--cc", default="cc")
    a = p.parse_args()
    out = a.evidence.resolve(); out.mkdir(parents=True, exist_ok=True)
    for name, wanted in REFERENCES.items():
        assert digest(a.reference_dir / name) == wanted, name
    shim = out / "shim/linux"; shim.mkdir(parents=True, exist_ok=True)
    (shim / "kernel.h").write_text("#include <stdint.h>\n#include <sys/types.h>\n")
    (shim / "string.h").write_text("#include <string.h>\n")
    # Include the unmodified pinned implementation; the host stub supplies its
    # required successful platform identity function. No board identity is read.
    ref = out / "reference.c"
    ref.write_text('#include <stdio.h>\n#include <stdint.h>\n#include <assert.h>\n'
                   '#include "aic_bsp_verify.c"\n'
                   'int sunxi_get_soc_chipid(uint8_t *id) { for(int i=0;i<16;++i)id[i]=(uint8_t)i;return 0; }\n'
                   'int main(void) { uint8_t seeds[8][4] = {' +
                   ','.join('{' + ','.join(str(n) for n in seed) + '}' for seed in SEEDS) +
                   '}; uint8_t c[16],r[16]; for(unsigned i=0;i<8;++i) {'
                   'assert(!data_confucion(seeds[i],c));assert(!wcn_bind_verify_calculate_verify_data(c,r));'
                   'assert(fwrite(c,1,16,stdout)==16);assert(fwrite(r,1,16,stdout)==16); } return 0; }\n')
    ref_exe = out / "binding-reference"
    ref_cmd = [a.cc, "-std=c11", "-O2", "-I", str(out / "shim"), "-I",
               str(a.reference_dir / "aic8800_bsp"), str(ref), "-o", str(ref_exe)]
    subprocess.run(ref_cmd, check=True, capture_output=True)
    result_bytes = subprocess.check_output([str(ref_exe)])
    assert result_bytes == expected(), "Pinned binding source differs from independent SHA-256 vectors"
    vectors = out / "binding-vectors.bin"; vectors.write_bytes(result_bytes)
    executable = out / "aic8800-runtime-check"
    cmd = [a.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include")]
    cmd += [str(ROOT / name) for name in SOURCES] + ["-o", str(executable)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    run = None
    if not build.returncode:
        run = subprocess.run([str(executable), str(vectors)], capture_output=True, text=True)
    record = {"result": "passed" if run and not run.returncode else "failed", "command": cmd,
              "build_exit_code": build.returncode, "build_stderr": build.stderr,
              "reference_command": ref_cmd, "reference_executable_sha256": digest(ref_exe),
              "reference_wrapper_sha256": digest(ref), "references": REFERENCES,
              "vectors_sha256": digest(vectors), "sources": {n: digest(ROOT / n) for n in SOURCES + INCLUDES},
              "runner_sha256": digest(Path(__file__)),
              "compiler": subprocess.check_output([a.cc, "--version"], text=True).splitlines()[0]}
    if run:
        record.update({"run_command": [str(executable), str(vectors)], "exit_code": run.returncode,
                       "stdout": run.stdout, "stderr": run.stderr, "executable_sha256": digest(executable)})
    (out / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"result": record["result"], "stdout": record.get("stdout", ""),
                      "stderr": record.get("stderr", build.stderr), "evidence": str(out)}))
    raise SystemExit(0 if record["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
