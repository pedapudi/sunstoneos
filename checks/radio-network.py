#!/usr/bin/env python3
"""Authenticate the pinned network stack; build/run only the composed network model."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import tarfile
ROOT=Path(__file__).resolve().parents[1]
TCP_REV="b86ad086e9870099bb0ac18a75dcb7f6dd508cc7"
TCP_ARCHIVE_SHA="60a2eea0de133948bb9a12f09abcba8e47800c3bbf61fe42fc7db6af453f142a"
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def verify(path):
    archive=path.parent/"FreeRTOS-Plus-TCP-b86ad086.tar.gz"
    assert digest(archive)==TCP_ARCHIVE_SHA
    manifest={}
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            relative=Path(member.name).relative_to("FreeRTOS-Plus-TCP-"+TCP_REV)
            if member.isfile() and (relative.parts[0]=="source" or str(relative) in ["LICENSE.md","History.txt"]):
                data=tar.extractfile(member).read();assert (path/relative).read_bytes()==data,str(relative)
                manifest[str(relative)]=hashlib.sha256(data).hexdigest()
    return manifest

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--verify-source",type=Path)
    p.add_argument("--tinyusb",type=Path);p.add_argument("--tcp",type=Path);p.add_argument("--rsn",type=Path);p.add_argument("--kernel",type=Path)
    p.add_argument("--headers",type=Path);p.add_argument("--linker",type=Path);p.add_argument("--qemu",type=Path)
    p.add_argument("--case",choices=["generation","entropy","late","alias","inbound","inbound-cancel","inbound-timeout","inbound-queue","inbound-late-child","inbound-lost-child","inbound-pending"],default="generation");p.add_argument("--evidence",type=Path);p.add_argument("--build-only",action="store_true")
    a=p.parse_args()
    if a.verify_source:verify(a.verify_source.resolve());print("Pinned FreeRTOS+TCP source matches");return
    assert all([a.tcp,a.rsn,a.kernel,a.headers,a.linker,a.evidence,a.tinyusb])
    out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
    source=out/"project";source.mkdir(exist_ok=True)
    (source/"CMakeLists.txt").write_text(f'cmake_minimum_required(VERSION 3.24)\nproject(network_composition C ASM)\nadd_subdirectory("{ROOT}" base)\nif(NOT TARGET radio-network-model)\ninclude("{ROOT}/cmake/radio-network.cmake")\nendif()\n')
    toolchain=out/"toolchain.cmake"
    toolchain.write_text(f'set(CMAKE_SYSTEM_NAME Generic)\nset(CMAKE_SYSTEM_PROCESSOR aarch64)\nset(CMAKE_C_COMPILER clang)\nset(CMAKE_ASM_COMPILER clang)\nset(CMAKE_C_COMPILER_TARGET aarch64-none-elf)\nset(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\nset(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path={a.linker}")\n')
    result={"tcp_archive_sha256":TCP_ARCHIVE_SHA,"tcp_source":verify(a.tcp.resolve()),"commands":[]}
    commands=[["cmake","-S",str(source),"-B",str(out/"build"),f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",f"-DFREERTOS_DIR={a.kernel}",f"-DSUNSTONE_TCP_DIR={a.tcp}",f"-DSUNSTONE_RSN_DIR={a.rsn}",f"-DSUNSTONE_NETWORK_C_HEADERS={a.headers}",f"-DSUNSTONE_LLD={a.linker}",f"-DTINYUSB_DIR={a.tinyusb}","-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
              ["cmake","--build",str(out/"build"),"--target","radio-network-model","-j4"]]
    for cmd in commands:
        run=subprocess.run(cmd,capture_output=True,text=True);result["commands"].append({"command":cmd,"exit_code":run.returncode,"stdout":run.stdout,"stderr":run.stderr})
        if run.returncode:break
    if not run.returncode:
        matches=list((out/"build").rglob("radio-network-model.elf"));assert len(matches)==1,matches
        elf=matches[0];result["elf_sha256"]=digest(elf)
        artifact=out/("radio-network-"+result["elf_sha256"]+".elf");shutil.copyfile(elf,artifact);result["artifact"]=str(artifact)
        if not a.build_only:
            assert a.qemu
            cmd=[str(a.qemu),"-M","virt,gic-version=3,secure=off,virtualization=off","-cpu","cortex-a53","-m","128M","-nographic","-nic","none","-monitor","none","-semihosting-config","enable=on,target=native,arg="+a.case,"-kernel",str(elf)]
            run=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
            result["run"]={"command":cmd,"exit_code":run.returncode,"stdout":run.stdout,"stderr":run.stderr}
    names=["checks/radio-network.c","checks/radio-network.py","cmake/radio-network.cmake",
           "drivers/net/aic8800_network.c","drivers/net/sunstone_net.c","include/aic8800_network.h",
           "include/sunstone_net.h","network/allocator.c","network/include/FreeRTOSConfig.h",
           "network/include/FreeRTOSIPConfig.h","docs/radio-network.md"]
    names += ["checks/aic8800-runtime.c","checks/aic8800-sdio-transport.c","checks/sdio-controller.c"]
    result["source_sha256"]={n:digest(ROOT/n) for n in names if (ROOT/n).is_file()}
    result.update(result="passed" if not run.returncode else "failed",exit_code=run.returncode)
    (out/"results.json").write_text(json.dumps(result,indent=2)+"\n")
    print(result["result"],run.stdout[-1500:],run.stderr[-5000:]);raise SystemExit(run.returncode!=0)
if __name__=="__main__":main()
