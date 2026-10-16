/* Native artifact-bound radio firmware engine. Protocol/artifact identities and
 * SHA-256 specification references are documented in docs/radio-firmware.md. */
#include "aic8800_firmware.h"

enum { LOAD_ADID, LOAD_BT_PATCH, APPLY_BT_TABLE, LOAD_WIFI, READ_POINTERS,
    APPLY_WIFI_PATCH, START_WIFI };
static const size_t sizes[] = { 1708, 24744, 23288, 341212, 341260 };
static const char *const hashes[] = {
    "a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56",
    "4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0",
    "5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60",
    "55b5a20865aca7cd2edaae1733a36da45d2b0b29060bc243d8e9ec212e139f6c",
    "ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7"
};
static int span(const void *p, size_t n) { return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{ return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an; }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static unsigned nibble(char c) { return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10); }
static uint32_t rotate(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
static void hash_init(struct aic8800_firmware *f)
{
    static const uint32_t initial[] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    for (unsigned i = 0; i < 8; ++i) f->hash[i] = initial[i];
    f->hash_offset = 0; f->hash_padding = 0;
}
static void hash_block(struct aic8800_firmware *f, const uint8_t *p)
{
    static const uint32_t k[] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64], a=f->hash[0],b=f->hash[1],c=f->hash[2],d=f->hash[3],
        e=f->hash[4],g=f->hash[6],h=f->hash[7],v=f->hash[5];
    for (unsigned i=0;i<16;++i) w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
    for (unsigned i=16;i<64;++i) {
        uint32_t x=w[i-15],y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    for (unsigned i=0;i<64;++i) {
        uint32_t t1=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&v)^(~e&g))+k[i]+w[i];
        uint32_t t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=v;v=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    f->hash[0]+=a;f->hash[1]+=b;f->hash[2]+=c;f->hash[3]+=d;
    f->hash[4]+=e;f->hash[5]+=v;f->hash[6]+=g;f->hash[7]+=h;
}
void aic8800_firmware_stop(struct aic8800_firmware *f, const void *owner, enum aic8800_firmware_error error)
{
    if (!f || !f->initialized || f->owner != owner || f->state == AIC8800_FW_FAULT) return;
    f->error = error; f->state = AIC8800_FW_FAULT;
}
static int observe(struct aic8800_firmware *f, uint64_t now)
{
    if (now < f->last_now || now >= f->deadline) {
        aic8800_firmware_stop(f,f->owner,AIC8800_FW_TIME_ERROR); return 0;
    }
    f->last_now=now;return 1;
}
int aic8800_firmware_init(struct aic8800_firmware *f, const struct aic8800_firmware_config *c)
{
    if (!span(f,sizeof(*f)) || !span(c,sizeof(*c)) || overlap(f,sizeof(*f),c,sizeof(*c)) ||
        f->initialized || c->profile != 1) return 0;
    struct aic8800_sdio_response chip={.is_memory_read=1,.address=AIC8800_SDIO_CHIP_ADDRESS,.value=c->identity.chip_word};
    struct aic8800_sdio_response sub={.is_memory_read=1,.address=AIC8800_SDIO_SUBREV_ADDRESS,.value=c->identity.subrevision_word};
    struct aic8800_radio_identity id;
    if (aic8800_sdio_identity(&chip,&sub,&id) != AIC8800_CODEC_OK || !id.known_revision ||
        (!id.is_h && id.revision != 3 && id.revision != 7)) return 0;
    for (unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i) {
        const struct aic8800_firmware_blob *b=&c->blobs[i];
        if (b->bytes != sizes[i == AIC8800_WIFI_IMAGE && id.is_h ? 4 : i] || !span(b->data,b->bytes) ||
            overlap(f,sizeof(*f),b->data,b->bytes) || overlap(c,sizeof(*c),b->data,b->bytes)) return 0;
        for (unsigned j=0;j<i;++j) if (overlap(b->data,b->bytes,c->blobs[j].data,c->blobs[j].bytes)) return 0;
    }
    struct aic8800_firmware initial={0}; initial.config=*c;initial.config.identity=id;
    initial.initialized=1;initial.h_variant=id.is_h;*f=initial;return 1;
}
int aic8800_firmware_claim(struct aic8800_firmware *f, const struct aic8800_radio_identity *id,
    const void *owner, uint64_t last_now, uint64_t deadline, uint64_t delay)
{
    if (!f || !id || !owner || !f->initialized || f->state != AIC8800_FW_NEW || f->owner ||
        deadline <= last_now || !delay || id->chip_word != f->config.identity.chip_word ||
        id->subrevision_word != f->config.identity.subrevision_word) return 0;
    f->owner=owner;f->last_now=last_now;f->deadline=deadline;f->power_delay_ticks=delay;
    f->state=AIC8800_FW_VERIFYING;hash_init(f);return 1;
}
static void hash_step(struct aic8800_firmware *f)
{
    const struct aic8800_firmware_blob *b=&f->config.blobs[f->hash_role];
    size_t left=b->bytes-f->hash_offset;
    if (left>=64) {hash_block(f,b->data+f->hash_offset);f->hash_offset+=64;return;}
    uint8_t tail[64]={0};
    if (!f->hash_padding) {
        for (size_t i=0;i<left;++i) tail[i]=b->data[f->hash_offset+i];
        tail[left]=0x80;
        if (left>55) {hash_block(f,tail);f->hash_padding=1;return;}
    }
    uint64_t bits=(uint64_t)b->bytes*8;
    for (unsigned i=0;i<8;++i) tail[63-i]=(uint8_t)(bits>>(8*i));
    hash_block(f,tail);
    const char *wanted=hashes[f->hash_role==AIC8800_WIFI_IMAGE && f->h_variant ? 4 : f->hash_role];
    for (unsigned i=0;i<32;++i) if (((f->hash[i/4]>>(24-8*(i%4)))&255) !=
        ((nibble(wanted[2*i])<<4)|nibble(wanted[2*i+1]))) {
        aic8800_firmware_stop(f,f->owner,AIC8800_FW_HASH_ERROR);return;
    }
    if (++f->hash_role<AIC8800_FIRMWARE_BLOBS) hash_init(f);
    else {f->state=AIC8800_FW_VALIDATING;f->table_offset=16;}
}
static int record(struct aic8800_firmware *f)
{
    const struct aic8800_firmware_blob *b=&f->config.blobs[AIC8800_BT_TABLE];
    if (f->table_offset>b->bytes || b->bytes-f->table_offset<24) return 0;
    f->table_type=le32(b->data+f->table_offset+16);f->table_pairs=le32(b->data+f->table_offset+20);
    return f->table_type<=6 && f->table_pairs<=(b->bytes-f->table_offset-24)/8 &&
        (f->table_type!=3 || f->table_pairs==9);
}
static void validate_step(struct aic8800_firmware *f)
{
    const struct aic8800_firmware_blob *b=&f->config.blobs[AIC8800_BT_TABLE];
    if (f->table_offset==b->bytes) {
        f->state=AIC8800_FW_PREPARING;f->phase=LOAD_ADID;f->table_offset=16;return;
    }
    if (!record(f)) {aic8800_firmware_stop(f,f->owner,AIC8800_FW_FORMAT_ERROR);return;}
    if (f->table_offset==16 && (f->table_type!=0 || f->table_pairs!=4 ||
        le32(b->data+44)!=0x201940 || le32(b->data+52)!=0x1e0000)) {
        aic8800_firmware_stop(f,f->owner,AIC8800_FW_FORMAT_ERROR);return;
    }
    f->table_offset+=24+(size_t)f->table_pairs*8;
}
static uint8_t crc(const uint8_t *p)
{
    uint8_t c=0;for(unsigned i=0;i<3;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(uint8_t)((c<<1)^((c&128)?7:0));}return c;
}
static void packet(struct aic8800_firmware *f,uint16_t id,unsigned parameters)
{
    for(size_t i=0;i<sizeof(f->request);++i)f->request[i]=0;
    unsigned length=12+parameters;f->request[0]=(uint8_t)length;f->request[1]=(uint8_t)(length>>8);
    f->request[2]=0x11;f->request[3]=crc(f->request);f->request[8]=(uint8_t)id;f->request[9]=(uint8_t)(id>>8);
    f->request[10]=1;f->request[12]=100;f->request[14]=(uint8_t)parameters;f->request[15]=(uint8_t)(parameters>>8);
    f->request_bytes=((16u+parameters+4u+511u)/512u)*512u;f->expected_message=id+1;
    f->state=AIC8800_FW_REQUEST;
}
static void word_write(struct aic8800_firmware *f,uint32_t address,uint32_t value)
{packet(f,0x0402,8);put32(f->request+16,address);put32(f->request+20,value);}
static void image_step(struct aic8800_firmware *f,unsigned role,uint32_t address)
{
    const struct aic8800_firmware_blob *b=&f->config.blobs[role];
    size_t bytes=b->bytes-f->image_offset;if(bytes>1024)bytes=1024;
    packet(f,0x040b,1032);put32(f->request+16,address+(uint32_t)f->image_offset);put32(f->request+20,(uint32_t)bytes);
    for(size_t i=0;i<bytes;++i)f->request[24+i]=b->data[f->image_offset+i];
}
static void prepare_step(struct aic8800_firmware *f)
{
    const uint8_t *table=f->config.blobs[AIC8800_BT_TABLE].data;
    static const uint32_t mode[]={1,0xffffffff,0,5,2,1500000,1,1,0x6f2f};
    static const uint32_t offsets[]={0x198,0x1a0,0x1c,0x1a4};
    switch(f->phase){
    case LOAD_ADID:image_step(f,AIC8800_BT_ADID,0x201940);break;
    case LOAD_BT_PATCH:image_step(f,AIC8800_BT_PATCH,0x1e0000);break;
    case APPLY_BT_TABLE:
        if(f->table_offset==f->config.blobs[AIC8800_BT_TABLE].bytes){f->bt_patch_applied=1;f->phase=LOAD_WIFI;break;}
        if(!record(f)){aic8800_firmware_stop(f,f->owner,AIC8800_FW_FORMAT_ERROR);break;}
        if(f->table_type==6){f->table_offset+=24+(size_t)f->table_pairs*8;break;}
        word_write(f,le32(table+f->table_offset+24+f->pair_index*8),f->table_type==3?mode[f->pair_index]:
            le32(table+f->table_offset+28+f->pair_index*8));break;
    case LOAD_WIFI:image_step(f,AIC8800_WIFI_IMAGE,0x120000);break;
    case READ_POINTERS:
        f->expected_address=0x120000+offsets[f->patch_index];packet(f,0x0400,4);put32(f->request+16,f->expected_address);break;
    case APPLY_WIFI_PATCH:{
        uint32_t address,value=0,base=f->pointers[1],pairs=f->pointers[3];
        switch(f->patch_index){
        case 0:address=base;value=0x48435450;break;
        case 1:address=base+8;value=0x50544348;break;
        case 2:address=base+4;value=pairs;break;
        case 3:address=base+12;value=1;break;
        case 4:address=pairs;value=f->pointers[0]+0x170;break;
        case 5:address=pairs+4;value=0x0100000a;break;
        default:address=base+48+(f->patch_index-6)*4;break;
        }word_write(f,address,value);break;}
    case START_WIFI:packet(f,0x040d,8);put32(f->request+16,0x120000);put32(f->request+20,1);break;
    default:aic8800_firmware_stop(f,f->owner,AIC8800_FW_FORMAT_ERROR);break;
    }
}
void aic8800_firmware_step(struct aic8800_firmware *f,const void *owner,uint64_t now)
{
    if(!f||!f->initialized||f->owner!=owner||!owner||f->state==AIC8800_FW_FAULT||f->state==AIC8800_FW_NEW)return;
    if(!observe(f,now))return;
    if(f->state==AIC8800_FW_VERIFYING)hash_step(f);
    else if(f->state==AIC8800_FW_VALIDATING)validate_step(f);
    else if(f->state==AIC8800_FW_PREPARING)prepare_step(f);
    else if(f->state==AIC8800_FW_DELAY&&now>=f->delay_until)f->state=AIC8800_FW_PREPARING;
}
int aic8800_firmware_sent(struct aic8800_firmware *f,const void *owner)
{
    if(!f||!f->initialized||!owner||f->owner!=owner||f->state!=AIC8800_FW_REQUEST)return 0;
    f->state=AIC8800_FW_WAITING;return 1;
}
static void advance(struct aic8800_firmware *f,uint64_t now,const struct aic8800_sdio_response *r)
{
    f->state=AIC8800_FW_PREPARING;
    if(f->phase==LOAD_ADID||f->phase==LOAD_BT_PATCH||f->phase==LOAD_WIFI){
        unsigned role=f->phase==LOAD_ADID?AIC8800_BT_ADID:f->phase==LOAD_BT_PATCH?AIC8800_BT_PATCH:AIC8800_WIFI_IMAGE;
        size_t left=f->config.blobs[role].bytes-f->image_offset;f->image_offset+=left>1024?1024:left;
        f->last_status=le32(r->parameters);
        if(f->last_status&&!f->first_nonzero_block_status)f->first_nonzero_block_status=f->last_status;
        if(f->image_offset==f->config.blobs[role].bytes){f->image_offset=0;++f->phase;}
    }else if(f->phase==APPLY_BT_TABLE){
        if(++f->pair_index==f->table_pairs){
            f->pair_index=0;f->table_offset+=24+(size_t)f->table_pairs*8;
            if(f->table_type==4){
                if(f->power_delay_ticks>UINT64_MAX-now){aic8800_firmware_stop(f,f->owner,AIC8800_FW_TIME_ERROR);return;}
                f->delay_until=now+f->power_delay_ticks;f->state=AIC8800_FW_DELAY;
            }
        }
    }else if(f->phase==READ_POINTERS){
        static const uint32_t offsets[]={0x198,0x1a0,0x1c,0x1a4};
        uint32_t value=le32(r->parameters+4);
        if(value!=le32(f->config.blobs[AIC8800_WIFI_IMAGE].data+offsets[f->patch_index])){
            aic8800_firmware_stop(f,f->owner,AIC8800_FW_READBACK_ERROR);return;}
        f->pointers[f->patch_index]=value;
        if(++f->patch_index==4){f->patch_index=0;f->phase=APPLY_WIFI_PATCH;}
    }else if(f->phase==APPLY_WIFI_PATCH){if(++f->patch_index==10)f->phase=START_WIFI;}
    else if(f->phase==START_WIFI){f->bootstatus=le32(r->parameters);f->state=AIC8800_FW_START_CONFIRMED;}
}
enum aic8800_codec_result aic8800_firmware_response(struct aic8800_firmware *f,const void *owner,
    const void *input,size_t available,uint64_t now,struct aic8800_sdio_response *output)
{
    if(!f||!f->initialized||!owner||f->owner!=owner||f->state==AIC8800_FW_FAULT||
        !span(input,available)||!span(output,sizeof(*output))||overlap(f,sizeof(*f),input,available)||
        overlap(f,sizeof(*f),output,sizeof(*output))||overlap(input,available,output,sizeof(*output)))return AIC8800_CODEC_INVALID;
    for(unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i)if(overlap(output,sizeof(*output),f->config.blobs[i].data,f->config.blobs[i].bytes))return AIC8800_CODEC_INVALID;
    if(!observe(f,now))return AIC8800_CODEC_INVALID;
    struct aic8800_sdio_response r;
    enum aic8800_codec_result result=aic8800_sdio_decode_identity_response(input,available,AIC8800_IDENTITY_CHIP,&r);
    if(result!=AIC8800_CODEC_OK&&result!=AIC8800_CODEC_UNRELATED){
        if(result!=AIC8800_CODEC_END)aic8800_firmware_stop(f,owner,AIC8800_FW_PROTOCOL_ERROR);
        return result;
    }
    if(r.has_message&&r.message_id==0x0417){
        if(r.parameter_bytes!=16){aic8800_firmware_stop(f,owner,AIC8800_FW_PROTOCOL_ERROR);return AIC8800_CODEC_MALFORMED;}
        if(!f->binding_required){
            f->binding_required=1;for(unsigned i=0;i<16;++i)f->binding_challenge[i]=r.parameters[i];
        }
    }
    if(f->state!=AIC8800_FW_WAITING||!r.has_message||r.message_id!=f->expected_message){*output=r;return AIC8800_CODEC_UNRELATED;}
    unsigned bytes=(r.message_id==0x0401||r.message_id==0x0403)?8:4;
    if(r.parameter_bytes!=bytes){aic8800_firmware_stop(f,owner,AIC8800_FW_PROTOCOL_ERROR);return AIC8800_CODEC_MALFORMED;}
    if(r.message_id==0x0401&&le32(r.parameters)!=f->expected_address){*output=r;return AIC8800_CODEC_UNRELATED;}
    f->last_response_message=r.message_id;f->last_response[0]=le32(r.parameters);
    f->last_response[1]=bytes==8?le32(r.parameters+4):0;
    advance(f,now,&r);*output=r;return AIC8800_CODEC_OK;
}
