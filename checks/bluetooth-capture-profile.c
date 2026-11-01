/* Reuse the accepted Bluetooth provider profile; execute only admission. */
#define main bluetooth_pm_profile_reference_main
#include "bluetooth-pm-profile.c"
#undef main
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
#include "recognition-models.h"
#endif
static unsigned digest_calls;
static size_t digest_bytes;
void __real_storage_digest_bytes(const void *,size_t,uint8_t[32]);
void __wrap_storage_digest_bytes(const void *data,size_t bytes,uint8_t hash[32])
{++digest_calls;digest_bytes+=bytes;__real_storage_digest_bytes(data,bytes,hash);}
static int admit(struct bluetooth_profile *p)
{
    uint8_t digest[32];__real_storage_digest_bytes(p,p->bytes,digest);
    digest_calls=0;digest_bytes=0;
    return bluetooth_profile_valid(p,digest);
}
#define BAD(field,value) do { struct bluetooth_profile bad=profile;bad.field=(value);assert(!admit(&bad));++refused; } while(0)
int main(void)
{
    assert(admit(&profile)&&digest_calls==1&&digest_bytes==624);
    profile.version=3;profile.bytes=sizeof(profile);
    profile.headset=(struct bluetooth_headset_profile){.mode=2,.incoming=1,.peer={1,2,3,4,5,6},
        .minimum_security=2,.server_channel=7,.scheduling_authority=17,.scheduling_evidence={1},
        .receive_latency_us=1000,.response_timeout_us=2000,.samples=24000,.mtu=128,.accept_slots=0x1fa0,
        .page_interval=0x800,.page_window=0x12,.line_timeout_us=1000000,.capture_timeout_us=5000000};
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
    for(unsigned i=0;i<2;++i){profile.recognition.bytes[i]=bluetooth_model_bytes[i];
        memcpy(profile.recognition.sha256[i],bluetooth_model_sha256[i],32);}
#endif
#if defined(TEST_CAPTURE_REFUSED) || (!defined(SUNSTONE_BLUETOOTH_CAPTURE_PROFILE) && !defined(SUNSTONE_BLUETOOTH_RECOGNITION))
    assert(!admit(&profile)&&digest_calls==0);
    puts("{\"version3_admitted\":false,\"digest_calls\":0}");return 0;
#else
    unsigned refused=0;
    assert(admit(&profile)&&digest_calls==1&&digest_bytes==sizeof(profile));
    struct bluetooth_profile changed=profile;
    changed.recognition.bytes[0]^=1;changed.recognition.sha256[1][0]^=1;
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
    assert(!admit(&changed));
#else
    assert(admit(&changed)&&digest_calls==1&&digest_bytes==sizeof(profile));
#endif
    uint8_t digest[32];__real_storage_digest_bytes(&profile,sizeof(profile),digest);
    assert(!bluetooth_profile_valid(&changed,digest));
    digest[0]^=1;assert(!bluetooth_profile_valid(&profile,digest));
    assert(!bluetooth_profile_valid(NULL,digest)&&!bluetooth_profile_valid(&profile,NULL));
    BAD(version,4);BAD(bytes,752);BAD(topology,0);BAD(sid_authority,0);
    BAD(radio.topology.identity,3);BAD(radio.handoff,1);BAD(uart.handoff,1);BAD(uart.format.baud,115200);
    BAD(headset.mode,0);BAD(headset.mode,4);BAD(headset.mode,6);BAD(headset.incoming,2);
    BAD(headset.minimum_security,1);BAD(headset.minimum_security,4);BAD(headset.server_channel,0);BAD(headset.server_channel,31);
    BAD(headset.pin[16],1);BAD(headset.allow_just_works,2);BAD(headset.scheduling_authority,0);
    BAD(headset.receive_latency_us,0);BAD(headset.receive_latency_us,1000000);
    BAD(headset.response_timeout_us,0);BAD(headset.response_timeout_us,1000000);
    BAD(headset.samples,0);BAD(headset.samples,24001);BAD(headset.mtu,47);BAD(headset.mtu,513);
    BAD(headset.accept_slots,0x9f);BAD(headset.page_interval,0x11);BAD(headset.page_interval,0x1001);
    BAD(headset.page_window,0x10);BAD(headset.page_window,0x801);BAD(headset.line_timeout_us,0);
    BAD(headset.capture_timeout_us,0);BAD(headset.capture_timeout_us,5000001);
    BAD(headset.peer_page_scan_mode,3);BAD(headset.peer_role_switch,2);BAD(headset.reserved2,1);
    BAD(firmware_address[1],profile.firmware_address[0]);BAD(firmware_address[0],0x43000000);BAD(firmware_bytes[0],0);
    changed=profile;memset(changed.headset.peer,0,sizeof(changed.headset.peer));assert(!admit(&changed));++refused;
    changed=profile;memset(changed.headset.scheduling_evidence,0,32);assert(!admit(&changed));++refused;
    changed=profile;changed.headset.mode=3;
#ifdef SUNSTONE_BLUETOOTH_MSBC
    assert(admit(&changed));changed.headset.samples=119;assert(!admit(&changed));++refused;
    changed=profile;changed.headset.mode=5;assert(admit(&changed));
#else
    assert(!admit(&changed));++refused;changed.headset.mode=5;assert(!admit(&changed));++refused;
#endif
    assert(admit(&profile)&&digest_calls==1&&digest_bytes==sizeof(profile));
    printf("{\"version3_admitted\":true,\"digest_calls\":%u,\"digest_bytes\":%zu,\"structural_refusals\":%u}\n",digest_calls,digest_bytes,refused);
    return 0;
#endif
}
