#!/usr/bin/env python3
"""Freeze the existing linked-object and compiler-dependency evidence format."""
import argparse,hashlib,importlib.util,json,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('jpeg_runtime_snapshot',ROOT/'tools/evidence-snapshot.py')
snapshot=importlib.util.module_from_spec(spec);spec.loader.exec_module(snapshot)
def freeze(build,out):
    out.mkdir(parents=True,exist_ok=False)
    dependencies,commands,records=snapshot.collect_dependencies(build,'sunstone-a733-h264-encode')
    image=build/'sunstone-a733-h264-encode.elf';shutil.copy2(image,out/'image.elf')
    (out/'compiled-dependencies.json').write_text(json.dumps(dependencies,indent=2)+'\n')
    (out/'compile-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    (out/'dependency-records.json').write_text(json.dumps(records,indent=2)+'\n')
    raw=out/'raw-dependency-records';raw.mkdir()
    raw_index={}
    for item in records.values():
        source=Path(item['record']);digest=item['record_sha256'];data=source.read_bytes()
        assert hashlib.sha256(data).hexdigest()==digest
        target=raw/digest
        if not target.exists():target.write_bytes(data)
        raw_index[str(source)]={'sha256':digest,'snapshot':str(target.relative_to(out))}
    (out/'raw-dependency-index.json').write_text(json.dumps(raw_index,indent=2)+'\n')
    inputs=out/'compiled-inputs';inputs.mkdir()
    for path,digest in dependencies.items():
        target=inputs/digest
        if not target.exists():target.write_bytes(Path(path).read_bytes())
        assert hashlib.sha256(target.read_bytes()).hexdigest()==digest
    manifest=dict(target='sunstone-a733-h264-encode',execution_profile='h264-encode-supervised-v1',
      final_elf=hashlib.sha256(image.read_bytes()).hexdigest(),build_id=(build/'h264_encode-application/build-id.txt').read_text().strip(),
      physical_access=False,compiler_input_count=len(dependencies),object_count=len(records),compiler_inputs=dependencies)
    (out/'image-input-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();freeze(a.build,a.output)
