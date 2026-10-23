#!/usr/bin/env python3
"""Run a rebuilt G2D SRAM launcher on new operation sessions inside an earlier prepared bundle.

The earlier bundle supplies the image, request, contract and session identity;
only launch.bin is replaced. Its geometry predicate runs as T32 code called
from A32 through BLX. Cases patch the extension and geometry words, which the
launcher reads but does not hash. System registers, counter and MMIO are
modeled; no reset, CPU startup, native admission, USB or physical execution.
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import sram_launcher


def words(template,geometry=None,**extension):
    """Return an (offset, bytes) patch over the extension, clock and geometry words."""
    names=('sw','sh','bx','by','alpha','output_format','reserved0','reserved1')
    values=[extension.get(name,0) for name in names]
    old=list(struct.unpack_from('<12I',template,856))
    for index,value in (geometry or {}).items():old[index]=value
    return (800,struct.pack('<8I',*values)+template[832:856]+struct.pack('<12I',*old))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle',type=Path,required=True,help='An earlier prepared G2D bundle; read only')
    parser.add_argument('--launcher',type=Path,required=True,help='The rebuilt launch.bin')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    bundle=args.output/'bundle';bundle.mkdir()
    for name in ('image.elf','session.bin','contract.bin','request.bin'):shutil.copy2(args.bundle/name,bundle/name)
    shutil.copy2(args.launcher,bundle/'launch.bin')
    template=(bundle/'session.bin').read_bytes()
    width,height,stride=struct.unpack_from('<3I',template,856)
    assert width>=8 and height>=8,'Use a template with room for the new geometries.'
    # Geometry indices: 3 format, 4 operation, 5 dx, 6 dy, 7 w, 8 h, 9 sx, 10 sy, 11 color.
    small={5:0,6:0,7:4,8:4,9:0,10:0,11:0}
    cases=[
        ('scale-reduce-handoff',12,words(template,{**small,4:5,7:2,8:3},sw=width,sh=height),None),
        ('scale-enlarge-convert-handoff',12,words(template,{**small,3:3,4:5,7:width,8:height},sw=1,sh=2,output_format=6),None),
        ('copy-convert-handoff',12,words(template,{**small,3:7,4:2},output_format=0),None),
        ('image-over-image-handoff',12,words(template,{**small,4:6},bx=width-4,by=height-4,alpha=1),None),
        ('scale-identity',14,words(template,{**small,4:5},sw=4,sh=4),None),
        ('scale-source-overflow',14,words(template,{**small,4:5,9:1},sw=width,sh=1),None),
        ('image-zero-alpha',14,words(template,{**small,4:6}),None),
        ('image-background-overflow',14,words(template,{**small,4:6},bx=width-3,alpha=9),None),
        ('fill-convert',14,words(template,{**small,4:1},output_format=1),None),
        ('format-range',14,words(template,{**small,4:2,3:8}),None),
        ('reserved-word',14,words(template,{**small,4:2},reserved1=1),None),
        ('unknown-operation',14,words(template,{**small,4:7}),None)]
    sram_launcher.check_bundle(bundle,args.output/'cases',(0x4153373347324131,1,904),(0x4153373347324331,288),20,cases,
        __doc__,'PASS {count} rebuilt G2D SRAM launcher cases')


if __name__=='__main__':main()
