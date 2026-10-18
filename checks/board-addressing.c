/* Reuse explicit lower-provider stubs; do not replay coordinator cases. */
#define main board_radio_legacy_cases
#include "board-radio.c"
#undef main
#include "../apps/board/radio_profile.c"

static void seal(struct board_radio_profile *p,struct a733_board_session *s)
{storage_digest_bytes(p,sizeof(*p),s->radio_profile_digest);}

int main(void)
{
    struct board_radio_profile raw={.magic=BOARD_RADIO_PROFILE_MAGIC,
        .version=BOARD_RADIO_PROFILE_SUPERVISED_VERSION,.bytes=sizeof(raw),
        .platform_authority=1,.ce_authority=2,.secure_ce_authority=3,.entropy_record=4,
        .characterization_record=5,.startup_test_record=6,.health_enable_epoch=7,
        .platform_evidence={1},.ce_evidence={2},.entropy_evidence={3},
        .ce={.profile=A733_CE_TRNG_ONLY},.peer_port=80,.request_bytes=4,.response_bytes=7,
        .client_timeout_ticks=3,.firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},
        .firmware_bytes={1708,24744,23288,341260}};
    memcpy(raw.chip_id,id.words,sizeof(raw.chip_id));
    struct a733_board_session s={.generation=2,.session_start_ticks=24,.deadline_us=60000001};
    struct board_radio_profile out;uint8_t digest[32],expected[32];
    for(unsigned policy=0;policy<2;++policy){
        raw.network_dhcp=policy;raw.address_timeout_ticks=policy?UINT32_MAX:0;
        raw.lease_cap_ticks=policy?UINT32_MAX:0;raw.network_dns[0]=9;raw.network_dns[3]=9;
        seal(&raw,&s);assert(board_radio_profile_runtime(&out,&raw,&s,digest));
        assert(out.version==3&&out.network_dhcp==policy&&out.response_bytes==7);
        assert(out.address_timeout_ticks==raw.address_timeout_ticks&&out.lease_cap_ticks==raw.lease_cap_ticks);
        assert(!memcmp(out.network_dns,raw.network_dns,4));
        storage_digest_bytes(&out,sizeof(out),expected);assert(!memcmp(digest,expected,32));
        assert(startup_profile_valid(&out,digest,&id));
        out.version=1;storage_digest_bytes(&out,sizeof(out),expected);
        assert(!startup_profile_valid(&out,expected,&id));
    }
    for(unsigned mutation=0;mutation<9;++mutation){
        struct board_radio_profile bad=raw;
        switch(mutation){
        case 0:bad.network_dhcp=2;break;
        case 1:bad.network_reserved[0]=1;break;
        case 2:bad.network_reserved[1]=1;break;
        case 3:bad.network_reserved[2]=1;break;
        case 4:bad.address_timeout_ticks=0;break;
        case 5:bad.lease_cap_ticks=0;break;
        case 6:bad.network_dhcp=0;break;
        case 7:bad.version=2;break;
        default:bad.bytes=1856;break;
        }
        seal(&bad,&s);memset(&out,0xcc,sizeof(out));memset(digest,0xdd,sizeof(digest));
        assert(!board_radio_profile_runtime(&out,&bad,&s,digest));
        const uint8_t *bytes=(const uint8_t *)&out;
        for(size_t i=0;i<sizeof(out);++i)assert(bytes[i]==0xcc);
        for(size_t i=0;i<sizeof(digest);++i)assert(digest[i]==0xdd);
    }
    seal(&raw,&s);s.radio_profile_digest[0]^=1;
    assert(!board_radio_profile_runtime(&out,&raw,&s,digest));
    puts("PASS board addressing: authenticated fixed/DHCP profile admission and runtime geometry");
    return 0;
}
