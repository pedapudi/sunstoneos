#!/usr/bin/env python3
"""Generate fixed input declarations and narrow shared-owner adapters."""
import argparse
import hashlib
import json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
models=json.loads((a.root/'apps/bluetooth/recognition-models.json').read_text())['models']
wave=json.loads((a.root/'apps/mimus-board/inputs.json').read_text())['recorded_wav']
assets=models+[wave]
assert all(0<v['bytes']<=limit for v,limit in zip(assets,[8388608,8388608,1048576]))
text='#include <stdint.h>\n#include <stddef.h>\n'
text+='static const uintptr_t mimus_asset_address[3]={0x44000000,0x44800000,0x45000000};\n'
text+='static const size_t mimus_asset_bytes[3]={'+','.join(str(v['bytes']) for v in assets)+'};\n'
text+='static const uint8_t mimus_asset_sha256[3][32]={'+','.join('{'+','.join('0x'+v['sha256'][i:i+2] for i in range(0,64,2))+'}' for v in assets)+'};\n'
(a.output/'assets.h').write_text(text)
def replace(source,old,new):
    assert source.count(old)==1, 'Shared owner source changed: '+old
    return source.replace(old,new)
s=(a.root/'apps/board/client.c').read_text().replace('#include "application.h"','#include "../board/application.h"')
s='#include "../mimus-board/application.h"\n'+s
# The original owner continues every poll after issuing or completing its read.
s=replace(s,'enum a733_usb2_result r=a733_usb2_native_poll(&board.host);','enum a733_usb2_result r=a733_usb2_native_poll(&board.host);\n        mimus_board_host_observed(r==A733_USB2_READY);')
s='#include <string.h>\nint mimus_board_keyboard_poll(void);\n'+s
s=replace(s,'if(!dual_step(&issued,&reports,&sector_done))break;',
    '''if(r==A733_USB2_READY){
                if(!mimus_board_keyboard_poll())break;
                if(!issued&&mimus_board_started()){
                    if(!a733_usb2_read_sector(&board.host,0,board.host.platform.config.lifetime_deadline))break;
                    issued=1;
                }
                if(issued==1){
                    int done=a733_usb2_read_result(&board.host,board.sector);if(done<0)break;
                    if(done==1){mimus_board_storage_completed();atomic_store_explicit(&board.result,1,memory_order_release);issued=2;}
                }
            }''')
s=replace(s,'}else if(r==A733_USB2_READY&&!issued){','}else if(r==A733_USB2_READY&&!issued&&mimus_board_started()){')
s=replace(s,'if(done==1){\n                if(board.session.workload==6){','if(done==1){\n                mimus_board_storage_completed();\n                if(board.session.workload==6){')
(a.output/'mimus-client.c').write_text(s)
s=(a.root/'apps/board/usb0.c').read_text().replace('#include "application.h"','#include "../board/application.h"')
s=replace(s,'static _Noreturn void stop(void){for(;;)vTaskDelay(1);}',
    'static _Noreturn void stop(void){board_fail(NULL);a733_typec_revoke(&board.typec.provider);for(;;)vTaskDelay(1);}')
(a.output/'mimus-usb-base.c').write_text(s)
