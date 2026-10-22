/* Station SAE through the native supplicant API, the pinned sae.c and the
 * minimal pinned OpenSSL build. The access point is a second sae.c instance
 * driven here in the order hostapd uses; IEEE Std 802.11-2020 Annex J.10
 * supplies the independent commit, key and PWE vectors. */
#include "aic8800_supplicant_port.h"
#include "aic8800_supplicant.h"
#include "aic8800_openssl.h"
#include "utils/common.h"
#include "utils/wpabuf.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"
#include "common/sae.h"
#include "crypto/crypto.h"
#include <assert.h>
#undef free
void *pvPortMalloc(size_t n){return malloc(n);}
void vPortFree(void *p){free(p);}
static const uint8_t sta_addr[6]={2,1,2,3,4,5},ap_addr[6]={2,17,34,51,68,85};
static const uint8_t ssid[]="sunstone";
static const uint8_t rsn_sae[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,8,128,0};
static uint64_t stream,entropy_bytes;
static int entropy_ok=1;
static int entropy(void *ctx,uint8_t *out,size_t n)
{
    (void)ctx;if(!entropy_ok)return 0;
    /* Deterministic for the reproducibility case; the arithmetic does not
     * need statistical quality. Production uses the CE entropy adapter. */
    for(size_t i=0;i<n;++i){stream^=stream<<13;stream^=stream>>7;stream^=stream<<17;out[i]=(uint8_t)(stream>>24);}
    entropy_bytes+=n;return 1;
}
static struct aic8800_supplicant sta,ap_ctx;
static struct aic8800_supplicant_config config(const char *pw,unsigned h2e)
{
    struct aic8800_supplicant_config c={.entropy=entropy,.entropy_context=&stream,.entropy_record=1,.beacon_record=2,
        .ticks_per_second=1000000,.command_timeout=1000000,.ap_rsn_bytes=sizeof(rsn_sae),
        .key_mgmt=AIC8800_SUPPLICANT_SAE,.ap_rsnx=h2e?(uint8_t)BIT(WLAN_RSNX_CAPAB_SAE_H2E):0};
    memcpy(c.ap_rsn,rsn_sae,sizeof(rsn_sae));c.sae_password_bytes=(uint8_t)strlen(pw);memcpy(c.sae_password,pw,strlen(pw));
    return c;
}
/* Access point: refuse listed groups, require a token, or tamper as asked. */
static struct {
    struct sae_data sae;struct sae_pt *pt;const char *pw;
    unsigned h2e,refuse,token_always,token_first,tokens,corrupt,reflect,accepted;
    uint8_t own_rsnx;
} ap;
static void ap_reset(const char *pw,unsigned h2e)
{
    memset(&ap_ctx,0,sizeof(ap_ctx));struct aic8800_supplicant_config c=config(pw,h2e);
    assert(aic8800_supplicant_init(&ap_ctx,&c));
    memset(&ap,0,sizeof(ap));ap.pw=pw;ap.h2e=h2e;
}
static size_t put(uint8_t *f,unsigned seq,unsigned status,const struct wpabuf *b)
{
    WPA_PUT_LE16(f,WLAN_AUTH_SAE);WPA_PUT_LE16(f+2,(u16)seq);WPA_PUT_LE16(f+4,(u16)status);
    if(b)memcpy(f+6,wpabuf_head(b),wpabuf_len(b));
    return 6+(b?wpabuf_len(b):0);
}
/* Without H2E the AP finds its token by length; hostapd tokens are 32 bytes or more. */
static const uint8_t token_value[32]={0xa5,0x5a,0x11,0x22,0x33,0x44,0x55,0x66,[31]=0x77};
/* Handle one station frame; write up to two response frames. */
static unsigned ap_receive(const uint8_t *f,size_t n,uint8_t out[2][1024],size_t sizes[2])
{
    assert(aic8800_supplicant_os_enter(&ap_ctx));
    unsigned seq=WPA_GET_LE16(f+2),status=WPA_GET_LE16(f+4),count=0;
    int groups[]={19,20,21,0};
    if(seq==1){
        unsigned group=WPA_GET_LE16(f+6);
        if(ap.refuse&(1u<<(group-19))){sizes[0]=put(out[0],1,WLAN_STATUS_FINITE_CYCLIC_GROUP_NOT_SUPPORTED,0);count=1;goto done;}
        assert(status==(ap.h2e?WLAN_STATUS_SAE_HASH_TO_ELEMENT:WLAN_STATUS_SUCCESS));
        const u8 *token=0;size_t token_len=0;
        if(ap.sae.group!=(int)group){sae_clear_data(&ap.sae);}
        u16 r=sae_parse_commit(&ap.sae,f+6,n-6,&token,&token_len,groups,ap.h2e,0);
        assert(r==WLAN_STATUS_SUCCESS);
        if(token)assert(token_len==sizeof(token_value)&&!memcmp(token,token_value,token_len));
        if(ap.token_always||(ap.token_first&&!token)){
            struct wpabuf *b=wpabuf_alloc(64);wpabuf_put_le16(b,(u16)group);
            if(ap.h2e){wpabuf_put_u8(b,WLAN_EID_EXTENSION);wpabuf_put_u8(b,1+sizeof(token_value));wpabuf_put_u8(b,WLAN_EID_EXT_ANTI_CLOGGING_TOKEN);}
            wpabuf_put_data(b,token_value,sizeof(token_value));
            /* Like hostapd, keep no state for a station asked for a token. */
            sizes[0]=put(out[0],1,WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ,b);wpabuf_free(b);sae_clear_data(&ap.sae);
            ++ap.tokens;count=1;goto done;
        }
        if(ap.reflect){memcpy(out[0],f,n);sizes[0]=n;count=1;goto done;}
        if(ap.h2e){
            if(!ap.pt)ap.pt=sae_derive_pt(groups,ssid,sizeof(ssid)-1,(const u8 *)ap.pw,strlen(ap.pw),0,0);
            assert(ap.pt&&!sae_prepare_commit_pt(&ap.sae,ap.pt,ap_addr,sta_addr,0,0));
        }else assert(!sae_prepare_commit(ap_addr,sta_addr,(const u8 *)ap.pw,strlen(ap.pw),&ap.sae));
        assert(!sae_process_commit(&ap.sae));
        struct wpabuf *b=wpabuf_alloc(SAE_COMMIT_MAX_LEN),*c=wpabuf_alloc(SAE_CONFIRM_MAX_LEN);
        assert(!sae_write_commit(&ap.sae,b,0,0,0)&&!sae_write_confirm(&ap.sae,c));
        sizes[0]=put(out[0],1,ap.h2e?WLAN_STATUS_SAE_HASH_TO_ELEMENT:WLAN_STATUS_SUCCESS,b);
        sizes[1]=put(out[1],2,WLAN_STATUS_SUCCESS,c);
        if(ap.corrupt)out[1][sizes[1]-1]^=1;
        wpabuf_free(b);wpabuf_free(c);ap.sae.state=SAE_CONFIRMED;count=2;
    }else{
        assert(seq==2&&status==0&&ap.sae.state==SAE_CONFIRMED);
        if(!sae_check_confirm(&ap.sae,f+6,n-6,0)){ap.sae.state=SAE_ACCEPTED;ap.accepted=1;}
    }
done:
    aic8800_supplicant_os_leave(&ap_ctx);return count;
}
static void ap_clear(void)
{
    assert(aic8800_supplicant_os_enter(&ap_ctx));
    sae_clear_data(&ap.sae);sae_deinit_pt(ap.pt);ap.pt=0;aic8800_supplicant_os_leave(&ap_ctx);
}
static uint64_t now;
static void sta_reset(const char *pw,unsigned h2e)
{
    memset(&sta,0,sizeof(sta));struct aic8800_supplicant_config c=config(pw,h2e);
    assert(aic8800_supplicant_init(&sta,&c));
}
/* Run the exchange; return the station's final result and count frames. */
static enum aic8800_supplicant_sae_result exchange(unsigned *commits)
{
    uint8_t frame[1024],out[2][1024];size_t bytes,sizes[2];*commits=0;
    enum aic8800_supplicant_sae_result r=aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,
        now,now+5000000,frame,sizeof(frame),&bytes);
    while(r==AIC8800_SUPPLICANT_SAE_SEND){
        if(WPA_GET_LE16(frame+2)==1)++*commits;
        unsigned count=ap_receive(frame,bytes,out,sizes);
        if(!count)return sta.sae_accepted?AIC8800_SUPPLICANT_SAE_ACCEPTED:AIC8800_SUPPLICANT_SAE_WAIT;
        r=AIC8800_SUPPLICANT_SAE_WAIT;
        for(unsigned i=0;i<count;++i){
            now+=1000;
            enum aic8800_supplicant_sae_result next=aic8800_supplicant_sae_receive(&sta,out[i],sizes[i],now,frame,sizeof(frame),&bytes);
            if(next==AIC8800_SUPPLICANT_SAE_FAILED)return next;
            if(next!=AIC8800_SUPPLICANT_SAE_WAIT||r==AIC8800_SUPPLICANT_SAE_WAIT)r=next;
            if(next==AIC8800_SUPPLICANT_SAE_SEND&&i+1<count){
                /* Deliver the station's confirm before the AP's own confirm
                 * reaches the station, as on air. */
                uint8_t keep[1024];size_t keep_bytes=bytes;memcpy(keep,frame,bytes);
                uint8_t o2[2][1024];size_t s2[2];assert(!ap_receive(keep,keep_bytes,o2,s2));
                r=AIC8800_SUPPLICANT_SAE_WAIT;
            }
        }
    }
    return r;
}
static void success(const char *name,unsigned h2e,unsigned expect_group)
{
    unsigned commits;sta_reset("correct horse",h2e);
    enum aic8800_supplicant_sae_result r=exchange(&commits);
    assert(r==AIC8800_SUPPLICANT_SAE_ACCEPTED&&ap.accepted&&sta.sae_accepted);
    assert(((struct sae_data *)sta.sae)->group==(int)expect_group&&ap.sae.group==(int)expect_group);
    assert(!memcmp(sta.config.pmk,ap.sae.pmk,32)&&!memcmp(sta.pmkid,ap.sae.pmkid,16)&&ap.sae.pmk_len==32);
    assert(((struct sae_data *)sta.sae)->h2e==h2e);
    aic8800_supplicant_sae_clear(&sta);assert(!sta.sae&&!sta.sae_pt&&!sta.sae_token&&!sta.config.sae_password_bytes);
    printf("%s: accepted group %u, %u commit(s)\n",name,expect_group,commits);
}
static void refused(const char *name,enum aic8800_supplicant_sae_result r)
{
    assert(r==AIC8800_SUPPLICANT_SAE_FAILED&&sta.state==AIC8800_SUPPLICANT_FAULT&&!sta.sae_accepted);
    /* A faulted owner keeps its state; os entry must stay refused. */
    assert(!aic8800_supplicant_os_enter(&sta));
    printf("%s: refused\n",name);
}
/* Annex J.10 vectors, adapted from the pinned common_module_tests.c. */
static void vectors(void)
{
    static const u8 a1[6]={0x4d,0x3f,0x2f,0xff,0xe3,0x87},a2[6]={0xa5,0xd8,0xaa,0x95,0x8e,0x3c};
    static const char pw[]="mekmitasdigoat";
    static const u8 rand_v[]={0x99,0x24,0x65,0xfd,0x3d,0xaa,0x3c,0x60,0xaa,0x65,0x65,0xb7,0xf6,0x2a,0x2a,0x7f,
        0x2e,0x12,0xdd,0x12,0xf1,0x98,0xfa,0xf4,0xfb,0xed,0x89,0xd7,0xff,0x1a,0xce,0x94};
    static const u8 mask_v[]={0x95,0x07,0xa9,0x0f,0x77,0x7a,0x04,0x4d,0x6a,0x08,0x30,0xb9,0x1e,0xa3,0xd5,0xdd,
        0x70,0xbe,0xce,0x44,0xe1,0xac,0xff,0xb8,0x69,0x83,0xb5,0xe1,0xbf,0x9f,0xb3,0x22};
    static const u8 local_commit[]={0x13,0x00,0x2e,0x2c,0x0f,0x0d,0xb5,0x24,0x40,0xad,0x14,0x6d,0x96,0x71,0x14,0xce,
        0x00,0x5c,0xe1,0xea,0xb0,0xaa,0x2c,0x2e,0x5c,0x28,0x71,0xb7,0x74,0xf6,0xc2,0x57,0x5c,0x65,0xd5,0xad,0x9e,0x00,
        0x82,0x97,0x07,0xaa,0x36,0xba,0x8b,0x85,0x97,0x38,0xfc,0x96,0x1d,0x08,0x24,0x35,0x05,0xf4,0x7c,0x03,0x53,0x76,
        0xd7,0xac,0x4b,0xc8,0xd7,0xb9,0x50,0x83,0xbf,0x43,0x82,0x7d,0x0f,0xc3,0x1e,0xd7,0x78,0xdd,0x36,0x71,0xfd,0x21,
        0xa4,0x6d,0x10,0x91,0xd6,0x4b,0x6f,0x9a,0x1e,0x12,0x72,0x62,0x13,0x25,0xdb,0xe1};
    static const u8 peer_commit[]={0x13,0x00,0x59,0x1b,0x96,0xf3,0x39,0x7f,0xb9,0x45,0x10,0x08,0x48,0xe7,0xb5,0x50,
        0x54,0x3b,0x67,0x20,0xd8,0x83,0x37,0xee,0x93,0xfc,0x49,0xfd,0x6d,0xf7,0xe0,0x8b,0x52,0x23,0xe7,0x1b,0x9b,0xb0,
        0x48,0xd3,0x87,0x3f,0x20,0x55,0x69,0x53,0xa9,0x6c,0x91,0x53,0x6f,0xd8,0xee,0x6c,0xa9,0xb4,0xa6,0x8a,0x14,0x8b,
        0x05,0x6a,0x90,0x9b,0xe0,0x3e,0x83,0xae,0x20,0x8f,0x60,0xf8,0xef,0x55,0x37,0x85,0x80,0x74,0xdb,0x06,0x68,0x70,
        0x32,0x39,0x98,0x62,0x99,0x9b,0x51,0x1e,0x0a,0x15,0x52,0xa5,0xfe,0xa3,0x17,0xc2};
    static const u8 kck[]={0x1e,0x73,0x3f,0x6d,0x9b,0xd5,0x32,0x56,0x28,0x73,0x04,0x33,0x88,0x31,0xb0,0x9a,
        0x39,0x40,0x6d,0x12,0x10,0x17,0x07,0x3a,0x5c,0x30,0xdb,0x36,0xf3,0x6c,0xb8,0x1a};
    static const u8 pmk[]={0x4e,0x4d,0xfa,0xb1,0xa2,0xdd,0x8a,0xc1,0xa9,0x17,0x90,0xf9,0x53,0xfa,0xaa,0x45,
        0x2a,0xe5,0xc6,0x87,0x3a,0xb7,0x5b,0x63,0x60,0x5b,0xa6,0x63,0xf8,0xa7,0xfe,0x59};
    static const u8 pmkid[]={0x87,0x47,0xa6,0x00,0xee,0xa3,0xf9,0xf2,0x24,0x75,0xdf,0x58,0xca,0x1e,0x54,0x98};
    static const u8 x19[]={0xc9,0x30,0x49,0xb9,0xe6,0x40,0x00,0xf8,0x48,0x20,0x16,0x49,0xe9,0x99,0xf2,0xb5,
        0xc2,0x2d,0xea,0x69,0xb5,0x63,0x2c,0x9d,0xf4,0xd6,0x33,0xb8,0xaa,0x1f,0x6c,0x1e};
    static const u8 y19[]={0x73,0x63,0x4e,0x94,0xb5,0x3d,0x82,0xe7,0x38,0x3a,0x8d,0x25,0x81,0x99,0xd9,0xdc,
        0x1a,0x5e,0xe8,0x26,0x9d,0x06,0x03,0x82,0xcc,0xbf,0x33,0xe6,0x14,0xff,0x59,0xa0};
    static const u8 b1[6]={0x00,0x09,0x5b,0x66,0xec,0x1e},b2[6]={0x00,0x0b,0x6b,0xd9,0x02,0x46};
    sta_reset("unused",0);assert(aic8800_supplicant_os_enter(&sta));
    struct sae_data sae;memset(&sae,0,sizeof(sae));struct wpabuf *buf=wpabuf_alloc(1000);
    assert(buf&&!sae_set_group(&sae,19)&&!sae_prepare_commit(a1,a2,(const u8 *)pw,strlen(pw),&sae));
    crypto_bignum_deinit(sae.tmp->sae_rand,1);sae.tmp->sae_rand=crypto_bignum_init_set(rand_v,sizeof(rand_v));
    struct crypto_bignum *mask=crypto_bignum_init_set(mask_v,sizeof(mask_v));
    assert(sae.tmp->sae_rand&&mask&&!crypto_bignum_add(sae.tmp->sae_rand,mask,sae.tmp->own_commit_scalar)&&
        !crypto_bignum_mod(sae.tmp->own_commit_scalar,sae.tmp->order,sae.tmp->own_commit_scalar)&&
        !crypto_ec_point_mul(sae.tmp->ec,sae.tmp->pwe_ecc,mask,sae.tmp->own_commit_element_ecc)&&
        !crypto_ec_point_invert(sae.tmp->ec,sae.tmp->own_commit_element_ecc));
    assert(!sae_write_commit(&sae,buf,0,0,0)&&wpabuf_len(buf)==sizeof(local_commit)&&
        !memcmp(wpabuf_head(buf),local_commit,sizeof(local_commit)));
    assert(!sae_parse_commit(&sae,peer_commit,sizeof(peer_commit),0,0,0,0,0)&&!sae_process_commit(&sae));
    assert(!memcmp(kck,sae.tmp->kck,32)&&!memcmp(pmk,sae.pmk,32)&&!memcmp(pmkid,sae.pmkid,16));
    int pt_groups[]={19,0};
    struct sae_pt *pt=sae_derive_pt(pt_groups,(const u8 *)"byteme",6,(const u8 *)pw,strlen(pw),(const u8 *)"psk4internet",12);
    assert(pt&&pt->group==19);
    struct crypto_ec_point *pwe=sae_derive_pwe_from_pt_ecc(pt,b1,b2);u8 bin[64];
    assert(pwe&&!crypto_ec_point_to_bin(pt->ec,pwe,bin,bin+32)&&!memcmp(bin,x19,32)&&!memcmp(bin+32,y19,32));
    crypto_ec_point_deinit(pwe,1);sae_deinit_pt(pt);crypto_bignum_deinit(mask,1);sae_clear_data(&sae);wpabuf_free(buf);
    aic8800_supplicant_os_leave(&sta);
    printf("vectors: Annex J.10 commit, KCK, PMK, PMKID and group 19 PWE match\n");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,0,_IONBF,0);
    const char *which=argc>1?argv[1]:"all";stream=argc>2?strtoull(argv[2],0,0):0x1234567;
    int all=!strcmp(which,"all");unsigned commits;
    if(!strcmp(which,"repeat")){
        /* With fixed addresses, the same entropy gives the same commit: no
         * time input reaches the random values. */
        sta_reset("correct horse",1);uint8_t frame[1024];size_t bytes;
        assert(aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,1,2,frame,sizeof(frame),&bytes)==AIC8800_SUPPLICANT_SAE_SEND);
        printf("commit ");for(size_t i=0;i<bytes;++i)printf("%02x",frame[i]);printf("\n");return 0;
    }
    if(!strcmp(which,"entropy")){
        /* Run first in a fresh process: the DRBG has no seed yet, so a refused
         * request fails the commit. A later owner can seed it again. */
        uint8_t frame[1024];size_t bytes;sta_reset("correct horse",0);entropy_ok=0;
        refused("entropy refusal",aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,now,now+10,frame,sizeof(frame),&bytes));
        assert(!entropy_bytes);entropy_ok=1;
        ap_reset("correct horse",1);success("after refusal",1,19);ap_clear();assert(entropy_bytes);
    }
    if(all||!strcmp(which,"vectors"))vectors();
    if(all||!strcmp(which,"success")){
        ap_reset("correct horse",0);success("hunting-and-pecking",0,19);ap_clear();
        ap_reset("correct horse",1);success("hash-to-element",1,19);ap_clear();
    }
    if(all||!strcmp(which,"token")){
        ap_reset("correct horse",0);ap.token_first=1;success("anti-clogging token",0,19);assert(ap.tokens==1);ap_clear();
        ap_reset("correct horse",1);ap.token_first=1;success("anti-clogging token container",1,19);assert(ap.tokens==1);ap_clear();
        ap_reset("correct horse",1);ap.token_always=1;sta_reset("correct horse",1);
        refused("repeated token requests",exchange(&commits));assert(commits==5);ap_clear();
    }
    if(all||!strcmp(which,"groups")){
        ap_reset("correct horse",1);ap.refuse=1;success("group 19 refused",1,20);ap_clear();
        ap_reset("correct horse",0);ap.refuse=3;success("groups 19 and 20 refused",0,21);ap_clear();
        ap_reset("correct horse",1);ap.refuse=7;sta_reset("correct horse",1);
        refused("every group refused",exchange(&commits));assert(commits==3);ap_clear();
    }
    if(all||!strcmp(which,"refusal")){
        ap_reset("correct horse",1);ap.corrupt=1;sta_reset("correct horse",1);refused("altered confirm",exchange(&commits));ap_clear();
        ap_reset("wrong horse",0);sta_reset("correct horse",0);refused("wrong password",exchange(&commits));ap_clear();
        /* A reflected commit is silently discarded and leaves the station waiting. */
        ap_reset("correct horse",0);ap.reflect=1;sta_reset("correct horse",0);
        assert(exchange(&commits)==AIC8800_SUPPLICANT_SAE_WAIT&&sta.state==AIC8800_SUPPLICANT_NEW&&
            ((struct sae_data *)sta.sae)->state==SAE_COMMITTED);
        aic8800_supplicant_sae_clear(&sta);ap_clear();printf("reflected commit: discarded\n");
        uint8_t frame[1024],reply[64];size_t bytes;
        sta_reset("correct horse",0);
        assert(aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,now,now+10,frame,sizeof(frame),&bytes)==AIC8800_SUPPLICANT_SAE_SEND);
        size_t n=put(reply,2,0,0);refused("confirm before commit",aic8800_supplicant_sae_receive(&sta,reply,n,now+1,frame,sizeof(frame),&bytes));
        sta_reset("correct horse",0);
        assert(aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,now,now+10,frame,sizeof(frame),&bytes)==AIC8800_SUPPLICANT_SAE_SEND);
        n=put(reply,1,WLAN_STATUS_UNSPECIFIED_FAILURE,0);refused("rejection status",aic8800_supplicant_sae_receive(&sta,reply,n,now+1,frame,sizeof(frame),&bytes));
        sta_reset("correct horse",1);
        assert(aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,now,now+10,frame,sizeof(frame),&bytes)==AIC8800_SUPPLICANT_SAE_SEND);
        n=put(reply,1,WLAN_STATUS_SUCCESS,0);refused("H2E commit with status 0",aic8800_supplicant_sae_receive(&sta,reply,n,now+1,frame,sizeof(frame),&bytes));
        sta_reset("correct horse",0);
        assert(aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,now,now+10,frame,sizeof(frame),&bytes)==AIC8800_SUPPLICANT_SAE_SEND);
        n=put(reply,1,WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ,0);reply[n++]=19;reply[n++]=0;
        refused("deadline",aic8800_supplicant_sae_receive(&sta,reply,n,now+10,frame,sizeof(frame),&bytes));
        /* A PSK configuration cannot start SAE, and claim refuses SAE. */
        memset(&sta,0,sizeof(sta));struct aic8800_supplicant_config c=config("correct horse",0);
        c.key_mgmt=AIC8800_SUPPLICANT_PSK;assert(!aic8800_supplicant_init(&sta,&c));
        c.sae_password_bytes=0;memset(c.sae_password,0,sizeof(c.sae_password));c.ap_rsn[19]=2;
        assert(aic8800_supplicant_init(&sta,&c));
        assert(aic8800_supplicant_sae_start(&sta,sta_addr,ap_addr,ssid,sizeof(ssid)-1,now,now+10,frame,sizeof(frame),&bytes)==AIC8800_SUPPLICANT_SAE_FAILED);
        c=config("correct horse",0);c.pmk[0]=1;memset(&sta,0,sizeof(sta));assert(!aic8800_supplicant_init(&sta,&c));
        printf("configuration: PSK cannot start SAE and SAE refuses a supplied PMK\n");
    }
    size_t peak,held=aic8800_openssl_heap_bytes(&peak);
    printf("OpenSSL heap: %zu bytes held, %zu bytes peak; %llu entropy bytes\n",held,peak,(unsigned long long)entropy_bytes);
    return 0;
}
