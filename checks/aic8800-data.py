#!/usr/bin/env python3
"""Compare native packets with pinned descriptors, then run only the data model."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
REFERENCES = {'ipc_shared.h': 'b5ba7dfa5b1cad22edfecc35b0a52e91c6d3e9568ecf2070115889f6da0b1111', 'rwnx_rx.h': '4e9322ae9da8bd7e41da60104e3760b3ee822ff421635242898507e9f0d72703', 'rwnx_tx.c': '900f56ddebcad3a52721860f6c167e75f40a48730a8104f6bbb9d4a17b8e31e4', 'rwnx_rx.c': 'a3bfd89f7f56d6eecadacd469d3ee31631b1194f114b308695d8e19b7c39484c', 'aicwf_sdio.c': '2d4bd746a042c521237fd23bc55c5b452e25f563ab3592385c5b7671d94287a4', 'aicwf_txrxif.c': '2c22a400aa25e38fa908d0f43313081811aa8b988641464555ed1643585e8ce4', 'sdio_host.c': 'fd9639318b326bac4969bc969eca40d1895b2a9825b0fbc5cd3033bccce08435', 'hal_desc.h': 'ac19cf6a29174a4db81b0662d110914b1a03dd5746d7b69f37ee0cf4e51bc39e', 'lmac_mac.h': '90dae163c4350a7786194c4b1f12f7e1d7012cdf1ffda683ff6f116d72bcddce', 'Makefile': '447e4982d090a6587ef1c321eb6450c8e683a71c204acec3e9df34fc57bbcd03'}
SOURCES = ["checks/aic8800-data.c", "drivers/net/aic8800_data.c", "drivers/net/aic8800_wifi.c",
           "drivers/net/aic8800_runtime.c", "drivers/net/aic8800_firmware.c", "drivers/net/aic8800_sdio_transport.c",
           "drivers/net/aic8800_sdio_codec.c", "drivers/storage/sunxi_sdio.c", "drivers/storage/sdio_card.c"]
INPUTS = ["include/aic8800_data.h", "include/aic8800_wifi.h", "include/aic8800_runtime.h", "include/aic8800_firmware.h",
          "include/aic8800_sdio_transport.h", "include/aic8800_sdio_codec.h", "include/sunxi_sdio.h", "include/sdio_card.h",
          "checks/aic8800-runtime.c", "checks/aic8800-sdio-transport.c", "checks/sdio-controller.c", "checks/aic8800-data.py"]
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    a = parser.parse_args(); out = a.evidence.resolve(); out.mkdir(parents=True, exist_ok=True)
    for name, wanted in REFERENCES.items(): assert digest(a.reference_dir / name) == wanted, name
    ipc = (a.reference_dir / "ipc_shared.h").read_text()
    rx = (a.reference_dir / "rwnx_rx.h").read_text()
    # Exact source declarations compile outside Git with minimal scalar typedefs.
    declarations = ipc[ipc.index("struct hostdesc {"):ipc.index("struct txdesc_host {")]
    declarations += rx[rx.index("struct rx_vector_1_old {"):rx.index("struct rwnx_legrate {")]
    prefix = r'''#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef int32_t s32;typedef uint32_t __le32;
typedef uint8_t u8_l;typedef uint16_t u16_l;typedef uint32_t u32_l;
#define __packed __attribute__((packed))
#define CONFIG_RWNX_FULLMAC 1
#define AICWF_SDIO_SUPPORT 1
#define AICWF_RX_REORDER 1
struct mac_addr {u16_l array[3];};
'''
    body = r'''
static void write_file(const char *name,const void *p,size_t n) {
    FILE *f=fopen(name,"wb");assert(f);assert(fwrite(p,1,n,f)==n);assert(!fclose(f));
}
int main(int argc,char **argv) {
    assert(argc==3);assert(sizeof(struct txdesc_api)==28);assert(sizeof(struct hw_rxhdr)==56);
    assert(offsetof(struct hostdesc,hostid)==4&&offsetof(struct hostdesc,flags)==26);
    uint8_t packets[2][1536]={{0}},mac[]={2,1,2,3,4,5},ap[]={2,0x11,0x22,0x33,0x44,0x55};
    for(unsigned ordinary=0;ordinary<2;++ordinary) {
        uint8_t *p=packets[ordinary];unsigned payload=ordinary?1500:4,length=28+payload;
        p[0]=(uint8_t)length;p[1]=(uint8_t)(length>>8);p[2]=1;
        uint8_t crc=0;for(unsigned i=0;i<3;++i){crc^=p[i];for(unsigned j=0;j<8;++j)crc=(uint8_t)((crc<<1)^((crc&128)?7:0));}p[3]=crc;
        struct txdesc_api d={0};d.host.packet_len=payload;d.host.hostid=ordinary?0:0x80000000u;
        memcpy(&d.host.eth_dest_addr,ap,6);memcpy(&d.host.eth_src_addr,mac,6);
        ((uint8_t *)&d.host.ethertype)[0]=ordinary?8:0x88;((uint8_t *)&d.host.ethertype)[1]=ordinary?0:0x8e;
        d.host.ac=1;d.host.tid=0xff;d.host.vif_idx=1;d.host.staid=3;memcpy(p+4,&d,sizeof(d));
        if(ordinary)for(unsigned i=0;i<payload;++i)p[32+i]=(uint8_t)(i+14);
        else {p[32]=2;p[33]=1;}
    }
    write_file(argv[1],packets,sizeof(packets));
    uint8_t record[128]={0};struct hw_rxhdr h={0};h.hwvect.len=36;
    h.hwvect.frm_successful_rx=1;h.hwvect.desc_done_rx=1;h.flags_upload=1;
    h.flags_vif_idx=1;h.flags_sta_idx=3;h.flags_dst_idx=0xff;
    h.pattern=0xaabbccdd;memcpy(record,&h,sizeof(h));
    record[60]=8;record[61]=2;memcpy(record+64,mac,6);memcpy(record+70,ap,6);memcpy(record+76,ap,6);
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x88,0x8e,2,1,0,0};memcpy(record+84,snap,sizeof(snap));
    write_file(argv[2],record,sizeof(record));return 0;
}
'''
    wrapper = out / "reference.c"; wrapper.write_text(prefix + declarations + body)
    reference = out / "reference"
    refcmd = [a.cc,"-std=c11","-O2",str(wrapper),"-o",str(reference)]
    subprocess.run(refcmd,check=True,capture_output=True)
    subprocess.run([str(reference),str(out/"packets.bin"),str(out/"rx.bin")],check=True)
    executable = out / "data-check"
    cmd = [a.cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
           "-fno-omit-frame-pointer","-I",str(ROOT/"include")]+[str(ROOT/n) for n in SOURCES]+["-o",str(executable)]
    build = subprocess.run(cmd,capture_output=True,text=True)
    result = {"build_command":cmd,"build_exit_code":build.returncode,"build_stderr":build.stderr,
              "reference_command":refcmd,"references":REFERENCES,
              "sources":{n:digest(ROOT/n) for n in SOURCES+INPUTS},
              "artifacts":{n:digest(out/n) for n in ["reference.c","reference","packets.bin","rx.bin"]}}
    if not build.returncode:
        run_cmd=[str(executable),str(out/"packets.bin"),str(out/"rx.bin")]
        run=subprocess.run(run_cmd,capture_output=True,text=True)
        result.update(run_command=run_cmd,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,
                      executable_sha256=digest(executable))
    result["result"]="passed" if not build.returncode and result.get("exit_code")==0 else "failed"
    (out/"results.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:result.get(k) for k in ["result","build_stderr","stdout","stderr"]}))
    raise SystemExit(result["result"]!="passed")
if __name__=="__main__":main()
