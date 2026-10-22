#!/usr/bin/env python3
"""Build a pinned BSD RSN composition with independent synthetic AP messages."""
import argparse
import hashlib
import hmac
import json
from pathlib import Path
import struct
import subprocess
ROOT = Path(__file__).resolve().parents[1]
ARCHIVE_SHA256 = "08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6"
EXTERNAL = ["rsn_supp/wpa.c", "rsn_supp/wpa_ie.c", "rsn_supp/pmksa_cache.c",
    "common/wpa_common.c", "common/ieee802_11_common.c", "utils/common.c", "utils/wpabuf.c",
    "crypto/sha1.c", "crypto/sha1-internal.c", "crypto/sha1-prf.c", "crypto/sha256.c",
    "crypto/sha256-internal.c", "crypto/sha256-prf.c", "crypto/md5.c", "crypto/md5-internal.c",
    "crypto/aes-internal.c", "crypto/aes-internal-enc.c", "crypto/aes-internal-dec.c",
    "crypto/aes-wrap.c", "crypto/aes-unwrap.c", "crypto/aes-omac1.c"]
NATIVE = ["drivers/net/aic8800_supplicant.c", "drivers/net/aic8800_supplicant_os.c",
    "drivers/net/aic8800_security.c", "drivers/net/aic8800_data.c", "drivers/net/aic8800_wifi.c",
    "drivers/net/aic8800_runtime.c", "drivers/net/aic8800_firmware.c",
    "drivers/net/aic8800_sdio_transport.c", "drivers/net/aic8800_sdio_codec.c", "drivers/storage/storage_digest.c",
    "drivers/storage/sdio_card.c", "drivers/storage/sunxi_sdio.c"]
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def vectors(path):
    from cryptography.hazmat.primitives.keywrap import aes_key_wrap
    # Synthetic AP only. HMAC and AES wrapping are independent of the C RSN library.
    pmk=bytes(range(32)); anonce=bytes(range(64,96)); snonce=bytes(range(128,160))
    sta=bytes([2,1,2,3,4,5]); ap=bytes([2,17,34,51,68,85])
    seed=min(sta,ap)+max(sta,ap)+min(anonce,snonce)+max(anonce,snonce)
    ptk=b"".join(hmac.digest(pmk,b"Pairwise key expansion\0"+seed+bytes([i]),"sha1") for i in range(4))[:64]
    rsn=bytes([48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0])
    gtk=bytes(range(160,176)); gtk2=bytes(range(176,192))
    def kde(key,keyid): return bytes([221,22,0,15,172,1,keyid,0])+key
    def packet(info,replay,nonce=bytes(32),data=b"",keylen=16,rsc=0):
        p=bytearray(bytes([2,3])+struct.pack(">H",95+len(data))+bytes([2])+struct.pack(">HHQ",info,keylen,replay)+nonce+bytes(16)+rsc.to_bytes(8,"little")+bytes(8)+bytes(16)+struct.pack(">H",len(data))+data)
        if info&0x100: p[81:97]=hmac.digest(ptk[:16],p,"sha1")[:16]
        return bytes(p)
    rows=[pmk,ptk[32:48],gtk,gtk2,rsn,
          packet(0x008a,1,anonce),
          packet(0x010a,1,snonce,rsn,keylen=0),
          packet(0x13ca,2,anonce,aes_key_wrap(ptk[16:32],rsn+kde(gtk,1)+bytes([221,0]))),
          packet(0x030a,2,keylen=0),
          packet(0x1382,3,data=aes_key_wrap(ptk[16:32],kde(gtk2,2)),rsc=5),
          packet(0x0302,3,keylen=0),
          packet(0x13ca,4,anonce,aes_key_wrap(ptk[16:32],rsn+kde(gtk,1)+bytes([221,0]))),
          packet(0x030a,4,keylen=0),
          packet(0x1382,5,data=aes_key_wrap(ptk[16:32],kde(gtk2,2)),rsc=5),
          packet(0x0302,5,keylen=0)]
    with path.open("wb") as f:
        for row in rows: f.write(struct.pack("<H",len(row))+row)
    return {"generator":"Python HMAC-SHA1 and cryptography AES key wrap", "sha256":digest(path)}
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--upstream",type=Path,required=True);p.add_argument("--evidence",type=Path,required=True)
    p.add_argument("--cc",default="cc");p.add_argument("--case",default="lifecycle")
    p.add_argument("--openssl",type=Path,help="pinned OpenSSL tree; run the RSN with crypto_openssl.c as SAE images do")
    a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);up=a.upstream.resolve()
    assert digest(up.parent/(up.name+".tar.gz"))==ARCHIVE_SHA256
    # Pin each consumed source/header to the authenticated release archive.
    import tarfile
    external={}
    with tarfile.open(up.parent/(up.name+".tar.gz")) as archive:
        for member in archive.getmembers():
            relative=Path(member.name).relative_to(up.name)
            if member.isfile() and (relative.parts[0]=="src" or str(relative) in ["COPYING","README"]):
                data=archive.extractfile(member).read();path=up/relative
                assert path.read_bytes()==data,str(relative)
                external[str(relative)]=hashlib.sha256(data).hexdigest()
    result={"archive_sha256":ARCHIVE_SHA256,"upstream":external,"vectors":vectors(out/"vectors.bin"),"commands":[]}
    flags=["-std=c11","-O1","-g","-ffunction-sections","-fdata-sections","-fsanitize=address,undefined",
           "-fno-omit-frame-pointer","-DSUNSTONE_AIC8800_RSN","-I",str(ROOT/"include"),"-I",str(up/"src"),"-I",str(up/"src/utils")]
    native,external,library=NATIVE,EXTERNAL,None
    if a.openssl:
        # Same source selection and host library as checks/sae-openssl.py.
        import importlib.util
        spec=importlib.util.spec_from_file_location("sae_recipe",ROOT/"checks/sae-openssl.py")
        sae=importlib.util.module_from_spec(spec);spec.loader.exec_module(sae)
        tree=a.openssl.resolve();result["openssl_files"]=sae.verify(tree)
        library,identity,_=sae.host_library(tree,out,a.cc);result["openssl_identity"]=identity
        native,external=sae.NATIVE,[n for n in EXTERNAL if n not in sae.INTERNAL]+sae.SAE
        flags+=["-DSUNSTONE_AIC8800_SAE","-I",str(library.parent/"include"),"-I",str(tree/"include")]
    objects=[];result["source_sha256"]={str(n.relative_to(ROOT)):digest(n) for n in ROOT.glob("include/*") if n.is_file()}
    source_list=[ROOT/n for n in native]+[ROOT/"checks/aic8800-supplicant.c"]+[up/"src"/n for n in external]
    for source in source_list:
        obj=out/(str(source.relative_to(up if source.is_relative_to(up) else ROOT)).replace("/","_")+".o")
        cmd=[a.cc]+flags
        if source.is_relative_to(up):cmd += ["-include",str(ROOT/"include/aic8800_supplicant_port.h"),"-Wno-unused-parameter"]
        else:cmd += ["-Wall","-Wextra","-Werror","-Wno-unused-parameter"]
        cmd += ["-c",str(source),"-o",str(obj)]
        run=subprocess.run(cmd,capture_output=True,text=True)
        result["commands"].append({"command":cmd,"exit_code":run.returncode,"stderr":run.stderr})
        if not source.is_relative_to(up):result["source_sha256"][str(source.relative_to(ROOT))]=digest(source)
        if run.returncode:break
        objects.append(str(obj))
    if not run.returncode:
        exe=out/"supplicant-check";cmd=[a.cc,"-fsanitize=address,undefined","-Wl,--gc-sections"]+objects+([str(library)] if library else [])+["-o",str(exe)]
        run=subprocess.run(cmd,capture_output=True,text=True);result["commands"].append({"command":cmd,"exit_code":run.returncode,"stderr":run.stderr})
        if not run.returncode:
            cmd=[str(exe),str(out/"vectors.bin"),a.case];run=subprocess.run(cmd,capture_output=True,text=True)
            result.update(executable_sha256=digest(exe),run_command=cmd,stdout=run.stdout,stderr=run.stderr)
    for name in ["checks/aic8800-runtime.c","checks/aic8800-sdio-transport.c","checks/sdio-controller.c","checks/aic8800-supplicant.py"]:
        result["source_sha256"][name]=digest(ROOT/name)
    result.update(exit_code=run.returncode,result="passed" if run.returncode==0 else "failed")
    (out/"results.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:result.get(k) for k in ["result","stdout","stderr"]}))
    if run.returncode: print(result["commands"][-1].get("stderr",""))
    raise SystemExit(run.returncode!=0)
if __name__=="__main__":main()
