#!/usr/bin/env python3
"""Check Wi-Fi management wire bytes against pinned full-MAC headers and source."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REFERENCES = {
    "lmac_msg.h": "916df5d5a2346a6504b35bad2fc221233e959a4a68de3d1bd61b1df6dfaf12d3",
    "lmac_mac.h": "90dae163c4350a7786194c4b1f12f7e1d7012cdf1ffda683ff6f116d72bcddce",
    "lmac_types.h": "90a8d7471e1f738cfd951c2cc6e362f06261fe4ed848b554d0a8b5fdcb1a209f",
    "rwnx_msg_tx.c": "755c5c635cbf157f0f5363550ca1023c7f05eeca1b0cb2e0db53dcf9d38dbd91",
    "rwnx_msg_rx.c": "9eed44d33b2f6241171e47648ba67e5d193d0e54558fa9f878e8821afe8f9f2b",
    "rwnx_main.c": "c33c214992334186b4857090600c8f86a0bb92ca62b923c40f3cc51f48966cf8",
    "rwnx_mod_params.c": "ace43d03d693995c4272a1c5360f3ad17b985f004c2e7abefa33e48ef856dda6",
    "rwnx_platform.c": "39611e9107beaf67be408fe6c249ab60b7bf75583190d6bd851f9c41027d795e",
    "aicwf_8800d80.c": "b102320fe7ffd98f51c04bd419e9359f78fd181bd3e71e0aaa2d3a6bcf5b2cb5",
    "Makefile": "447e4982d090a6587ef1c321eb6450c8e683a71c204acec3e9df34fc57bbcd03",
}
SOURCES = ["checks/aic8800-wifi.c", "drivers/net/aic8800_wifi.c", "drivers/net/aic8800_data.c", "drivers/net/aic8800_runtime.c",
           "drivers/net/aic8800_firmware.c", "drivers/net/aic8800_sdio_transport.c",
           "drivers/net/aic8800_sdio_codec.c", "drivers/storage/sunxi_sdio.c", "drivers/storage/sdio_card.c"]
HEADERS = ["include/aic8800_wifi.h", "include/aic8800_data.h", "include/aic8800_runtime.h", "include/aic8800_firmware.h",
           "include/aic8800_sdio_transport.h", "include/aic8800_sdio_codec.h", "include/sunxi_sdio.h",
           "include/sdio_card.h", "checks/aic8800-runtime.c", "checks/aic8800-sdio-transport.c", "checks/sdio-controller.c"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def power_initializer(source):
    start = source.index("{", source.index(".txpwr_lvl_v3 ="))
    depth = 0
    for i in range(start, len(source)):
        depth += (source[i] == "{") - (source[i] == "}")
        if depth == 0:
            return source[start:i+1]
    raise ValueError("Missing complete power profile")


def reference(a, out):
    shim = out / "shim/linux"; shim.mkdir(parents=True, exist_ok=True)
    (shim / "version.h").write_text("#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))\n#define LINUX_VERSION_CODE KERNEL_VERSION(6,1,0)\n")
    (shim / "types.h").write_text("#include <stdint.h>\n#include <stdbool.h>\ntypedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;\n#define __packed __attribute__((packed))\n")
    (shim / "bits.h").write_text("#define BIT(n) (1U<<(n))\n")
    (shim / "ieee80211.h").write_text("")
    initial = power_initializer((a.reference_dir / "rwnx_platform.c").read_text())
    vectors = a.binding_vectors.read_bytes()
    assert len(vectors) == 256
    # The earlier binding source/Python comparison is reused; these bytes merely
    # supply one late-binding event to the new management serialization fixture.
    reply = ','.join(str(v) for v in vectors[16:32])
    source = r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "lmac_msg.h"
static void emit(uint16_t id,const void *p,uint16_t size) {
    uint8_t h[4]={id,id>>8,size,size>>8};assert(fwrite(h,1,4,stdout)==4);
    if(size)assert(fwrite(p,1,size,stdout)==size);
}
int main(void) {
    struct mm_set_txpwr_lvl_req power={0};power.txpwr_lvl_v3=(txpwr_lvl_conf_v3_t)POWER_INITIALIZER;
    emit(MM_SET_TXPWR_IDX_LVL_REQ,&power,sizeof(power));
    struct mm_set_rf_calib_req cal={.cal_cfg_24g=0x0f8f,.cal_cfg_5g=0x0f0f,
        .param_alpha=0x0c34c008,.bt_calib_param=0x264203};emit(MM_SET_RF_CALIB_REQ,&cal,sizeof(cal));
    struct mm_get_mac_addr_req mac={.get=1};emit(MM_GET_MAC_ADDR_REQ,&mac,sizeof(mac));
    emit(MM_RESET_REQ,NULL,0);emit(MM_VERSION_REQ,NULL,0);
    uint8_t binding[16]={BINDING_REPLY};emit(0x0415,binding,16);
    struct me_config_req me={0};me.ht_cap.ht_capa_info=1;me.tx_lft=512;me.phy_bw_max=PHY_CHNL_BW_20;
    emit(ME_CONFIG_REQ,&me,sizeof(me));
    struct me_chan_config_req channels={0};channels.chan2G4_cnt=1;channels.chan5G_cnt=1;
    channels.chan2G4[0]=(struct mac_chan_def){.freq=2412,.band=0,.flags=CHAN_NO_IR,.tx_power=17};
    channels.chan5G[0]=(struct mac_chan_def){.freq=5180,.band=1,.flags=CHAN_NO_IR|CHAN_RADAR,.tx_power=10};
    emit(ME_CHAN_CONFIG_REQ,&channels,sizeof(channels));
    struct mm_start_req start={.uapsd_timeout=300,.lp_clk_accuracy=20};emit(MM_START_REQ,&start,sizeof(start));
    struct mm_add_if_req add={.type=MM_STA};uint8_t addr[6]={2,1,2,3,4,5};memcpy(&add.addr,addr,6);
    emit(MM_ADD_IF_REQ,&add,sizeof(add));
    struct scanu_start_req scan={0};scan.chan[0]=channels.chan2G4[0];scan.chan[1]=channels.chan5G[0];
    memset(&scan.bssid,255,sizeof(scan.bssid));scan.vif_idx=1;scan.chan_cnt=2;
    emit(SCANU_START_REQ,&scan,sizeof(scan));return 0;
}
'''.replace("POWER_INITIALIZER", initial).replace("BINDING_REPLY", reply)
    wrapper = out / "reference.c"; wrapper.write_text(source)
    executable = out / "reference"
    cmd = [a.cc, "-std=c11", "-O2", "-DCONFIG_RWNX_FULLMAC", "-I", str(out / "shim"),
           "-I", str(a.reference_dir), str(wrapper), "-o", str(executable)]
    subprocess.run(cmd, check=True, capture_output=True)
    data = subprocess.check_output([str(executable)]); (out / "parameters.bin").write_bytes(data)
    at = 0; packets = []
    while at < len(data):
        ident, size = struct.unpack_from("<HH", data, at); at += 4
        params = data[at:at+size]; assert len(params) == size; at += size
        payload = bytes(4) + struct.pack("<4H", ident, ident >> 10, 100, size) + params
        header = struct.pack("<HB", len(payload), 0x11); crc = 0
        for byte in header:
            crc ^= byte
            for _ in range(8):
                crc = ((crc << 1) ^ (7 if crc & 128 else 0)) & 255
        packet = header + bytes([crc]) + payload
        assert len(packet) + 4 <= 512
        packets.append(packet.ljust(512, b"\0"))
    assert len(packets) == 11
    transcript = out / "transcript.bin"; transcript.write_bytes(b"".join(packets))
    return {"command": cmd, "wrapper_sha256": digest(wrapper), "executable_sha256": digest(executable),
            "parameter_sha256": digest(out / "parameters.bin"), "transcript_sha256": digest(transcript)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--reference-dir", type=Path, required=True)
    p.add_argument("--binding-vectors", type=Path, required=True)
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--cc", default="cc")
    a = p.parse_args(); out = a.evidence.resolve(); out.mkdir(parents=True, exist_ok=True)
    for name, wanted in REFERENCES.items():
        assert digest(a.reference_dir / name) == wanted, name
    ref = reference(a, out)
    executable = out / "aic8800-wifi-check"
    cmd = [a.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include")]
    cmd += [str(ROOT / name) for name in SOURCES] + ["-o", str(executable)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    record = {"command": cmd, "build_exit_code": build.returncode, "build_stderr": build.stderr,
              "reference": ref, "references": REFERENCES, "binding_vectors_sha256": digest(a.binding_vectors),
              "binding_vectors_path": str(a.binding_vectors), "runner_sha256": digest(Path(__file__)),
              "sources": {n: digest(ROOT / n) for n in SOURCES + HEADERS},
              "compiler": subprocess.check_output([a.cc, "--version"], text=True).splitlines()[0]}
    if not build.returncode:
        run_cmd = [str(executable), str(out / "transcript.bin"), str(a.binding_vectors)]
        run = subprocess.run(run_cmd, capture_output=True, text=True)
        record.update(run_command=run_cmd, exit_code=run.returncode, stdout=run.stdout,
                      stderr=run.stderr, executable_sha256=digest(executable))
    record["result"] = "passed" if not build.returncode and record.get("exit_code") == 0 else "failed"
    (out / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"result": record["result"], "stdout": record.get("stdout", ""),
                      "stderr": record.get("stderr", build.stderr), "evidence": str(out)}))
    raise SystemExit(0 if record["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
