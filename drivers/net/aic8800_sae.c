/* Station SAE authentication with the pinned wpa_supplicant implementation.
 * Commit, confirm, anti-clogging and group rejection follow its station SME;
 * the elliptic-curve arithmetic comes from the pinned OpenSSL library, which
 * the supplicant OS entry sets up. */
#include "aic8800_supplicant_port.h"
#include "aic8800_supplicant.h"
#include "utils/common.h"
#include "utils/wpabuf.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"
#include "common/sae.h"
/* Groups 19, 20 and 21 in the source's default order; bit i of sae_rejected
 * marks groups[i] as refused by this AP. A commit may be resent at most
 * SAE_ROUNDS times for anti-clogging tokens and group changes together. */
static int groups[]={19,20,21,0};
#define SAE_ROUNDS 4u
static int enabled(const struct aic8800_supplicant *s,int group)
{
    for(unsigned i=0;groups[i];++i)if(groups[i]==group)return !(s->sae_rejected&(1u<<i));
    return 0;
}
static void sae_fail(struct aic8800_supplicant *s){aic8800_supplicant_os_leave(s);aic8800_supplicant_stop(s);}
/* Copy an Authentication frame body: algorithm, sequence, status, then fields. */
static int emit(const struct wpabuf *b,unsigned seq,unsigned status,uint8_t *frame,size_t capacity,size_t *bytes)
{
    if(!b||capacity<6||wpabuf_len(b)>capacity-6)return 0;
    WPA_PUT_LE16(frame,WLAN_AUTH_SAE);WPA_PUT_LE16(frame+2,(u16)seq);WPA_PUT_LE16(frame+4,(u16)status);
    os_memcpy(frame+6,wpabuf_head(b),wpabuf_len(b));*bytes=6+wpabuf_len(b);return 1;
}
static int commit(struct aic8800_supplicant *s,uint8_t *frame,size_t capacity,size_t *bytes)
{
    struct sae_data *sae=s->sae;
    if(s->sae_rounds++>SAE_ROUNDS)return 0;
    if(!sae->tmp){
        int rejected[4]={0},n=0;
        for(unsigned i=0;groups[i];++i)if(s->sae_rejected&(1u<<i))rejected[n++]=groups[i];
        for(s->sae_group=0;groups[s->sae_group]&&!enabled(s,groups[s->sae_group]);++s->sae_group){}
        if(!groups[s->sae_group]||sae_set_group(sae,groups[s->sae_group]))return 0;
        sae->akmp=WPA_KEY_MGMT_SAE;
        if(s->sae_pt?sae_prepare_commit_pt(sae,s->sae_pt,s->sae_own,s->sae_bssid,n?rejected:0,0):
            sae_prepare_commit(s->sae_own,s->sae_bssid,s->config.sae_password,s->config.sae_password_bytes,sae))return 0;
    }
    struct wpabuf *b=wpabuf_alloc(SAE_COMMIT_MAX_LEN+(s->sae_token?3+wpabuf_len(s->sae_token):0));
    int ok=b&&!sae_write_commit(sae,b,s->sae_token,0,0)&&
        emit(b,1,sae->h2e?WLAN_STATUS_SAE_HASH_TO_ELEMENT:WLAN_STATUS_SUCCESS,frame,capacity,bytes);
    wpabuf_free(b);if(ok)sae->state=SAE_COMMITTED;return ok;
}
enum aic8800_supplicant_sae_result aic8800_supplicant_sae_start(struct aic8800_supplicant *s,
    const uint8_t own[6],const uint8_t bssid[6],const uint8_t *ssid,size_t ssid_bytes,
    uint64_t now,uint64_t deadline,uint8_t *frame,size_t capacity,size_t *bytes)
{
    /* Clear wipes the password, so a cleared supplicant cannot start again. */
    if(!s||!s->initialized||s->state!=AIC8800_SUPPLICANT_NEW||s->config.key_mgmt!=AIC8800_SUPPLICANT_SAE||
        !s->config.sae_password_bytes||s->config.sae_password_bytes>sizeof(s->config.sae_password)||
        s->sae||s->sae_accepted||!own||!bssid||!ssid||!ssid_bytes||ssid_bytes>32||!frame||!bytes||
        deadline<=now||!aic8800_supplicant_os_enter(s))return AIC8800_SUPPLICANT_SAE_FAILED;
    s->last_now=now;s->sae_deadline=deadline;os_memcpy(s->sae_own,own,6);os_memcpy(s->sae_bssid,bssid,6);
    if(!(s->sae=os_zalloc(sizeof(struct sae_data)))){sae_fail(s);return AIC8800_SUPPLICANT_SAE_FAILED;}
    /* Hash-to-element when the AP advertises it; its PT covers every group. */
    if((s->config.ap_rsnx&BIT(WLAN_RSNX_CAPAB_SAE_H2E))&&
        !(s->sae_pt=sae_derive_pt(groups,ssid,ssid_bytes,s->config.sae_password,s->config.sae_password_bytes,0,0))){
        sae_fail(s);return AIC8800_SUPPLICANT_SAE_FAILED;}
    if(!commit(s,frame,capacity,bytes)){sae_fail(s);return AIC8800_SUPPLICANT_SAE_FAILED;}
    aic8800_supplicant_os_leave(s);return AIC8800_SUPPLICANT_SAE_SEND;
}
/* A token request carries the group, then the token; with H2E the token sits
 * in an Anti-Clogging Token Container element. */
