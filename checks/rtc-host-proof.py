#!/usr/bin/env python3
"""Verify only final RTC software-evidence metadata; reuse the frozen device cases."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
ROOT=Path(__file__).resolve().parents[1]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    out=Path(sys.argv[1]);folder=out/'final-host-proof';folder.mkdir()
    path=ROOT/'tools/check-a733-rtc-supervised.py'
    spec=importlib.util.spec_from_file_location('rtc_final_proof',path);tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
    sources=tool.software_sources();frozen={}
    for name,path in sources.items():
        target=folder/name;shutil.copy2(path,target);frozen[name]={'path':str(target.resolve()),'sha256':digest(target)}
    shutil.copy2(Path(__file__),folder/'check.py')
    image=out/'build/sunstone-a733-rtc.elf';launcher=out/'launcher/launch.bin'
    evidence=[out/name for name in ('result.json','steps.json','host-result.json','launcher-result.json','native-validator-preflight.json')]
    value={'schema':'a733-rtc-supervised-software-v1','physical_access':False,'admission_cases':True,'launcher_cases':True,'host_capture':True,
           'image':{'path':str(image.resolve()),'sha256':digest(image)},'launcher':{'path':str(launcher.resolve()),'sha256':digest(launcher)},
           'sources':frozen,'evidence':[{'path':str(p.resolve()),'sha256':digest(p)} for p in evidence],
           'limits':'Original frozen driver/native/host cases reused. Only final proof-flag schema is checked here. Native USB serializer and physical RTC remain unqualified.'}
    prepared={'files':{'image.elf':digest(image),'launch.bin':digest(launcher)},'_source_hashes':{name:item['sha256'] for name,item in frozen.items()}}
    # Preserve actual metadata and exact checked inputs before admission.
    (out/'software-evidence.json').write_text(json.dumps(value,indent=2)+'\n')
    (folder/'inputs.json').write_text(json.dumps({'sources':frozen,'checker_sha256':digest(folder/'check.py'),'metadata_sha256':digest(out/'software-evidence.json')},indent=2)+'\n')
    tool.validate_software(value,prepared)
    changed=copy.deepcopy(value);changed['host_capture']=False
    try:tool.validate_software(changed,prepared)
    except ValueError:pass
    else:raise AssertionError('Missing actual host capture accepted')
    (folder/'result.json').write_text(json.dumps({'passed':True,'nominal_metadata':True,'missing_capture_refused':True,'replayed_device_cases':False},indent=2)+'\n')
    print('PASS final RTC software-evidence metadata')
if __name__=='__main__':main()
