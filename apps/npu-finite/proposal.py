#!/usr/bin/env python3
"""Describe native NPU session, upload and return geometry without hardware access."""
import argparse,hashlib,importlib.util,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from host_evidence import unique_directory
spec=importlib.util.spec_from_file_location('npu_proposal_host',Path(__file__).with_name('host.py'))
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--checkpoint-check',type=Path,help='Optional check result for inclusion in the report.')
a=p.parse_args();image=a.evidence/'image.elf';data=image.read_bytes()
parts,address,build,layout,geometry=host.image_profile(data,{})
record={'physical_access':False,'execution_profile':host.geometry_profile(geometry),'image_sha256':host.sha(data),
        'build_id':build.hex(),'session_bytes':layout[0],'session_upload':0x41f00000,'contract_upload':0x42004000,
        'entry':0x41000000,'geometry':geometry,'segments':[{'address':part['address'],'memory_bytes':part['memory_bytes'],
        'file_bytes':len(part['data']),'flags':part['flags']} for part in parts],
        'procedure':['Hold the selected port lock and validate its live A733 ROM instance.',
          'Save the complete owned DRAM range before uploading and reading back the native image and session.',
          'Launch once under the USB transition guard and watchdog policy.',
          'Validate returned reports before restoring saved DRAM; retain ownership on any uncertain result.']}
if a.checkpoint_check:record['check']=json.loads(a.checkpoint_check.read_bytes())
a.output.parent.mkdir(parents=True,exist_ok=True)
if a.output.exists():a.output=unique_directory(a.output.parent)/a.output.name
host.save(a.output,record);print('Wrote native NPU layout report:',a.output)
