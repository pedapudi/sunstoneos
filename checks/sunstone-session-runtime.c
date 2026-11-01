/* Exercise the typed parser with the native board and service validators.
 * session-fixture.h freezes bytes from the nominal OS service producer. */
#include "os.h"
#include "application.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "session-fixture.h"
#ifndef TEST_RUN_US
#define TEST_RUN_US 2000000
#endif
#ifndef TEST_RESERVE_US
#define TEST_RESERVE_US 5000000
#endif
void __assert_func(const char *file,int line,const char *function,const char *expression)
{(void)file;(void)line;(void)function;(void)expression;__builtin_trap();}
#ifndef NO_APPLICATION
__attribute__((section("sunstone_manifest"),aligned(8)))
static const struct ss_manifest manifest={.magic=SS_MANIFEST_MAGIC,.version=SS_MANIFEST_VERSION,.bytes=sizeof(struct ss_manifest),
    .name="parser",.devices_optional=SS_DEVICE_USB_STORAGE|SS_DEVICE_USB_KEYBOARD,
    .region_count=1,.regions={{.address=0x44000000,.bytes=4096,.kind=SS_REGION_INPUT,.access=SS_ACCESS_READ}},
    .return_reserve_us=TEST_RESERVE_US,.run_deadline_us=TEST_RUN_US};
const struct ss_application ss_application={.manifest=&manifest};
#else
const struct ss_application ss_application={0};
#endif
static _Alignas(8) uint8_t data[SS_SESSION_MAX_BYTES],saved[SS_SESSION_MAX_BYTES];
static struct a733_board_session *prefix=(void *)data;
static struct ss_session_header *header=(void *)(data+SS_SESSION_PREFIX_BYTES);
static void append(uint32_t type,const void *payload,size_t bytes)
{
    struct ss_extent *e=(void *)(data+header->bytes);e->type=type;e->bytes=bytes;
    if(bytes)memcpy(e+1,payload,bytes);header->bytes+=sizeof(*e)+((bytes+7)&~(size_t)7);++header->extents;
}
static void baseline(void)
{
    struct os_session service;
    assert(sizeof(service_fixture)==sizeof(service));memcpy(&service,service_fixture,sizeof(service));
    assert(os_session_valid(&service)&&!service.board.deadline_us);
    memset(data,0,sizeof(data));memcpy(prefix,&service.board,sizeof(*prefix));
    *header=(struct ss_session_header){.magic=SS_SESSION_MAGIC,.version=SS_SESSION_VERSION,
        .bytes=SS_SESSION_PREFIX_BYTES+sizeof(*header),.flags=SS_SESSION_BIND_SID};
#ifndef NO_APPLICATION
    storage_digest_bytes(&manifest,sizeof(manifest),header->manifests_sha256);
#else
    storage_digest_bytes(NULL,0,header->manifests_sha256);
#endif
    append(SS_EXTENT_THERMAL,&service.magic,A733_OS_SESSION_BYTES-SS_SESSION_PREFIX_BYTES);
#ifndef NO_APPLICATION
    struct ss_input_fact input={.bytes=4096};storage_digest_bytes("input",5,input.sha256);
    append(SS_EXTENT_INPUTS,&input,sizeof(input));
#endif
    memcpy(saved,data,sizeof(data));
}
static void restore(void){memcpy(data,saved,sizeof(data));}
static int admit(void){struct os_session out;return os_session_decode(data,header->bytes,&out);}
int main(void)
{
    baseline();
#ifdef EXPECT_BUDGET_REFUSAL
    assert(!admit());puts("finite manifest budget beyond supervised lifetime refused");return 0;
#endif
    assert(admit());
    header->manifests_sha256[0]^=1;assert(!admit());restore();
    header->bytes=SS_SESSION_MAX_BYTES+1;assert(!admit());restore();
    header->extents++;assert(!admit());restore();
    header->flags=0;assert(!admit());restore();
    header->devices=SS_DEVICE_USB_KEYBOARD;assert(!admit());restore();
    header->devices=SS_DEVICE_AUDIO_I2S|SS_DEVICE_AUDIO_HEADSET;assert(!admit());restore();
    header->flags=0x80000000;assert(!admit());restore();
    append(SS_EXTENT_THERMAL,NULL,0);assert(!admit());restore();
    ((struct ss_extent *)(data+656))->bytes=SS_SESSION_MAX_BYTES;assert(!admit());restore();
    append(20,"x",1);data[header->bytes-1]=1;assert(!admit());restore();
    ((uint64_t *)(data+664))[2]=1;assert(!admit());restore();
    prefix->deadline_us=1000000;assert(!admit());restore();
    append(SS_EXTENT_AUDIO,"x",1);assert(!admit());restore();
    append(SS_EXTENT_NPU,"x",1);assert(!admit());restore();
    append(SS_EXTENT_CPU_GRANT,"x",1);assert(!admit());restore();
#ifndef NO_APPLICATION
    size_t bytes;struct ss_input_fact *input=(void *)os_session_extent(data,SS_EXTENT_INPUTS,&bytes);
    assert(bytes==sizeof(*input));input->bytes=4097;assert(!admit());restore();
    input=(void *)os_session_extent(data,SS_EXTENT_INPUTS,&bytes);input->bytes=0;assert(admit());restore();
    /* A valid host profile also needs the board's physical handoff fields. */
    prefix->workload=0;prefix->latch_identity=5;prefix->latch_owner=6;prefix->latch_known_mask=0x3fff;
    prefix->typec_owner=13;prefix->usb2_owner=14;prefix->usb2_revision=1;
    prefix->source_min_mv=4750;prefix->source_max_mv=5250;prefix->source_ma=500;
    prefix->zero_threshold_max_mv=600;prefix->fusb_device_id=0x81;
    header->devices=SS_DEVICE_USB_STORAGE;assert(admit());
    prefix->workload=4;header->devices|=SS_DEVICE_USB_KEYBOARD;assert(admit());restore();
#endif
    puts("native typed-session manifest binding, framing, device grants and bounds passed");return 0;
}
