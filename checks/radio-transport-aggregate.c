/* Runtime receive of one aggregate larger than 4,096 bytes through the real
 * dispatcher, transport, controller and card code on the existing model. */
#define AIC8800_RUNTIME_FIXTURE_MAIN runtime_fixture_unused_main
#include "aic8800-runtime.c"
#undef AIC8800_RUNTIME_FIXTURE_MAIN
#include "aic8800_dispatch.h"

static struct aic8800_dispatch dispatch;
int main(void)
{
    begin(AIC8800_PAD_KEEP);setup();identity(2000);
    while(transport.state==AIC8800_TRANSPORT_IDENTITY)step();
    assert(transport.state==AIC8800_TRANSPORT_COMPLETE);
    base_transport=transport;base_host=host;base_card=card;base_model=m;
    fresh(0);assert(aic8800_dispatch_init(&dispatch,&transport));
    /* Three 1,500-byte data records, a 4,604-byte print record and an
     * unrelated control message: 9,124 bytes reported as 18 blocks. */
    memset(receive,0,sizeof(receive));size_t at=0;
    for(unsigned i=0;i<3;++i,at+=1500){receive[at]=0xa0;receive[at+1]=5;receive[at+2]=0x80;receive[at+60]=(uint8_t)(0xb0+i);}
    receive[at]=0xf8;receive[at+1]=0x11;receive[at+2]=0x13;receive[at+4]=0xc0;at+=4604;
    uint8_t parameter=7;at+=message(receive+at,0x1234,&parameter,1);
    assert(at==9124);
    uint64_t transfers=transport.completed_transfers;m.card[1][4]=18;
    for(unsigned i=0;i<4000&&dispatch.retired_records<5&&!dispatch.faulted;++i){++m.now;aic8800_dispatch_poll(&dispatch);}
    assert(!dispatch.faulted&&transport.state==AIC8800_TRANSPORT_RUNTIME&&dispatch.retired_records==5);
    /* One CMD53 read the complete aggregate. */
    assert(transport.completed_transfers==transfers+1&&transport.rx_bytes==18*512&&host.request.bytes==18*512);
    assert(dispatch.record_count==4&&dispatch.discarded_diagnostics==1);
    uint64_t last=0;
    for(unsigned i=0;i<4;++i){
        struct aic8800_dispatch_record r;assert(aic8800_dispatch_take_record(&dispatch,&r)&&r.sequence>last);last=r.sequence;
        if(i<3)assert(r.kind==AIC8800_DISPATCH_UNSUPPORTED_DATA&&r.bytes==1500&&!r.truncated&&r.payload[60]==0xb0+i);
        else assert(r.kind==AIC8800_DISPATCH_PRINT&&r.bytes==AIC8800_DISPATCH_RECORD_BYTES&&r.truncated&&r.payload[4]==0xc0);
    }
    for(unsigned i=0;i<100&&transport.cursor!=transport.rx_bytes;++i){++m.now;aic8800_dispatch_poll(&dispatch);}
    assert(transport.cursor==transport.rx_bytes&&!transport.held&&!dispatch.faulted);
    puts("Radio transport aggregate: one 18-block CMD53, records parsed in order, long diagnostic truncated");
    return 0;
}
