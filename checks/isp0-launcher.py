#!/usr/bin/env python3
"""Check the prepared ISP0 SRAM launcher without rebuilding or accessing hardware."""
import argparse,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import sram_launcher
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
sram_launcher.check_bundle(args.bundle,args.output,(0x4153373349504131,1,880),(0x4153373349504331,224),20,
    [('bounded-handoff',12,None,None),
     ('invalid-raw-width',14,(848,struct.pack('<I',641)),None),
     ('invalid-reserved-geometry',14,(868,struct.pack('<I',1)),None)],
    'Actual prepared AArch32 entry, ISP0 geometry predicates, fixed 20-second contract publication and refusal restoration. System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.',
    'PASS three ISP0 SRAM launcher cases')
