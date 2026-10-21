/* New application composition only. Provider callbacks model hardware;
 * UART/H4/HCI, security policy, channels, AG and PCM capture are production. */
#define BLUETOOTH_APPLICATION_FIXTURE_ENTRY archived_application_main
#include "bluetooth-application.c"
#undef BLUETOOTH_APPLICATION_FIXTURE_ENTRY
#include "../apps/bluetooth/headset.c"
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
void bluetooth_recognition_abort(void){} /* Metadata-only OS boundary; no engine completion. */
#endif
static size_t output_cursor;
static unsigned attempt,rf_stage,at_sent,audio_injected,disconnected,deny,loss;
static uint16_t local_cid;static unsigned handoff,speech,informational;
static void put16(uint8_t *p,unsigned n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static unsigned get16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static void acl_in(unsigned cid,const uint8_t *p,unsigned n)
{uint8_t h[]={2,0x23,0x20,(uint8_t)(n+4),(uint8_t)((n+4)>>8),(uint8_t)n,(uint8_t)(n>>8),(uint8_t)cid,(uint8_t)(cid>>8)};bytes(&wire,h,9);bytes(&wire,p,n);}
static void sig_in(unsigned code,unsigned id,const uint8_t *p,unsigned n)
{uint8_t packet[64]={(uint8_t)code,(uint8_t)id,(uint8_t)n,(uint8_t)(n>>8)};memcpy(packet+4,p,n);acl_in(1,packet,n+4);}
static uint8_t fcs(const uint8_t *p,unsigned n)
{unsigned crc=255;for(unsigned i=0;i<n;++i){crc^=p[i];for(unsigned j=0;j<8;++j)crc=(crc&1)?(crc>>1)^0xe0:crc>>1;}return (uint8_t)(255-crc);}
static void rf_in(unsigned dlci,unsigned ctrl,const uint8_t *p,unsigned n,int credit)
{uint8_t packet[200]={(uint8_t)(dlci*4+3),(uint8_t)ctrl,(uint8_t)(n*2+1)};unsigned at=3;if(credit>=0)packet[at++]=(uint8_t)credit;if(n)memcpy(packet+at,p,n);at+=n;packet[at++]=fcs(packet,(ctrl&0xef)==0xef?2:3);acl_in(local_cid,packet,at);}
static void mcc_in(unsigned type,unsigned command,const uint8_t *p,unsigned n)
{uint8_t packet[40]={(uint8_t)(type*4+command*2+1),(uint8_t)(n*2+1)};memcpy(packet+2,p,n);rf_in(0,0xef,packet,n+2,-1);}
static void command_reply(unsigned op,const uint8_t *p,unsigned n)
{
    uint8_t r[80]={1,(uint8_t)op,(uint8_t)(op>>8),0};unsigned length=4;
    if(op==0x0c03){}
    else if(op==0x1001){length=12;r[4]=13;}
    else if(op==0x1002){length=68;uint8_t *c=r+4;c[0]=0x30;c[1]=3;c[2]=0x20;c[5]=0x40;c[6]=1;c[7]=0xcc;c[8]=3;c[10]=0x18;c[16]=0x20;c[18]=0x0c;c[29]=0x18;c[63]=0x70;}
    else if(op==0x1003){length=12;r[5]=0x28;r[6]=1;
#ifdef BLUETOOTH_CHECK_MSBC
        if(!msbc_no_cap)r[6]|=8;
#endif
r[7]=0x80;r[9]=0x20;r[10]=8;}
    else if(op==0x1005){length=11;const uint8_t b[]={64,0,60,8,0,3,0};memcpy(r+4,b,7);}
    else if(op==0x1009){length=10;memcpy(r+4,"123456",6);}
    else if(op==0x0c15){length=6;put16(r+4,profile.headset.accept_slots);}
    else if(op==0x0c1b){length=8;put16(r+4,profile.headset.page_interval);put16(r+6,profile.headset.page_window);}
    else if(op==0x0c2e||op==0x0c5a||op==0x0c55||op==0x0c79){length=5;r[4]=1;}
    else if(op==0x0c19){length=5;r[4]=2;}
    else if(op==0x1004){length=14;r[4]=p[0];r[5]=0;}
    else if(op==0x0409){cs(&wire,op,1,0);const uint8_t conn[]={0,0x23,0,1,2,3,4,5,6,1,0};event(&wire,3,conn,sizeof(conn));
        /* Link events a real controller may emit after connection, more than
         * the four receive slots; no stack owner consumes them. */
        if(informational){const uint8_t slots[]={0x23,0,5},psrm[]={1,2,3,4,5,6,1},lsto[]={0x23,0,0x80,0x0c},
            role[]={0,1,2,3,4,5,6,1},mode[]={0,0x23,0,2,0x20,0},vendor[]={0x42};
            event(&wire,0x1b,slots,3);event(&wire,0x20,psrm,7);event(&wire,0x38,lsto,4);
            event(&wire,0x12,role,8);event(&wire,0x14,mode,6);event(&wire,0xff,vendor,1);}
        return;}
    else if(op==0x042a){assert(informational&&n==7&&!memcmp(p,"\1\2\3\4\5\6",6)&&p[6]==0x0d);cs(&wire,op,1,0);
        const uint8_t failed[]={0x0d,0,0,1,2,3,4,5,6,2,0,0,0,0,0,0,2};event(&wire,0x2c,failed,sizeof(failed));return;}
    else if(op==0x0411){
        assert(n==2&&get16(p)==0x23);cs(&wire,op,1,0);
        const uint8_t io[]={1,2,3,4,5,6,3,0,4};event(&wire,0x32,io,sizeof(io));
        const uint8_t req[]={1,2,3,4,5,6};event(&wire,0x31,req,sizeof(req));return;
    }
    else if(op==0x042b){length=10;memcpy(r+4,p,6);const uint8_t confirm[]={1,2,3,4,5,6,0,0,0,0};event(&wire,0x33,confirm,sizeof(confirm));}
    else if(op==0x042d){assert(deny);++attempt;length=10;memcpy(r+4,p,6);}
    else if(op==0x042c){
        assert(!deny);++attempt;length=10;memcpy(r+4,p,6);
        uint8_t key[23]={1,2,3,4,5,6};memset(key+6,0x5a,16);key[22]=4;event(&wire,0x18,key,sizeof(key));
        const uint8_t complete[]={0,1,2,3,4,5,6};event(&wire,0x36,complete,sizeof(complete));
        const uint8_t auth[]={0,0x23,0};event(&wire,6,auth,sizeof(auth));
    }
    else if(op==0x0413){cs(&wire,op,1,0);const uint8_t encrypted[]={0,0x23,0,1};event(&wire,8,encrypted,sizeof(encrypted));return;}
    else if(op==0x1408){length=7;memcpy(r+4,p,2);r[6]=16;}
    else if(op==0x041b){cs(&wire,op,1,0);uint8_t features[]={0,0x23,0,0,0x28,1,0x80,0,0x20,0,0};
#ifdef BLUETOOTH_CHECK_MSBC
        if(!msbc_no_cap)features[5]|=8;
#endif
        event(&wire,0x0b,features,sizeof(features));return;}
    else if(op==0x043d){assert(n==59);
#ifdef BLUETOOTH_CHECK_MSBC
        ++msbc_setups;assert(voice.ag.selected_codec==msbc_selected||msbc_no_cap);
        if(p[10]==3){assert(get16(p+54)==13||get16(p+54)==8);assert(get16(p+56)==0x380||get16(p+56)==0x3c8);
            assert(p[15]==3&&p[32]==3&&p[37]==3&&p[48]==0&&p[49]==0&&p[50]==0&&p[51]==0);
            if(msbc_refuse){cs(&wire,op,1,0x0c);return;}
            if(msbc_fallback){cs(&wire,op,1,0);const uint8_t failed[]={0x1c,0,0,1,2,3,4,5,6,2,0,0,0,0,0,0,3};event(&wire,0x2c,failed,sizeof(failed));return;}
        }
#endif
        cs(&wire,op,1,0);uint8_t sync[]={0,0,0,1,2,3,4,5,6,2,6,2,30,0,30,0,2};
#ifdef BLUETOOTH_CHECK_MSBC
        if(p[10]==3)sync[16]=3;
#endif
        event(&wire,0x2c,sync,sizeof(sync));return;}
    else if(op==0x0406){assert(n==3&&!get16(p)&&p[2]==0x13);cs(&wire,op,1,0);return;}
    else assert(op==0x0c01||op==0x0c05||op==0x0c16||op==0x0c1c||op==0x0c2f||op==0x0c5b||op==0x0c1a||op==0x0c56);
    event(&wire,0x0e,r,length);
    if(op==0x0c19&&handoff){const uint8_t partial[]={4,0xff};bytes(&wire,partial,2);}
}
static void peer_acl(const uint8_t *p,unsigned n)
{
    assert(n>=4);unsigned cid=get16(p+2);p+=4;n-=4;
    if(cid==1){assert(n>=4);unsigned code=p[0],id=p[1];const uint8_t *body=p+4;
        if(code==3){assert(get16(body+4)==0);local_cid=(uint16_t)get16(body);}
        else if(code==4){uint8_t answer[6];put16(answer,local_cid);memset(answer+2,0,4);sig_in(5,id,answer,6);
            uint8_t config[8];put16(config,local_cid);config[2]=config[3]=0;config[4]=1;config[5]=2;put16(config+6,128);sig_in(4,55,config,8);}
        else if(code==5){assert(get16(body+4)==0);rf_in(0,0x3f,NULL,0,-1);rf_stage=1;}
        else assert(0);
    } else {assert(cid==0x61&&n>=4);unsigned dlci=p[0]>>2,ctrl=p[1];
        if(ctrl==0x73&&dlci==0){const uint8_t pn[]={14,0xf0,7,0,120,0,0,100};mcc_in(0x20,1,pn,8);}
        else if(ctrl==0x73&&dlci==14){}
        else {assert((ctrl&0xef)==0xef);unsigned count=p[2]>>1;const uint8_t *data=p+3;
            if(!dlci){unsigned type=data[0]>>2,cr=(data[0]>>1)&1;
                if(type==0x20){assert(!cr);rf_in(14,0x3f,NULL,0,-1);}
                else if(type==0x38){const uint8_t msc[]={59,0x8d};if(cr){mcc_in(0x38,0,msc,2);mcc_in(0x38,1,msc,2);}else rf_in(14,0xff,NULL,0,100);}
                else assert(0);
            }else {assert(dlci==14);(void)count;
#ifdef BLUETOOTH_CHECK_MSBC
                for(unsigned i=0;i+7<count;++i)if(!memcmp(data+i,"+BCS: ",6)){
                    msbc_selected=data[i+6]-'0';char response[20];snprintf(response,sizeof(response),"AT+BCS=%u\r",msbc_bad_bcs?3:msbc_selected);
                    rf_in(14,0xef,(const uint8_t *)response,(unsigned)strlen(response),-1);
                }
#endif
            }}
    }
}
static uint8_t assembled[1028];static unsigned assembled_bytes,assembled_total;
static void wire_outputs(void)
{
    while(output_cursor<wire.tn){const uint8_t *p=wire.tx+output_cursor;unsigned n=0,head;
        if(p[0]==1){head=4;if(wire.tn-output_cursor<head)return;n=p[3];}
        else if(p[0]==2){head=5;if(wire.tn-output_cursor<head)return;n=get16(p+3);}
        else {assert(p[0]==3);head=4;if(wire.tn-output_cursor<head)return;n=p[3];}
        if(wire.tn-output_cursor<head+n)return;
        if(p[0]==1)command_reply(get16(p+1),p+4,n);
        else {uint8_t credits[]={1,p[1],(uint8_t)(p[2]&15),1,0};
#ifdef BLUETOOTH_CHECK_PLAYBACK
            if(p[0]==3)playback_observe(p+4,n);
            if(p[0]!=3||!playback_hold_credits)
#endif
            event(&wire,0x13,credits,5);if(p[0]==2){
            if((p[2]>>4)==2){assert(!assembled_bytes&&n>=4);assembled_total=get16(p+5)+4;}
            else assert((p[2]>>4)==1&&assembled_bytes);
            assert(n<=sizeof(assembled)-assembled_bytes);memcpy(assembled+assembled_bytes,p+5,n);assembled_bytes+=n;
            if(assembled_bytes==assembled_total){peer_acl(assembled,assembled_bytes);assembled_bytes=assembled_total=0;}
        }}
        output_cursor+=head+n;
    }
}
static int pump(void)
{wire.room=1024;++wire.now;int r=bluetooth_application_poll(&app);wire_outputs();return r;}
static void until(unsigned phase)
{for(unsigned i=0;i<2000&&voice.stage!=phase;++i){int r=pump();if(r<0)fprintf(stderr,"failure phase=%u HCI=%u acl=%u sec=%u config=%u now=%llu\n",voice.stage,app.hci.state,voice.acl.state,voice.security.fatal,voice.security.configuration_stage,(unsigned long long)wire.now);assert(r>=0);}assert(voice.stage==phase);}
static unsigned owned_records(void)
{unsigned n=0;for(unsigned i=0;i<app.hci.transport.count;++i)n+=app.hci.transport.records[i].owner==BT_H4_OWNED;return n;}
static void setup(void)
{
    wire.now=1;wire.room=1024;providers.used=1;providers.boot.identity=99;atomic_init(&recovery,1);
    profile=(struct bluetooth_profile){.magic=BLUETOOTH_PROFILE_MAGIC,.version=2,.bytes=752,
        .topology=2,.sid_authority=7,.radio_authority=3,.uart_authority=4,.wake_authority=5,
        .wiring_evidence={1},.sid_evidence={2},.fresh_transport_evidence={3},.pm_latch={6,7,0x3f,0},
        .radio={.owner=20,.topology={.identity=2}},.wake={.authority=5,.desired={{1,0,0},{0,0,0}}},
        .uart={.owner=21,.route=A733_UART1_BLUETOOTH,.format={1,8,2,0,1,24000000,1500000,0}},
        .firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},.firmware_bytes={1708,24744,23288,341260},
        .session_owner=22,.runtime_lease_us=10000000,.renew_margin_us=3000000,
        .command_timeout_us=500000,.frame_timeout_us=100000,.binding_timeout_us=1000000,
        .headset={.mode=1,.incoming=1,.peer={1,2,3,4,5,6},.allow_just_works=!deny,.minimum_security=2,
          .server_channel=7,.scheduling_authority=17,.scheduling_evidence={1},.receive_latency_us=1000,
          .response_timeout_us=2000,.samples=4,.mtu=128,.accept_slots=0x1fa0,.page_interval=0x800,.page_window=0x12,
          .line_timeout_us=1000000,.capture_timeout_us=1000000}};
    if(speech){profile.headset.samples=24000;profile.headset.capture_timeout_us=5000000;}
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
    profile.version=3;profile.bytes=832;
    memcpy(profile.recognition.bytes,bluetooth_model_bytes,sizeof(bluetooth_model_bytes));
    memcpy(profile.recognition.sha256,bluetooth_model_sha256,sizeof(bluetooth_model_sha256));
#endif
#ifdef BLUETOOTH_CHECK_MSBC
    profile.headset.mode=3;profile.headset.samples=speech?24000:120;
#elif defined(BLUETOOTH_CHECK_REPEATED)
    profile.headset.mode=2;
#endif
#ifdef BLUETOOTH_CHECK_PLAYBACK
    profile.headset.mode=playback_fixture_mode;profile.headset.samples=1200;
    if(playback_fixture_mode==4){profile.version=2;profile.bytes=752;}
#endif
    uint8_t digest[32];storage_digest_bytes(&profile,profile.bytes,digest);
    assert(bluetooth_application_init(&app,&profile,&providers,&chip,owner,&recovery,0,60000000,digest));
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
    assert(app.radio_config.deadline_us==30000000&&app.session_config.bootstrap_deadline==30000000&&app.hci_deadline==35000000&&app.deadline==59990000);
    assert(stage_deadline(&app,48000000)==40000000&&stage_deadline(&app,59500000)==40000000);
    if(profile.version==3){profile.recognition.sha256[0][0]^=1;storage_digest_bytes(&profile,profile.bytes,digest);
    assert(!bluetooth_profile_valid(&profile,digest));profile.recognition.sha256[0][0]^=1;
    storage_digest_bytes(&profile,profile.bytes,digest);assert(bluetooth_profile_valid(&profile,digest));}
#endif
    scheduler=taskSCHEDULER_RUNNING;current_task=owner;bootstrap_ready=1;
}
#ifndef BLUETOOTH_HEADSET_FIXTURE_ENTRY
#define BLUETOOTH_HEADSET_FIXTURE_ENTRY main
#endif
int BLUETOOTH_HEADSET_FIXTURE_ENTRY(int argc,char **argv)
{
    assert(argc==2||argc==4);speech=!strcmp(argv[1],"speech");deny=!strcmp(argv[1],"justworks");loss=!strcmp(argv[1],"loss");handoff=!strcmp(argv[1],"handoff");
    informational=!strcmp(argv[1],"informational");setup();
    if(handoff){
        until(SECURITY_CONFIG);struct bt_h4 *h=&app.hci.transport;
        assert(h->assembling<h->count&&voice.acl.l2cap_owner==&voice.l2cap);
        unsigned assembling=h->assembling;uint64_t serial=h->records[assembling].serial;
        struct bt_l2cap duplicate={0};app.iteration_live=1;
        assert(bt_l2cap_init_bootstrap(&duplicate,&voice.acl,128,1000000,60000000,policy,&context,&h->binding)==-1);
        app.iteration_live=0;assert(h->assembling==assembling&&h->records[assembling].serial==serial);
        uint8_t length=0;bytes(&wire,&length,1);
    }
    if(!strcmp(argv[1],"early-copy")){
        uint8_t out[1024];size_t copied=99;struct bluetooth_pcm_snapshot snapshot;
        memset(&snapshot,0xa5,sizeof(snapshot));
        const struct bluetooth_pcm_snapshot zero={0};
        assert(bluetooth_headset_copy(&app,0,0,8,out,&copied,&snapshot)==0&&!copied&&!memcmp(&snapshot,&zero,sizeof(zero)));
    }
    until(CONNECT);
    if(!strcmp(argv[1],"security-pending")){
        app.iteration_live=1;uint64_t channel=0;
        assert(policy(&context,voice.link,1,1)==-1&&service(&context,voice.link,0x111f,7)==-1);
        assert(bt_l2cap_open(&voice.l2cap,voice.link,1,10000000,&channel)==-1&&!channel);
        app.iteration_live=0;puts("PASS pending authentication refuses PSM/service admission");return 0;
    }
    const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,1};
    event(&wire,4,request,sizeof(request));
    if(deny){for(unsigned i=0;i<500&&!attempt;++i)assert(pump()>=0);assert(attempt==1);assert(!voice.security.connections[0].authenticated);puts("PASS initiator Just Works refusal uses actual upstream SSP handler");return 0;}
    until(CONTROL);uint64_t generation;unsigned sc;
    app.iteration_live=1;assert(bt_security_authorized(&voice.security,voice.link,&generation,&sc)==1&&!sc);app.iteration_live=0;
    if(!strcmp(argv[1],"keyreplace")){
        uint8_t key[23]={1,2,3,4,5,6};memset(key+6,0xa5,16);key[22]=4;event(&wire,0x18,key,sizeof(key));
        assert(pump()==-1&&(atomic_load(&recovery)&4));puts("PASS authenticated key replacement retains without inherited proof");return 0;
    }
    if(!strcmp(argv[1],"deadline")){wire.now=voice.operation_deadline;assert(pump()==-1);puts("PASS operation deadline retains entire native owner");return 0;}
    const uint8_t connect[]={3,0,0x61,0};sig_in(2,50,connect,4);until(AG);
    assert(rf_stage);const char *at="AT+BRSF=0\rAT+CIND=?\rAT+CIND?\rAT+CMER=3,,,1\r";rf_in(14,0xef,(const uint8_t *)at,(unsigned)strlen(at),-1);at_sent=1;
    until(CAPTURE);assert(at_sent&&voice.ag.phase==BT_HFP_AG_READY);
    if(informational){const uint8_t request[]={1,2,3,4,5,6,4,4,0x24,2};event(&wire,4,request,sizeof(request));}
    if(speech){
        assert(argc==4);FILE *input=fopen(argv[2],"rb");assert(input);
        uint8_t data[48000];assert(fread(data,1,sizeof(data),input)==sizeof(data)&&fgetc(input)==EOF&&!fclose(input));
        for(unsigned offset=0;offset<sizeof(data);){unsigned count=sizeof(data)-offset;if(count>255)count=255;
            uint8_t header[]={3,0,0,(uint8_t)count};bytes(&wire,header,4);bytes(&wire,data+offset,count);
            for(unsigned i=0;i<4;++i)assert(pump()>=0);offset+=count;
        }
        audio_injected=1;
    }else {
    uint8_t pcm1[]={3,0,(uint8_t)(loss?0x10:0),3,1,2,3},pcm2[]={3,0,0,5,4,5,6,7,8};bytes(&wire,pcm1,sizeof(pcm1));bytes(&wire,pcm2,sizeof(pcm2));audio_injected=1;
    }
    until(CLOSE);assert(audio_injected);
    uint8_t out[1024];size_t copied;struct bluetooth_pcm_snapshot snapshot;
    assert(bluetooth_headset_copy(&app,0,0,8,out,&copied,&snapshot)==0&&!copied&&!snapshot.bytes);
    for(unsigned i=0;i<8;++i)assert(pump()>=0);
    assert(voice.audio.links[0].phase==BT_SCO_DISCONNECT_WAIT);
    const uint8_t closed[]={0,0,0,0x13};event(&wire,5,closed,sizeof(closed));disconnected=1;
    if(loss){assert(pump()==-1);assert(bluetooth_headset_copy(&app,0,0,8,out,&copied,&snapshot)==-1);puts("PASS loss metadata prevents successful PCM publication");return 0;}
    until(DONE);assert(disconnected);
    if(informational){assert(!owned_records()&&!voice.audio.refuse&&voice.audio.next_link==2);
        puts("PASS informational link events and a stray eSCO request free every receive slot");return 0;}
    if(speech){
        assert(voice.snapshot.bytes==48000&&!voice.snapshot.loss_packets);
        FILE *output=fopen(argv[3],"wb");assert(output);
        for(unsigned offset=0;offset<48000;){size_t count=48000-offset;if(count>1024)count=1024;
            assert(bluetooth_headset_copy(&app,0,offset,count,out,&copied,&snapshot)==1&&copied==count);
            assert(fwrite(out,1,copied,output)==copied);offset+=(unsigned)count;
        }
        assert(!fclose(output));puts("PASS actual modeled security/control/SCO disconnect and48000 copied speech bytes");return 0;
    }
    assert(bluetooth_headset_copy(&app,0,0,8,out,&copied,&snapshot)==1&&copied==8&&!memcmp(out,"\1\2\3\4\5\6\7\10",8));
    assert(snapshot.records==2&&!snapshot.loss_packets&&snapshot.bytes==8);
    assert(bluetooth_headset_copy(&app,1,0,48,out,&copied,&snapshot)==1&&copied==48);
    struct bluetooth_pcm_record records[2];memcpy(records,out,48);assert(records[0].source_bytes==3&&records[0].copied_bytes==3&&records[1].offset==3&&records[1].source_bytes==5);
    assert(bluetooth_headset_copy(&app,0,7,2,out,&copied,&snapshot)==-1);
    puts("PASS native security to RFCOMM/AG to odd-segment PCM and confirmed-disconnect publication");return 0;
}
