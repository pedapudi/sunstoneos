#!/usr/bin/env python3
"""Execute the linked native UART write policy for FCR values on ports 0-2.

Reuses the UART DMA image harness from uart-dma-native.py: RTOS state and MMIO
are modeled; the native binding's current-task check and allowlist execute
their actual instructions. No physical UART access.
"""
import argparse,hashlib,importlib.util,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('uart_dma_native',ROOT/'checks/uart-dma-native.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
UNIT='/drivers/uart/a733_uart_platform_native.c'
# Each port accepts exactly its planned trigger with and without the FIFO reset
# bits; UART2 also accepts the DMA mode value.
CASES={0:{1:1,7:1,0x81:0,0x87:0,9:0,0x41:0},
       1:{0x81:1,0x87:1,1:0,7:0,9:0,0x41:0,0x47:0,0xc1:0},
       2:{0x81:1,0x87:1,9:1,1:0,7:0,0x41:0,0x89:0}}

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--image',type=Path,required=True);p.add_argument('--session',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 image=base.Image(a.image);m=base.Machine(image,a.session.read_bytes(),'success');m.setup()
 native=m.app+image.layouts['uart_application']['native'];results=[]
 for port,values in CASES.items():
  m.field(native,'a733_uart_platform_native','port',port)
  fcr=0x02500000+port*0x1000+8
  for value,allowed in values.items():
   before=len(m.writes);m.val(fcr,4,0xee)
   got=m.call('write32',native,fcr,value,unit=UNIT)
   wrote=m.get(fcr,4)==value
   assert got==allowed and wrote==bool(allowed),(port,hex(value),got,wrote)
   if port==2:assert len(m.writes)==before+allowed
   results.append(dict(port=port,value=value,accepted=bool(got)))
 a.output.mkdir(parents=True,exist_ok=False)
 (a.output/'result.json').write_text(json.dumps(dict(result='pass',physical_access=False,
  image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=results),indent=2)+'\n')
 print('PASS native UART FCR allowlist accepts exactly the planned trigger values')
if __name__=='__main__':main()
