#!/usr/bin/env python3
"""Compare configured station power save with the pinned full-MAC ME_CONFIG ABI."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("wifi_reference", ROOT / "checks/aic8800-wifi.py")
wifi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wifi)
SOURCES = ["checks/aic8800-wifi-power.c", "drivers/net/aic8800_wifi.c", "drivers/net/aic8800_sdio_codec.c"]
HEADERS = ["include/aic8800_wifi.h", "include/aic8800_sdio_codec.h"]
REFERENCE = r'''
#include <stdio.h>
#include <assert.h>
#include "lmac_msg.h"
int main(void) {
    /* ps_on and dpsm as rwnx_send_me_config_req sets them from ps_on/dpsm. */
    for(unsigned mode=0;mode<3;++mode){
        struct me_config_req me={0};me.ht_cap.ht_capa_info=1;me.tx_lft=512;me.phy_bw_max=PHY_CHNL_BW_20;
        me.ps_on=mode!=0;me.dpsm=mode==2;
        assert(sizeof(me)==112);assert(fwrite(&me,1,sizeof(me),stdout)==sizeof(me));
    }
    /* MM_FEAT_PS_BIT gates ps_on in rwnx_handle_dynparams. */
    assert(MM_FEAT_PS_BIT==2&&MM_FEAT_UMAC_BIT==8);return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--reference-dir", type=Path, required=True)
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--cc", default="cc")
    a = p.parse_args(); out = a.evidence.resolve(); out.mkdir(parents=True, exist_ok=True)
    for name, wanted in wifi.REFERENCES.items():
        assert wifi.digest(a.reference_dir / name) == wanted, name
    shim = out / "shim/linux"; shim.mkdir(parents=True, exist_ok=True)
    (shim / "version.h").write_text("#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))\n#define LINUX_VERSION_CODE KERNEL_VERSION(6,1,0)\n")
    (shim / "types.h").write_text("#include <stdint.h>\n#include <stdbool.h>\ntypedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;\n#define __packed __attribute__((packed))\n")
    (shim / "bits.h").write_text("#define BIT(n) (1U<<(n))\n")
    (shim / "ieee80211.h").write_text("")
    wrapper = out / "reference.c"; wrapper.write_text(REFERENCE)
    reference = out / "reference"
    ref_cmd = [a.cc, "-std=c11", "-O2", "-DCONFIG_RWNX_FULLMAC", "-I", str(out / "shim"),
               "-I", str(a.reference_dir), str(wrapper), "-o", str(reference)]
    subprocess.run(ref_cmd, check=True, capture_output=True)
    configs = out / "me-config.bin"; configs.write_bytes(subprocess.check_output([str(reference)]))
    executable = out / "aic8800-wifi-power-check"
    cmd = [a.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include")]
    cmd += [str(ROOT / name) for name in SOURCES] + ["-o", str(executable)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    record = {"command": cmd, "build_exit_code": build.returncode, "build_stderr": build.stderr,
              "reference_command": ref_cmd, "references": wifi.REFERENCES,
              "me_config_sha256": wifi.digest(configs), "runner_sha256": wifi.digest(Path(__file__)),
              "sources": {n: wifi.digest(ROOT / n) for n in SOURCES + HEADERS},
              "compiler": subprocess.check_output([a.cc, "--version"], text=True).splitlines()[0]}
    if not build.returncode:
        run = subprocess.run([str(executable), str(configs)], capture_output=True, text=True)
        record.update(exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr,
                      executable_sha256=wifi.digest(executable))
    record["result"] = "passed" if not build.returncode and record.get("exit_code") == 0 else "failed"
    (out / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"result": record["result"], "stdout": record.get("stdout", ""),
                      "stderr": record.get("stderr", build.stderr), "evidence": str(out)}))
    raise SystemExit(0 if record["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
