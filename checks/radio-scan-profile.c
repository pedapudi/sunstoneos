/* The scan-only radio profile that tools/radio_scan_prepare.py writes, against the native code that consumes it:
 * struct layout, startup validation, the firmware, Wi-Fi and runtime initializations and the platform's pin and rail
 * rules. Usage: radio-scan-profile PROFILE FIRMWARE-DIR (the four images in their catalogue order). */
#include "../apps/board/radio.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OFFSETS(X) X(platform) X(radio_identity) X(transport) X(bootstrap) X(wifi) X(firmware_address) X(firmware_bytes) \
    X(session_owner) X(runtime_lease_us) X(renew_margin_us) X(binding_timeout_us) X(command_timeout_us) X(refill_timeout_us) \
    X(association_timeout_us) X(security_timeout_us) X(platform_authority) X(platform_evidence) X(network_reserved)
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"offsets")){
#define P(f) printf(#f " %zu\n",offsetof(struct board_radio_profile,f));
        OFFSETS(P)
#undef P
        printf("size %zu\n",sizeof(struct board_radio_profile));
        printf("platform.owner %zu\nplatform.topology %zu\nplatform.fabric %zu\nplatform.fabric_hz %zu\nplatform.expected %zu\n"
               "platform.desired %zu\nplatform.card %zu\n",offsetof(struct a733_radio_platform_config,owner),
               offsetof(struct a733_radio_platform_config,topology),offsetof(struct a733_radio_platform_config,fabric),
               offsetof(struct a733_radio_platform_config,fabric_hz),offsetof(struct a733_radio_platform_config,expected),
               offsetof(struct a733_radio_platform_config,desired),offsetof(struct a733_radio_platform_config,card));
        return 0;
    }
    assert(argc==3);
    static struct board_radio_profile f;
    FILE *file=fopen(argv[1],"rb");assert(file&&fread(&f,1,sizeof(f),file)==sizeof(f)&&fgetc(file)==EOF);fclose(file);
    assert(f.version==BOARD_RADIO_PROFILE_SCAN_SUPERVISED_VERSION&&f.bytes==sizeof(f)&&board_radio_profile_network_valid(&f));
    /* The runtime form: what board_radio_profile_runtime makes of the upload (version 7, deadlines set). */
    f.version=BOARD_RADIO_PROFILE_SCAN_VERSION;
    f.platform.handoff=f.ce.handoff=9;f.platform.deadline_us=f.bootstrap_deadline_us=40000000;
    f.platform.lifetime_deadline_us=59990000;f.startup_deadline_us=50000000;
    assert(board_radio_profile_scan_only(&f));
    /* The stage limit rides in the first reserved network byte: valid up to identity, only on scan-only profiles. */
    assert(board_radio_profile_stage_limit(&f)<=BOARD_RADIO_STAGE_ASSOCIATE);
    {
        struct board_radio_profile g=f;
        g.network_reserved[0]=BOARD_RADIO_STAGE_ASSOCIATE+1;assert(!board_radio_profile_network_valid(&g));
        /* The association stage declares its entropy unqualified; every other stage declares nothing. */
        g=f;g.network_reserved[0]=BOARD_RADIO_STAGE_ASSOCIATE;g.network_reserved[1]=0;assert(!board_radio_profile_network_valid(&g));
        g.network_reserved[1]=1;assert(board_radio_profile_network_valid(&g));
        g.network_reserved[0]=BOARD_RADIO_STAGE_FIRMWARE;assert(!board_radio_profile_network_valid(&g));
        g=f;g.network_reserved[0]=BOARD_RADIO_STAGE_ENUMERATE;assert(board_radio_profile_network_valid(&g));
        g.version=BOARD_RADIO_PROFILE_VERSION;assert(!board_radio_profile_network_valid(&g));
    }
    a733_clock_mask_t clocks;a733_rail_mask_t rails;
    assert(a733_radio_platform_scopes(&f.platform,&clocks)&&a733_radio_platform_rails(&f.platform,&rails));
    for(unsigned i=0;i<8;++i){
        unsigned bank=i<6?A733_PIN_G:A733_PIN_M,pin=i<6?i:i-6;
        assert((f.platform.expected[i].function==0||f.platform.expected[i].function==15)&&
            f.platform.desired[i].function==(i<6?2:i==6?0:1)&&
            a733_pin_config_valid_banked(bank,pin,f.platform.expected[i])&&a733_pin_config_valid_banked(bank,pin,f.platform.desired[i]));
    }
    assert(f.platform.card.supported_ocr==0x00300000&&f.platform.card.command_timeout&&f.platform.card.ready_poll_interval);
    /* The firmware images, in their roles, against the identity the profile expects. */
    static const char *names[4]={"fw_adid_8800d80_u02.bin","fw_patch_8800d80_u02.bin","fw_patch_table_8800d80_u02.bin","fmacfw_8800d80_u02.bin"};
    static struct aic8800_firmware_config fw;static struct aic8800_firmware firmware;
    fw.identity=f.radio_identity;fw.profile=1;
    for(unsigned i=0;i<4;++i){
        char path[512];snprintf(path,sizeof(path),"%s/%s",argv[2],names[i]);
        uint8_t *data=malloc((size_t)f.firmware_bytes[i]);file=fopen(path,"rb");
        assert(data&&file&&fread(data,1,(size_t)f.firmware_bytes[i],file)==(size_t)f.firmware_bytes[i]&&fgetc(file)==EOF);fclose(file);
        fw.blobs[i]=(struct aic8800_firmware_blob){data,(size_t)f.firmware_bytes[i]};
        assert(f.firmware_address[i]>=0x43010000&&f.firmware_address[i]+f.firmware_bytes[i]<=0x43100000);
    }
    assert(aic8800_firmware_init(&firmware,&fw));
    static struct aic8800_wifi wifi;static struct aic8800_runtime runtime;
    assert(aic8800_wifi_init(&wifi,&f.wifi));
    struct aic8800_runtime_config runtime_config={.binding_timeout=f.binding_timeout_us};
    assert(!aic8800_runtime_init(&runtime,&runtime_config)); /* The platform-id reader is the session's to supply. */
    runtime_config.read_platform_id=(int (*)(void *,uint8_t *))(uintptr_t)1;
    assert(aic8800_runtime_init(&runtime,&runtime_config));
    assert(f.runtime_lease_us>f.renew_margin_us&&f.session_owner&&f.association_timeout_us);
    printf("PASS radio scan profile: native layout, startup form, firmware, Wi-Fi and runtime initializations, pins and rails\n");
    return 0;
}
