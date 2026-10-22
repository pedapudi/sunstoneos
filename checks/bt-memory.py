#!/usr/bin/env python3
"""Build and run the shared Bluetooth range-helper unit check under ASan/UBSan."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
p=argparse.ArgumentParser()
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=False)
files=['include/bt_memory.h','include/a733_range.h','checks/bt-memory.c','checks/bt-memory.py']
manifest={f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files}
cc=shutil.which('clang-21');assert cc
exe=a.output/'bt-memory-model'
commands=[[cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-Iinclude',
           'checks/bt-memory.c','-o',str(exe)],[str(exe)],
          [cc,'--target=aarch64-none-elf','-std=c11','-Wall','-Wextra','-Werror','-ffreestanding','-fsyntax-only','-Iinclude','-include','bt_memory.h','-x','c','/dev/null']]
log=''.join(subprocess.check_output(c,cwd=root,stderr=subprocess.STDOUT,text=True) for c in commands)
(a.output/'model.log').write_text(log)
manifest['commands']=[[str(x) for x in c] for c in commands]
manifest['compiler']={'path':cc,'sha256':hashlib.sha256(pathlib.Path(cc).read_bytes()).hexdigest()}
(a.output/'results.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(log,end='')
