/* Native artifact-bound radio firmware engine. Protocol/artifact identities and
 * SHA-256 specification references are documented in docs/radio-firmware.md. */
#include "aic8800_firmware.h"
#include "a733_range.h"

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
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static unsigned nibble(char c) { return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10); }
static void hash_init(struct aic8800_firmware *f)
{ storage_digest_init(&f->digest); f->hash_offset = 0; }
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
    if (!a733_span_excl(f,sizeof(*f)) || !a733_span_excl(c,sizeof(*c)) || a733_extents_overlap_prechecked((uintptr_t)f,sizeof(*f),(uintptr_t)c,sizeof(*c)) ||
        f->initialized || c->profile != 1 || c->table_bursts > 1) return 0;
    struct aic8800_sdio_response chip={.is_memory_read=1,.address=AIC8800_SDIO_CHIP_ADDRESS,.value=c->identity.chip_word};
    struct aic8800_sdio_response sub={.is_memory_read=1,.address=AIC8800_SDIO_SUBREV_ADDRESS,.value=c->identity.subrevision_word};
    struct aic8800_radio_identity id;
    if (aic8800_sdio_identity(&chip,&sub,&id) != AIC8800_CODEC_OK || !id.known_revision ||
        (!id.is_h && id.revision != 3 && id.revision != 7)) return 0;
    for (unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i) {
        const struct aic8800_firmware_blob *b=&c->blobs[i];
        if (b->bytes != sizes[i == AIC8800_WIFI_IMAGE && id.is_h ? 4 : i] || !a733_span_excl(b->data,b->bytes) ||
            a733_extents_overlap_prechecked((uintptr_t)f,sizeof(*f),(uintptr_t)b->data,b->bytes) ||
            a733_extents_overlap_prechecked((uintptr_t)c,sizeof(*c),(uintptr_t)b->data,b->bytes)) return 0;
        for (unsigned j=0;j<i;++j)
            if (a733_extents_overlap_prechecked((uintptr_t)b->data,b->bytes,(uintptr_t)c->blobs[j].data,c->blobs[j].bytes)) return 0;
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
    size_t bytes=b->bytes-f->hash_offset;
    if (bytes>AIC8800_FIRMWARE_HASH_STEP_BYTES) bytes=AIC8800_FIRMWARE_HASH_STEP_BYTES;
    (void)storage_digest_add(&f->digest,b->data+f->hash_offset,bytes);f->hash_offset+=bytes;
    if (f->hash_offset<b->bytes) return;
    uint8_t actual[32];storage_digest_end(&f->digest,actual);
    const char *wanted=hashes[f->hash_role==AIC8800_WIFI_IMAGE && f->h_variant ? 4 : f->hash_role];
    for (unsigned i=0;i<32;++i) if (actual[i] != ((nibble(wanted[2*i])<<4)|nibble(wanted[2*i+1]))) {
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
static void packet(struct aic8800_firmware *f,uint16_t id,unsigned parameters)
{
    for(size_t i=0;i<sizeof(f->request);++i)f->request[i]=0;
    aic8800_sdio_command_header(f->request,id,(uint16_t)parameters);
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
/* Table bursts cover consecutive word addresses below the 0x40000000 register
 * space, within one record, and only where they replace many word writes. */
#define BURST_MIN_PAIRS 16u
#define BURST_MAX_PAIRS 256u
static uint32_t table_run(const struct aic8800_firmware *f,const uint8_t *pairs)
{
    uint32_t first=le32(pairs+f->pair_index*8),n=0;
    if(!f->config.table_bursts||f->table_type==3||(first&3))return 0;
    while(n<BURST_MAX_PAIRS&&f->pair_index+n<f->table_pairs&&
        le32(pairs+(f->pair_index+n)*8)==first+4*n&&first+4*n<0x40000000u)++n;
    return n>=BURST_MIN_PAIRS?n:0;
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
        f->table_burst=table_run(f,table+f->table_offset+24);
        if(f->table_burst){
            const uint8_t *pair=table+f->table_offset+24+f->pair_index*8;
            packet(f,0x040b,1032);put32(f->request+16,le32(pair));put32(f->request+20,f->table_burst*4);
            for(uint32_t i=0;i<f->table_burst;++i)put32(f->request+24+4*i,le32(pair+8*i+4));
            break;
        }
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
static void block_status(struct aic8800_firmware *f,const struct aic8800_sdio_response *r)
{
    f->last_status=le32(r->parameters);
    if(f->last_status&&!f->first_nonzero_block_status)f->first_nonzero_block_status=f->last_status;
}
static void advance(struct aic8800_firmware *f,uint64_t now,const struct aic8800_sdio_response *r)
{
    f->state=AIC8800_FW_PREPARING;
    if(f->phase==LOAD_ADID||f->phase==LOAD_BT_PATCH||f->phase==LOAD_WIFI){
        unsigned role=f->phase==LOAD_ADID?AIC8800_BT_ADID:f->phase==LOAD_BT_PATCH?AIC8800_BT_PATCH:AIC8800_WIFI_IMAGE;
        size_t left=f->config.blobs[role].bytes-f->image_offset;f->image_offset+=left>1024?1024:left;
        block_status(f,r);
        if(f->image_offset==f->config.blobs[role].bytes){f->image_offset=0;++f->phase;}
    }else if(f->phase==APPLY_BT_TABLE){
        if(f->table_burst)block_status(f,r);
        f->pair_index+=f->table_burst?f->table_burst:1;f->table_burst=0;
        if(f->pair_index==f->table_pairs){
            f->pair_index=0;f->table_offset+=24+(size_t)f->table_pairs*8;
            if(f->table_type==4){
                if(!a733_deadline_after(now,f->power_delay_ticks,&f->delay_until)){aic8800_firmware_stop(f,f->owner,AIC8800_FW_TIME_ERROR);return;}
                f->state=AIC8800_FW_DELAY;
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
        !a733_span_excl(input,available)||!a733_span_excl(output,sizeof(*output))||
        a733_extents_overlap_prechecked((uintptr_t)f,sizeof(*f),(uintptr_t)input,available)||
        a733_extents_overlap_prechecked((uintptr_t)f,sizeof(*f),(uintptr_t)output,sizeof(*output))||
        a733_extents_overlap_prechecked((uintptr_t)input,available,(uintptr_t)output,sizeof(*output)))return AIC8800_CODEC_INVALID;
    for(unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i)
        if(a733_extents_overlap_prechecked((uintptr_t)output,sizeof(*output),(uintptr_t)f->config.blobs[i].data,
            f->config.blobs[i].bytes))return AIC8800_CODEC_INVALID;
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
