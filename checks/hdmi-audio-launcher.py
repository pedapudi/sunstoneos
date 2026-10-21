#!/usr/bin/env python3
"""Check the prepared HDMI audio SRAM launcher without rebuilding or accessing hardware."""
import argparse,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import sram_launcher
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
request=(args.bundle/'request.bin').read_bytes()
sram_launcher.check_bundle(args.bundle,args.output,(0x4153373348444131,1,656),(0x4153373348444331,224),20,
    [('bounded-handoff',12,None,None),
     ('request-contract-digest-mismatch',3,None,(96,bytes([request[96]^1])))],
    'Actual prepared AArch32 entry, request/contract digest binding, fixed 20-second contract publication and refusal restoration. Audio session validation occurs later and is outside this check. System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.',
    'PASS two HDMI audio SRAM launcher cases',image_name='image_elf.bin',header_offset=0,padded_session=True)
