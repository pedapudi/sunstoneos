#!/usr/bin/env python3
"""Check the prepared deinterlacer SRAM launcher without rebuilding or accessing hardware."""
import argparse,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import sram_launcher
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
selection=parser.add_mutually_exclusive_group()
selection.add_argument('--motion-only',action='store_true',
                    help='Check only motion-mode admission and unsupported-mode refusal')
selection.add_argument('--tnr-only',action='store_true',
                    help='Check only temporal-filter admission and unsupported-mode refusal')
args=parser.parse_args()
def cases(template):
    if args.motion_only:
        assert struct.unpack_from('<I',template,876)[0]==2
        return [('motion-handoff',12,None,None),
                ('unsupported-motion-mode',14,(876,struct.pack('<I',4)),None)]
    if args.tnr_only:
        assert struct.unpack_from('<I',template,876)[0]==3
        return [('temporal-filter-handoff',12,None,None),
                ('unsupported-temporal-mode',14,(876,struct.pack('<I',4)),None)]
    return [('bounded-handoff',12,None,None),
            ('invalid-weave-geometry',14,(856,struct.pack('<I',641)),None),
            ('memory-authority-mismatch',14,(200,bytes(8)),None)]
sram_launcher.check_bundle(args.bundle,args.output,(0x4153373344494131,1,888),(0x4153373344494331,288),20,cases,
    'Actual prepared AArch32 entry, deinterlacer geometry/authority predicates, fixed 20-second contract publication and refusal restoration. System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.',
    'PASS {count} deinterlacer SRAM launcher cases',runtime_contract=args.motion_only or args.tnr_only)
