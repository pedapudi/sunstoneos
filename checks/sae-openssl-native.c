/* Station SAE on the native AArch64 OpenSSL build inside the network model's
 * FreeRTOS task, with the FreeRTOS heap behind OpenSSL. A second sae.c
 * instance answers as the access point, as in checks/sae-openssl.c. The
 * entropy is a synthetic model input, not production entropy. */
#include "aic8800_supplicant_port.h"
#include "aic8800_supplicant.h"
#include "aic8800_openssl.h"
#include "platform.h"
#include "utils/common.h"
#include "utils/wpabuf.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"
#include "common/sae.h"
static struct aic8800_supplicant station,access_point;
static uint64_t stream=0x9e3779b97f4a7c15u;
static int entropy(void *context,uint8_t *out,size_t n)
{
    (void)context;
    for(size_t i=0;i<n;++i){stream^=stream<<13;stream^=stream>>7;stream^=stream<<17;out[i]=(uint8_t)(stream>>24);}
    return 1;
}
static const uint8_t sta[6]={2,1,2,3,4,5},ap[6]={2,17,34,51,68,85},ssid[]="sunstone";
static const char password[]="correct horse";
static int init(struct aic8800_supplicant *s,unsigned h2e)
{
    static const uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,8,128,0};
    struct aic8800_supplicant_config c={.entropy=entropy,.entropy_context=&stream,.entropy_record=1,.beacon_record=2,
        .ticks_per_second=1000,.command_timeout=1000,.ap_rsn_bytes=sizeof(rsn),.key_mgmt=AIC8800_SUPPLICANT_SAE,
        .ap_rsnx=h2e?(uint8_t)BIT(WLAN_RSNX_CAPAB_SAE_H2E):0,.sae_password_bytes=sizeof(password)-1};
    os_memcpy(c.ap_rsn,rsn,sizeof(rsn));os_memcpy(c.sae_password,password,sizeof(password)-1);
    os_memset(s,0,sizeof(*s));return aic8800_supplicant_init(s,&c);
}
static size_t frame(uint8_t *f,unsigned seq,unsigned status,const struct wpabuf *b)
{
    WPA_PUT_LE16(f,WLAN_AUTH_SAE);WPA_PUT_LE16(f+2,(u16)seq);WPA_PUT_LE16(f+4,(u16)status);
    os_memcpy(f+6,wpabuf_head(b),wpabuf_len(b));return 6+wpabuf_len(b);
}
/* One exchange; returns 1 when both sides accept with equal PMK and PMKID. */
static int exchange(unsigned h2e)
{
    static uint8_t out[1024],commit[1024],confirm[1024];size_t bytes,commit_bytes=0,confirm_bytes=0;
    if(!init(&station,h2e)||!init(&access_point,h2e)||
        aic8800_supplicant_sae_start(&station,sta,ap,ssid,sizeof(ssid)-1,1,100000,out,sizeof(out),&bytes)!=AIC8800_SUPPLICANT_SAE_SEND)return 0;
    struct sae_data sae;os_memset(&sae,0,sizeof(sae));struct sae_pt *pt=0;int groups[]={19,20,21,0},ok=0;
    if(!aic8800_supplicant_os_enter(&access_point))return 0;
    struct wpabuf *b=wpabuf_alloc(SAE_COMMIT_MAX_LEN),*c=wpabuf_alloc(SAE_CONFIRM_MAX_LEN);
    if(b&&c&&WPA_GET_LE16(out+2)==1&&sae_parse_commit(&sae,out+6,bytes-6,0,0,groups,h2e,0)==WLAN_STATUS_SUCCESS&&
        (h2e?(pt=sae_derive_pt(groups,ssid,sizeof(ssid)-1,(const u8 *)password,sizeof(password)-1,0,0))&&
            !sae_prepare_commit_pt(&sae,pt,ap,sta,0,0):
            !sae_prepare_commit(ap,sta,(const u8 *)password,sizeof(password)-1,&sae))&&
        !sae_process_commit(&sae)&&!sae_write_commit(&sae,b,0,0,0)&&!sae_write_confirm(&sae,c)){
        commit_bytes=frame(commit,1,h2e?WLAN_STATUS_SAE_HASH_TO_ELEMENT:WLAN_STATUS_SUCCESS,b);
        confirm_bytes=frame(confirm,2,WLAN_STATUS_SUCCESS,c);ok=1;
    }
    aic8800_supplicant_os_leave(&access_point);
    ok=ok&&aic8800_supplicant_sae_receive(&station,commit,commit_bytes,2,out,sizeof(out),&bytes)==AIC8800_SUPPLICANT_SAE_SEND&&
        aic8800_supplicant_os_enter(&access_point);
    if(ok){ok=WPA_GET_LE16(out+2)==2&&!sae_check_confirm(&sae,out+6,bytes-6,0);aic8800_supplicant_os_leave(&access_point);}
    ok=ok&&aic8800_supplicant_sae_receive(&station,confirm,confirm_bytes,3,out,sizeof(out),&bytes)==AIC8800_SUPPLICANT_SAE_ACCEPTED&&
        !os_memcmp(station.config.pmk,sae.pmk,32)&&!os_memcmp(station.pmkid,sae.pmkid,16);
    if(aic8800_supplicant_os_enter(&access_point)){
        wpabuf_free(b);wpabuf_free(c);sae_clear_data(&sae);sae_deinit_pt(pt);aic8800_supplicant_os_leave(&access_point);}
    aic8800_supplicant_sae_clear(&station);
    return ok&&!station.sae;
}
int sae_native_case(void)
{
    if(!exchange(0)||!exchange(1))return 0;
    size_t peak,held=aic8800_openssl_heap_bytes(&peak);
    platform_puts("PASS SAE: hunting-and-pecking and hash-to-element group 19 accepted; OpenSSL heap held/peak ");
    platform_hex(held);platform_hex(peak);platform_puts("\n");
    return 1;
}
