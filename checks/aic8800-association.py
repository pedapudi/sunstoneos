#!/usr/bin/env python3
"""Compare station control with pinned full-MAC ABI, then run focused composition."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("wifi_reference", ROOT / "checks/aic8800-wifi.py")
wifi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wifi)


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
    source = r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "lmac_msg.h"
static void emit(uint16_t id,const void *p,uint16_t size) {
    uint8_t h[4]={id,id>>8,size,size>>8};assert(fwrite(h,1,4,stdout)==4);
    assert(fwrite(p,1,size,stdout)==size);
}
int main(int argc,char **argv) {
    assert(argc==2);
    struct sm_connect_req c={0};c.ssid.length=4;memcpy(c.ssid.array,"test",4);
    uint8_t addr[]={2,0x11,0x22,0x33,0x44,0x55};memcpy(&c.bssid,addr,6);
    c.chan=(struct mac_chan_def){.freq=2412,.band=0,.flags=CHAN_NO_IR,.tx_power=17};
    c.flags=CONTROL_PORT_HOST|CONTROL_PORT_NO_ENC|DISABLE_HT;
    ((uint8_t *)&c.ctrl_port_ethertype)[0]=0x88;((uint8_t *)&c.ctrl_port_ethertype)[1]=0x8e;
    c.vif_idx=1;emit(SM_CONNECT_REQ,&c,sizeof(c));
    /* Selected RSN profile: version1, CCMP group/pairwise, PSK AKM, no PMF. */
    uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    c.flags|=WPA_WPA2_IN_USE;c.ie_len=sizeof(rsn);memcpy(c.ie_buf,rsn,sizeof(rsn));emit(SM_CONNECT_REQ,&c,sizeof(c));
    struct sm_disconnect_req d={.reason_code=3,.vif_idx=1};emit(SM_DISCONNECT_REQ,&d,sizeof(d));
    struct sm_connect_ind ind;memset(&ind,0xaa,sizeof(ind));ind.status_code=0;memcpy(&ind.bssid,addr,6);
    ind.roamed=0;ind.vif_idx=1;ind.ap_idx=3;ind.ch_idx=2;ind.qos=0;ind.acm=0;
    ind.assoc_req_ie_len=6;ind.assoc_rsp_ie_len=3;
    uint8_t ies[]={0,4,'t','e','s','t',1,1,2};memcpy(ind.assoc_ie_buf,ies,sizeof(ies));
    ind.aid=7;ind.band=0;ind.center_freq=2412;ind.width=PHY_CHNL_BW_20;
    ind.center_freq1=2412;ind.center_freq2=0;for(unsigned i=0;i<4;++i)ind.ac_param[i]=0x44332211;
    FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(&ind,1,sizeof(ind),f)==sizeof(ind));assert(!fclose(f));
    assert(sizeof(ind)==852);assert(sizeof(c)==320);assert(sizeof(d)==4);
    assert(sizeof(struct sm_connect_cfm)==1);assert(sizeof(struct sm_disconnect_ind)==6);return 0;
}
'''
    wrapper = out / "reference.c"; wrapper.write_text(source); refexe = out / "reference"
    cmd = [a.cc, "-std=c11", "-O2", "-DCONFIG_RWNX_FULLMAC", "-I", str(out / "shim"),
           "-I", str(a.reference_dir), str(wrapper), "-o", str(refexe)]
    subprocess.run(cmd, check=True, capture_output=True)
    data = subprocess.check_output([str(refexe), str(out / "indication.bin")]); (out / "parameters.bin").write_bytes(data)
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
        packets.append((header + bytes([crc]) + payload).ljust(512, b"\0"))
    assert len(packets) == 3; (out / "packets.bin").write_bytes(b"".join(packets))
    sources = ["checks/aic8800-association.c"] + wifi.SOURCES[1:]
    executable = out / "association-check"
    buildcmd = [a.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "include")]
    buildcmd += [str(ROOT / name) for name in sources] + ["-o", str(executable)]
    build = subprocess.run(buildcmd, capture_output=True, text=True)
    record = {"command": buildcmd, "build_exit_code": build.returncode, "build_stderr": build.stderr,
              "references": wifi.REFERENCES, "reference_command": cmd,
              "reference_artifacts": {n: wifi.digest(out / n) for n in
                                      ["reference.c", "reference", "parameters.bin", "packets.bin", "indication.bin"]},
              "sources": {n: wifi.digest(ROOT / n) for n in sources + wifi.HEADERS},
              "runners": {n: wifi.digest(ROOT / n) for n in ["checks/aic8800-association.py", "checks/aic8800-wifi.py"]},
              "compiler": subprocess.check_output([a.cc, "--version"], text=True).splitlines()[0]}
    if not build.returncode:
        run_cmd = [str(executable), str(out / "packets.bin"), str(out / "indication.bin")]
        run = subprocess.run(run_cmd, capture_output=True, text=True)
        record.update(run_command=run_cmd, exit_code=run.returncode, stdout=run.stdout,
                      stderr=run.stderr, executable_sha256=wifi.digest(executable))
    record["result"] = "passed" if not build.returncode and record.get("exit_code") == 0 else "failed"
    (out / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"result": record["result"], "stdout": record.get("stdout", ""),
                      "stderr": record.get("stderr", build.stderr), "evidence": str(out)}))
    raise SystemExit(record["result"] != "passed")


if __name__ == "__main__":
    main()
