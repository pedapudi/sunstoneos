/* The radio record's RSN classifier against the pinned wpa_supplicant parser and the supplicant's own cipher
 * conditions: for each element, the classifier passes exactly when the PSK candidate test would, and otherwise names
 * the condition that fails. Compiled with the supplicant's port header, as the image compiles the parser. */
#include "utils/common.h"
#include "common/wpa_common.h"
#include "aic8800_supplicant_rsn.h"
#include "a733_radio_result.h"
#include <assert.h>
#include <stdio.h>
void *os_memset(void *d,int v,size_t n){unsigned char *p=d;while(n--)*p++=(unsigned char)v;return d;}
void *os_memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
int os_memcmp(const void *a,const void *b,size_t n)
{const unsigned char *x=a,*y=b;for(;n;--n,++x,++y)if(*x!=*y)return *x-*y;return 0;}
static const uint8_t psk_ccmp[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,2,0,0};
static const uint8_t mixed[]={48,24,1,0,0x00,0x0f,0xac,2,2,0,0x00,0x0f,0xac,4,0x00,0x0f,0xac,2,1,0,0x00,0x0f,0xac,2,0,0};
static const uint8_t tkip_pairwise[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,2,1,0,0x00,0x0f,0xac,2,0,0};
static const uint8_t transition_capable[]={48,24,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,2,0,0x00,0x0f,0xac,2,0x00,0x0f,0xac,8,0x80,0};
static const uint8_t transition_required[]={48,24,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,2,0,0x00,0x0f,0xac,2,0x00,0x0f,0xac,8,0xc0,0};
static const uint8_t sae_only[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,8,0x80,0};
static const uint8_t sae_required[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,8,0xc0,0};
static const uint8_t psk_sha256[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,6,0,0};
static const uint8_t version_only[]={48,2,1,0};
static const uint8_t short_count[]={48,10,1,0,0x00,0x0f,0xac,4,2,0,0x00,0x0f,0xac,4};
static const uint8_t group_gcmp[]={48,20,1,0,0x00,0x0f,0xac,8,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,2,0,0};
static const uint8_t pmf_mgmt_group[]={48,26,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,2,0x80,0,0,0,
    0x00,0x0f,0xac,6};
static const struct { const char *name; const uint8_t *e; size_t n; unsigned check; int parse; } cases[]={
    {"WPA2-PSK with CCMP",psk_ccmp,sizeof(psk_ccmp),A733_RADIO_RSN_PASSED,0},
    /* CONFIG_NO_TKIP: the upstream parser refuses a TKIP group cipher outright. */
    {"WPA/WPA2 mixed, TKIP group",mixed,sizeof(mixed),A733_RADIO_RSN_GROUP_NOT_CCMP,-1},
    {"TKIP pairwise only",tkip_pairwise,sizeof(tkip_pairwise),A733_RADIO_RSN_PAIRWISE_NO_CCMP,0},
    {"WPA2/WPA3 transition, MFP capable",transition_capable,sizeof(transition_capable),A733_RADIO_RSN_PASSED,0},
    {"WPA2/WPA3 transition, MFP required",transition_required,sizeof(transition_required),A733_RADIO_RSN_MFP_REQUIRED,0},
    {"WPA3 SAE only, MFP capable",sae_only,sizeof(sae_only),A733_RADIO_RSN_NO_PSK,0},
    {"WPA3 SAE only, MFP required",sae_required,sizeof(sae_required),A733_RADIO_RSN_MFP_REQUIRED,0},
    {"PSK-SHA256 only",psk_sha256,sizeof(psk_sha256),A733_RADIO_RSN_NO_PSK,0},
    {"version only (CCMP and 802.1X by default)",version_only,sizeof(version_only),A733_RADIO_RSN_NO_PSK,0},
    {"pairwise count past the end",short_count,sizeof(short_count),A733_RADIO_RSN_MALFORMED,-2},
    {"GCMP group",group_gcmp,sizeof(group_gcmp),A733_RADIO_RSN_GROUP_NOT_CCMP,0},
    {"PSK with BIP-CMAC management group",pmf_mgmt_group,sizeof(pmf_mgmt_group),A733_RADIO_RSN_PASSED,0},
};
int main(void)
{
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        struct wpa_ie_data ie;
        int parse=wpa_parse_wpa_ie_rsn(cases[i].e,cases[i].n,&ie);
        int accepted=!parse&&aic8800_rsn_ciphers_accepted(&ie)&&(ie.key_mgmt&WPA_KEY_MGMT_PSK);
        struct a733_radio_rsn s;a733_radio_rsn_summarize(&s,cases[i].e,cases[i].n);
        enum a733_radio_rsn_check check=a733_radio_rsn_classify(&s);
        printf("%-45s parse %3d accepted %d check %u\n",cases[i].name,parse,accepted,(unsigned)check);fflush(stdout);
        assert(parse==cases[i].parse&&check==cases[i].check);
        /* The classifier passes exactly when the supplicant's test accepts. */
        assert((check==A733_RADIO_RSN_PASSED)==accepted);
    }
    struct a733_radio_rsn none;a733_radio_rsn_summarize(&none,NULL,0);
    assert(a733_radio_rsn_classify(&none)==A733_RADIO_RSN_NO_ELEMENT&&none.group==A733_RADIO_RSN_SUITE_ABSENT);
    puts("PASS radio RSN summary: the record's classifier agrees with the pinned parser and the PSK candidate test");
    return 0;
}