static int token(struct aic8800_supplicant *s,const uint8_t *p,size_t n)
{
    struct sae_data *sae=s->sae;
    if(n<2||WPA_GET_LE16(p)!=sae->group)return 0;
    p+=2;n-=2;
    if(sae->h2e){
        if(n<3||p[0]!=WLAN_EID_EXTENSION||!p[1]||p[1]>n-2||p[2]!=WLAN_EID_EXT_ANTI_CLOGGING_TOKEN)return 0;
        n=p[1]-1u;p+=3;
    }
    wpabuf_free(s->sae_token);s->sae_token=wpabuf_alloc_copy(p,n);return s->sae_token!=0;
}
/* The AP's Rejected Groups element must not name a group this station enables. */
static int downgraded(const struct aic8800_supplicant *s)
{
    const struct sae_data *sae=s->sae;const struct wpabuf *r=sae->tmp?sae->tmp->peer_rejected_groups:0;
    if(!r)return 0;
    if(wpabuf_len(r)&1)return 1;
    for(size_t i=0;i<wpabuf_len(r);i+=2)if(enabled(s,WPA_GET_LE16(wpabuf_head_u8(r)+i)))return 1;
    return 0;
}
enum aic8800_supplicant_sae_result aic8800_supplicant_sae_receive(struct aic8800_supplicant *s,
    const uint8_t *body,size_t body_bytes,uint64_t now,uint8_t *frame,size_t capacity,size_t *bytes)
{
    if(!s||!s->sae||s->state!=AIC8800_SUPPLICANT_NEW||!body||!frame||!bytes)return AIC8800_SUPPLICANT_SAE_FAILED;
    if(s->sae_accepted)return AIC8800_SUPPLICANT_SAE_WAIT;
    if(now<s->last_now||now>=s->sae_deadline||body_bytes<6||WPA_GET_LE16(body)!=WLAN_AUTH_SAE||
        !aic8800_supplicant_os_enter(s)){aic8800_supplicant_stop(s);return AIC8800_SUPPLICANT_SAE_FAILED;}
    s->last_now=now;
    struct sae_data *sae=s->sae;
    unsigned seq=WPA_GET_LE16(body+2),status=WPA_GET_LE16(body+4);
    const uint8_t *p=body+6;size_t n=body_bytes-6;
    if(seq==1&&sae->state==SAE_COMMITTED&&status==WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ){
        /* Reuse the current commit values with the requested token. */
        if(!token(s,p,n)||!commit(s,frame,capacity,bytes)){sae_fail(s);return AIC8800_SUPPLICANT_SAE_FAILED;}
        aic8800_supplicant_os_leave(s);return AIC8800_SUPPLICANT_SAE_SEND;
    }
    if(seq==1&&sae->state==SAE_COMMITTED&&status==WLAN_STATUS_FINITE_CYCLIC_GROUP_NOT_SUPPORTED){
        s->sae_rejected|=(uint8_t)(1u<<s->sae_group);sae_clear_temp_data(sae);
        wpabuf_free(s->sae_token);s->sae_token=0;
        if(!commit(s,frame,capacity,bytes)){sae_fail(s);return AIC8800_SUPPLICANT_SAE_FAILED;}
        aic8800_supplicant_os_leave(s);return AIC8800_SUPPLICANT_SAE_SEND;
    }
    if(seq==1&&sae->state==SAE_CONFIRMED&&(status==WLAN_STATUS_SUCCESS||status==WLAN_STATUS_SAE_HASH_TO_ELEMENT)){
        /* As in the source SME, a repeated AP commit is ignored after confirm. */
        aic8800_supplicant_os_leave(s);return AIC8800_SUPPLICANT_SAE_WAIT;
    }
    if(seq==1&&sae->state==SAE_COMMITTED&&status==(sae->h2e?WLAN_STATUS_SAE_HASH_TO_ELEMENT:WLAN_STATUS_SUCCESS)){
        u16 r=sae_parse_commit(sae,p,n,0,0,groups,sae->h2e,0);
        if(r==SAE_SILENTLY_DISCARD){aic8800_supplicant_os_leave(s);return AIC8800_SUPPLICANT_SAE_WAIT;}
        struct wpabuf *b=0;
        int ok=r==WLAN_STATUS_SUCCESS&&!downgraded(s)&&!sae_process_commit(sae)&&
            (b=wpabuf_alloc(SAE_CONFIRM_MAX_LEN))&&!sae_write_confirm(sae,b)&&emit(b,2,WLAN_STATUS_SUCCESS,frame,capacity,bytes);
        wpabuf_free(b);
        if(!ok){sae_fail(s);return AIC8800_SUPPLICANT_SAE_FAILED;}
        wpabuf_free(s->sae_token);s->sae_token=0;sae->state=SAE_CONFIRMED;
        aic8800_supplicant_os_leave(s);return AIC8800_SUPPLICANT_SAE_SEND;
    }
    if(seq==2&&sae->state==SAE_CONFIRMED&&status==WLAN_STATUS_SUCCESS&&!sae_check_confirm(sae,p,n,0)&&
        sae->pmk_len==sizeof(s->config.pmk)){
        sae->state=SAE_ACCEPTED;os_memcpy(s->config.pmk,sae->pmk,sizeof(s->config.pmk));
        os_memcpy(s->pmkid,sae->pmkid,sizeof(s->pmkid));sae_clear_temp_data(sae);
        s->sae_accepted=1;aic8800_supplicant_os_leave(s);return AIC8800_SUPPLICANT_SAE_ACCEPTED;
    }
    /* Any other status, sequence or state is a refusal. */
    sae_fail(s);return AIC8800_SUPPLICANT_SAE_FAILED;
}
void aic8800_supplicant_sae_clear(struct aic8800_supplicant *s)
{
    if(!s||!s->initialized||!aic8800_supplicant_os_enter(s))return;
    if(s->sae){sae_clear_data(s->sae);os_free(s->sae);s->sae=0;}
    sae_deinit_pt(s->sae_pt);s->sae_pt=0;wpabuf_free(s->sae_token);s->sae_token=0;
    aic8800_security_zero(s->config.sae_password,sizeof(s->config.sae_password));s->config.sae_password_bytes=0;
    aic8800_supplicant_os_leave(s);
}
