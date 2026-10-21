#!/usr/bin/env python3
"""Check the prepared G2D SRAM launcher without rebuilding or accessing hardware."""
import argparse,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import sram_launcher
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
selection=parser.add_mutually_exclusive_group()
selection.add_argument('--blend-only',action='store_true',
                    help='Check only source-over admission and nonopaque-background refusal')
selection.add_argument('--scale-only',action='store_true',
                    help='Check only 2x scaling admission and changed geometry refusals')
args=parser.parse_args()
def cases(template):
    if args.blend_only:
        assert struct.unpack_from('<I',template,872)[0]==3
        color=struct.unpack_from('<I',template,900)[0]
        assert color>>24==255
        return [('source-over-handoff',12,None,None),
                ('nonopaque-background',14,(900,struct.pack('<I',color&0x00ffffff|0xfe000000)),None)]
    if args.scale_only:
        width,height,stride,fmt,op,dx,dy,w,h,sx,sy,color=struct.unpack_from('<12I',template,856)
        assert op==4 and not color and w>=2 and h>=2 and not (w&1 or h&1)
        assert sx+w//2<=width and sy+h//2<=height
        assert sx+w>width or sy+h>height, 'Use a source crop that requires half-size bounds'
        return [('scale-half-source-handoff',12,None,None),
                ('scale-odd-destination',14,(884,struct.pack('<I',w-1)),None),
                ('scale-source-overflow',14,(892,struct.pack('<I',width-w//2+1)),None)]
    return [('bounded-handoff',12,None,None),
            ('destination-overflow',14,(876,struct.pack('<I',0xffffffff)),None),
            ('memory-authority-mismatch',14,(200,bytes(8)),None)]
sram_launcher.check_bundle(args.bundle,args.output,(0x4153373347324131,1,904),(0x4153373347324331,288),20,cases,
    'Actual prepared AArch32 entry, G2D geometry/authority predicates, fixed 20-second contract publication and refusal restoration. System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.',
    'PASS {count} G2D SRAM launcher cases',runtime_contract=args.blend_only or args.scale_only)
