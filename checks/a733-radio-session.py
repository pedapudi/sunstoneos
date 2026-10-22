#!/usr/bin/env python3
"""Build the native radio session composition and preserve one selected case batch."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
REFERENCES={
 "aicwf_txrxif.c":"2c22a400aa25e38fa908d0f43313081811aa8b988641464555ed1643585e8ce4",
 "rwnx_cmds.c":"41e06e13c9ad59c244710c89abd9fd53d665739744f73323e8ed9297c11bc71a",
 "rwnx_msg_rx.c":"9eed44d33b2f6241171e47648ba67e5d193d0e54558fa9f878e8821afe8f9f2b"}
INPUTS=["checks/a733-radio-session.c","checks/a733-radio-session.py","cmake/radio-session.cmake",
 "drivers/net/a733_radio_session.c","include/a733_radio_session.h","drivers/net/sunstone_net.c","include/sunstone_net.h"]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ["tcp","rsn","kernel","tinyusb","headers","linker","qemu","reference-dir","evidence"]:p.add_argument("--"+name,type=Path,required=True)
 p.add_argument("--cases",nargs="+",choices=["session"],default=["session"])
 p.add_argument("--build-only",action="store_true");p.add_argument("--firmware",type=Path,required=True);a=p.parse_args()
 out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);source=out/"project";source.mkdir(exist_ok=True)
 for name,h in REFERENCES.items():assert sha(a.reference_dir/name)==h,name
 blobs=[("fw_adid_8800d80_u02.bin","a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56"),
        ("fw_patch_8800d80_u02.bin","4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0"),
        ("fw_patch_table_8800d80_u02.bin","5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60"),
        ("fmacfw_8800d80_h_u02.bin","ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7")]
 assembly=out/"firmware.S"
 lines=['.section .rodata.session_blobs,"a"']
 for i,(name,h) in enumerate(blobs):
  assert sha(a.firmware/name)==h,name
  lines += ['.balign 16',f'.global session_blob{i}',f'session_blob{i}:',f'.incbin "{a.firmware/name}"']
 assembly.write_text("\n".join(lines)+"\n")
 (source/"CMakeLists.txt").write_text(f'cmake_minimum_required(VERSION 3.24)\nproject(session_composition C ASM)\nadd_subdirectory("{ROOT}" base)\nif(NOT TARGET radio-dispatch-model)\ninclude("{ROOT}/cmake/radio-dispatch.cmake")\nendif()\ninclude("{ROOT}/cmake/radio-session.cmake")\n')
 toolchain=out/"toolchain.cmake";toolchain.write_text(f'set(CMAKE_SYSTEM_NAME Generic)\nset(CMAKE_SYSTEM_PROCESSOR aarch64)\nset(CMAKE_C_COMPILER clang)\nset(CMAKE_ASM_COMPILER clang)\nset(CMAKE_C_COMPILER_TARGET aarch64-none-elf)\nset(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\nset(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path={a.linker}")\nset(SUNSTONE_LLD "{a.linker}")\n# The repository toolchain also defines the path-map helpers its CMakeLists uses.\ninclude("{ROOT}/cmake/aarch64-clang.cmake")\n')
 result={"references":REFERENCES,"firmware":dict(blobs),"commands":[],"runs":[]}
 commands=[["cmake","-S",str(source),"-B",str(out/"build"),f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",f"-DFREERTOS_DIR={a.kernel}",f"-DTINYUSB_DIR={a.tinyusb}",f"-DSUNSTONE_TCP_DIR={a.tcp}",f"-DSUNSTONE_RSN_DIR={a.rsn}",f"-DSUNSTONE_NETWORK_C_HEADERS={a.headers}",f"-DSUNSTONE_LLD={a.linker}",f"-DSUNSTONE_SESSION_BLOBS={assembly}"],
 ["cmake","--build",str(out/"build"),"--target","radio-session-model","-j4"]]
 for cmd in commands:
  run=subprocess.run(cmd,capture_output=True,text=True);result["commands"].append({"command":cmd,"returncode":run.returncode,"stdout":run.stdout,"stderr":run.stderr})
  if run.returncode:break
 if not run.returncode:
  matches=list((out/"build").rglob("radio-session-model.elf"));assert len(matches)==1,matches;elf=matches[0];identity=sha(elf);artifact=out/("radio-session-"+identity+".elf");shutil.copyfile(elf,artifact)
  result.update(artifact=str(artifact),elf_sha256=identity)
  snapshot=out/("source-"+identity);snapshot.mkdir(exist_ok=True)
  result["source_sha256"]={}
  for name in INPUTS:
   if not (ROOT/name).exists():continue
   target=snapshot/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/name,target);result["source_sha256"][name]=sha(target)
  # Preserve the complete compile dependency closure before executing the image.
  paths={ROOT/name for name in INPUTS}
  paths.update([assembly,toolchain,source/"CMakeLists.txt",ROOT/"CMakeLists.txt",ROOT/"platform/qemu-virt/link.ld",a.qemu,a.linker])
  target_dir=elf.parent/"CMakeFiles/radio-session-model.dir"
  paths.update(path for path in target_dir.iterdir() if path.is_file() and path.suffix in (".txt",".make"))
  paths.update(ROOT.glob("cmake/*.cmake"))
  paths.update(a.firmware/name for name,_ in blobs)
  paths.update(a.reference_dir/name for name in REFERENCES)
  compiler=shutil.which("clang");assert compiler;paths.add(Path(compiler).resolve())
  for dep in (out/"build").rglob("*.d"):
   names=dep.read_text().replace("\\\n"," ").split(":",1)[1].split()
   paths.update(Path(name).resolve() if Path(name).is_absolute() else (out/"build"/name).resolve() for name in names)
  records=[]
  for path in sorted(paths):
   content=path.read_bytes();digest=hashlib.sha256(content).hexdigest();target=out/"inputs"/digest
   target.parent.mkdir(exist_ok=True)
   if not target.exists():target.write_bytes(content)
   records.append({"path":str(path),"sha256":digest,"snapshot":str(target.relative_to(out))})
  (out/"inputs.json").write_text(json.dumps(records,indent=2)+"\n")
  if not a.build_only:
   env=dict(os.environ);lib=a.qemu.parent.parent/"lib/x86_64-linux-gnu"
   if lib.is_dir():env["LD_LIBRARY_PATH"]=str(lib)+(":"+env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
   for case in a.cases:
    cmd=[str(a.qemu),"-M","virt,gic-version=3,secure=off,virtualization=off","-cpu","cortex-a53","-m","128M","-nographic","-nic","none","-monitor","none","-semihosting-config","enable=on,target=native,arg="+case,"-kernel",str(artifact)]
    try:
     run=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=60);record={"case":case,"command":cmd,"returncode":run.returncode,"stdout":run.stdout,"stderr":run.stderr}
    except subprocess.TimeoutExpired as error:record={"case":case,"command":cmd,"returncode":124,"stdout":str(error.stdout),"stderr":str(error.stderr)}
    result["runs"].append(record);print(case,record["returncode"],record["stdout"],flush=True)
    if record["returncode"]:break
 result["passed"]=not result["commands"][-1]["returncode"] and all(not r["returncode"] for r in result["runs"])
 (out/"results.json").write_text(json.dumps(result,indent=2)+"\n")
 if not result["passed"]:print(result["commands"][-1]["stderr"][-4000:])
 raise SystemExit(not result["passed"])
if __name__=="__main__":main()
