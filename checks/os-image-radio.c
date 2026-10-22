/* The integrated OS image's radio entry (apps/os/radio.c) and, in a resident
 * build, its resident radio profile bounds (apps/os/radio_profile.c), against
 * a modeled sealed session, radio profile span and translation. Both sources
 * run unchanged; the runner (checks/os-image-radio.py) executes them under
 * unicorn. Each failed expectation stops with its own code. */
#include "../apps/os/radio.c"
#ifdef SUNSTONE_BOARD_RESIDENT
#include "../apps/os/radio_profile.c"
#endif
struct board_application board;
volatile unsigned os_check_result;
static struct os_session session;
static unsigned attributes=0xff;
#define PROFILE ((struct board_radio_profile *)(uintptr_t)UINT64_C(0x43000000))
const struct os_session *os_session_admitted(void){return &session;}
int aarch64_device_translate(uintptr_t address,int write,struct aarch64_translation *t)
{
    if(write||address<UINT64_C(0x43000000)||address>=UINT64_C(0x43100000))return 0;
    *t=(struct aarch64_translation){address,attributes,3};return 1;
}
static void check(int ok,unsigned code){if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
/* A profile with distinct radio and crypto engine owners, sealed into the session. */
static void nominal(void)
{
    memset(&session,0,sizeof(session));memset(PROFILE,0,sizeof(*PROFILE));
    PROFILE->magic=BOARD_RADIO_PROFILE_MAGIC;PROFILE->version=BOARD_RADIO_PROFILE_LISTENER_SUPERVISED_VERSION;
    PROFILE->bytes=sizeof(*PROFILE);PROFILE->platform.owner=31;PROFILE->ce.owner=32;
    PROFILE->tcp_mode=1;PROFILE->peer_address[0]=10;
    session.board.workload=1;session.board.generation=9;
    session.board.radio_profile_address=UINT64_C(0x43000000);session.board.radio_profile_bytes=sizeof(*PROFILE);
    session.thermal_owner=15;
#ifdef SUNSTONE_A733_OS_AUDIO
    session.audio.dma.owner=17;
#endif
    storage_digest_bytes(PROFILE,sizeof(*PROFILE),session.board.radio_profile_digest);
    attributes=0xff;
}
static void reseal(void){storage_digest_bytes(PROFILE,sizeof(*PROFILE),session.board.radio_profile_digest);}
void os_check_run(void)
{
    struct a733_pin_handoff pins;memset(&pins,0,sizeof(pins));
    nominal();check(os_radio_pins(&pins)==1,10);
    check(!os_radio_pins(NULL),11);
    /* The authenticated profile, not a changed one, names the owners. */
    nominal();PROFILE->ce.owner=33;check(!os_radio_pins(&pins),12);
    nominal();PROFILE->platform.owner=session.thermal_owner;reseal();check(!os_radio_pins(&pins),13);
    nominal();PROFILE->ce.owner=session.thermal_owner;reseal();check(!os_radio_pins(&pins),14);
#ifdef SUNSTONE_A733_OS_AUDIO
    nominal();PROFILE->platform.owner=session.audio.dma.owner;reseal();check(!os_radio_pins(&pins),15);
    nominal();PROFILE->ce.owner=session.audio.dma.owner;reseal();check(!os_radio_pins(&pins),16);
#endif
    nominal();session.board.workload=4;check(!os_radio_pins(&pins),17);
    nominal();session.board.radio_profile_bytes=sizeof(*PROFILE)-8;check(!os_radio_pins(&pins),18);
    nominal();session.board.radio_profile_address=UINT64_C(0x43001000);check(!os_radio_pins(&pins),19);
    /* An uncached or otherwise different mapping of the profile is refused. */
    nominal();attributes=0x44;check(!os_radio_pins(&pins),20);
    /* Start requires main.c's radio owner and network client. */
    nominal();check(!os_radio_start(),21);
    board.handles[5]=(TaskHandle_t)&board.tasks[5];check(!os_radio_start(),22);
    board.handles[6]=(TaskHandle_t)&board.tasks[6];check(os_radio_start()==1,23);
    session.board.workload=0;check(!os_radio_start(),24);
#ifdef SUNSTONE_BOARD_RESIDENT
    /* Resident bounds: fixed bootstrap and startup offsets from the session
     * start, and the session's unbounded lifetime for the radio and client. */
    static struct board_radio_profile out;uint8_t digest[32],expected[32];
    struct a733_board_session *b=&session.board;
    nominal();b->session_start_ticks=24*UINT64_C(5000000);b->deadline_us=UINT64_MAX/24;
    check(board_radio_profile_runtime(&out,PROFILE,b,digest)==1,30);
    check(out.version==BOARD_RADIO_PROFILE_LISTENER_VERSION&&out.platform.handoff==9&&out.ce.handoff==9,31);
    check(out.bootstrap_deadline_us==5000000+OS_RADIO_BOOTSTRAP_US&&out.platform.deadline_us==out.bootstrap_deadline_us&&
        out.ce.deadline_us==out.bootstrap_deadline_us&&out.startup_deadline_us==5000000+OS_RADIO_STARTUP_US,32);
    check(out.platform.lifetime_deadline_us==UINT64_MAX/24-10000&&out.client_deadline_us==out.platform.lifetime_deadline_us,33);
    storage_digest_bytes(&out,sizeof(out),expected);check(!memcmp(digest,expected,32),34);
    /* A 60-second session, a profile that already carries a deadline and a
     * changed profile are refused. */
    b->deadline_us=5000000+60000000;check(!board_radio_profile_runtime(&out,PROFILE,b,digest),35);
    b->deadline_us=UINT64_MAX/24;PROFILE->client_deadline_us=1;check(!board_radio_profile_runtime(&out,PROFILE,b,digest),36);
    reseal();check(!board_radio_profile_runtime(&out,PROFILE,b,digest),37);
    nominal();b->deadline_us=UINT64_MAX/24;PROFILE->version=BOARD_RADIO_PROFILE_LISTENER_VERSION;reseal();
    check(!board_radio_profile_runtime(&out,PROFILE,b,digest),38);
#endif
    os_check_result=1;
}
