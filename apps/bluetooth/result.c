#include "result.h"
#include <string.h>
#include "bt_memory.h"
static void put32(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i));}
static void put64(uint8_t *p,uint64_t value){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(value>>(8*i));}
int bluetooth_result_frame(uint8_t out[BLUETOOTH_RESULT_MAX],size_t *bytes,unsigned state,
    uint64_t epoch,uint64_t generation,const uint8_t profile[32],uint64_t firmware,uint64_t uart,
    const struct bt_hci_identity *id)
{
    if(!epoch||!generation||state>2||!bt_range_disjoint(out,BLUETOOTH_RESULT_MAX,bytes,sizeof(*bytes))||
        !bt_range_disjoint(out,BLUETOOTH_RESULT_MAX,profile,32)||!bt_range_disjoint(bytes,sizeof(*bytes),profile,32)||
        (state==1&&(!id||id->valid!=63||!firmware||!uart||firmware==uart||
            !bt_range_disjoint(out,BLUETOOTH_RESULT_MAX,id,sizeof(*id))||!bt_range_disjoint(bytes,sizeof(*bytes),id,sizeof(*id)))))return 0;
    uint8_t frame[BLUETOOTH_RESULT_MAX]={0};
    put32(frame,0x43485442);put32(frame+4,1);put32(frame+8,state);put32(frame+12,state==1?97:0);
    put64(frame+16,epoch);put64(frame+24,generation);memcpy(frame+32,profile,32);
    if(state==1){put64(frame+64,firmware);put64(frame+72,uart);put32(frame+80,id->valid);
        memcpy(frame+84,id->version,8);memcpy(frame+92,id->supported_commands,64);memcpy(frame+156,id->features,8);
        memcpy(frame+164,id->buffer_size,7);memcpy(frame+171,id->address,6);}
    size_t n=state==1?177:80;memcpy(out,frame,n);*bytes=n;return 1;
}
