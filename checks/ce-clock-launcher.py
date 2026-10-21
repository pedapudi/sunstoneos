#!/usr/bin/env python3
"""Check the prepared CE clock SRAM launcher without rebuilding or accessing hardware."""
import argparse,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import sram_launcher
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
sram_launcher.check_bundle(args.bundle,args.output,(0x4153373343524131,3,824),(0x4153373343524331,224),60,
    [('cold-clock-session-handoff',12,None,None),
     ('unsupported-session-version',14,(600,struct.pack('<Q',4)),None)],
    'Actual prepared AArch32 entry, CE clock session-version predicates, fixed 60-second contract publication and refusal restoration. System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.',
    'PASS two CE clock SRAM launcher cases')
