#!/usr/bin/env python3
"""Check the prepared DisplayPort SRAM launcher without rebuilding or accessing hardware."""
import argparse,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import sram_launcher
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
sram_launcher.check_bundle(args.bundle,args.output,(0x4153373344504131,1,952),(0x4153373344504331,288),20,
    [('bounded-handoff',12,None,None),
     ('typec-source-overvoltage',14,(916,struct.pack('<I',5251)),None),
     ('memory-authority-mismatch',14,(200,bytes(8)),None),
     ('supervised-high-half-deadline',14,(160,struct.pack('<Q',1<<40)),None),
     ('supervised-reserved-word',14,(156,struct.pack('<I',1)),None)],
    'Actual prepared AArch32 entry, DisplayPort source/authority predicates, fixed 20-second contract publication and refusal restoration. System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.',
    'PASS {count} DisplayPort SRAM launcher cases')
