#!/usr/bin/env python3
"""Execute the linked native UART write policy for FCR, MCR, THR and RS-485 control.

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
# FCR (offset 8) accepts exactly the planned trigger with and without the FIFO
# reset bits, plus the DMA mode value on every port. MCR (0x10) accepts the
# planned flow control alone or with loopback, IrDA SIR and RS-485 9-bit. THR
# (0) alone accepts a ninth bit. RS-485 control (0xc0) exists on UART1..6.
CASES={(0,8):{1:1,7:1,9:1,0x81:0,0x87:0,0x41:0},
       (1,8):{0x81:1,0x87:1,9:1,1:0,7:0,0x41:0,0x47:0,0xc1:0},
       (2,8):{0x81:1,0x87:1,9:1,1:0,7:0,0x41:0,0x89:0},
       (1,0x10):{0:1,0x22:1,0x10:1,0x32:1,0x40:1,0xa2:1,0x80:0,0x50:0,0xb2:0,0x02:0,0x62:0},
       (1,0):{0x5a:1,0x15a:1,0x200:0},
       (1,0xc0):{0x18:1,0:1,0x10:0,0x19:0,0x118:0},(0,0xa4):{0xc2:1,7:1,0x42:0},
       (0,0x88):{0xe5:1,0xa5:0},(1,0x8c):{0:1,3:1,7:0}}

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--image',type=Path,required=True);p.add_argument('--session',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 image=base.Image(a.image);m=base.Machine(image,a.session.read_bytes(),'success');m.setup()
 native=m.app+image.layouts['uart_application']['native'];results=[]
 for (port,offset),values in CASES.items():
  m.field(native,'a733_uart_platform_native','port',port)
  address=0x02500000+port*0x1000+offset
  for value,allowed in values.items():
   before=len(m.writes);m.val(address,4,0xee)
   got=m.call('write32',native,address,value,unit=UNIT)
   wrote=m.get(address,4)==value
   assert got==allowed and wrote==bool(allowed),(port,hex(offset),hex(value),got,wrote)
   if port==2:assert len(m.writes)==before+allowed # The harness records UART2 MMIO only.
   results.append(dict(port=port,offset=offset,value=value,accepted=bool(got)))
 a.output.mkdir(parents=True,exist_ok=False)
 (a.output/'result.json').write_text(json.dumps(dict(result='pass',physical_access=False,
  image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=results),indent=2)+'\n')
 print('PASS native UART write allowlist accepts exactly the planned FIFO, mode, ninth-bit and RS-485 values')
if __name__=='__main__':main()
