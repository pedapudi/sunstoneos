/* The integrated OS image's radio entry (apps/os/radio.c) and its radio
 * profile bounds in each lifetime (apps/os/radio_profile.c, with the
 * supervised bounds of apps/board/radio_profile.c), against a modeled sealed
 * session, radio profile span and translation. The sources run unchanged;
 * the runner (checks/os-image-radio.py) executes them under unicorn. Each
 * failed expectation stops with its own code. */
#include "../apps/os/radio.c"
#include "../apps/os/radio_profile.c"
#include "../apps/os/radio_profile_supervised.c"
/* The admitted lifetime of the modeled session (enum a733_lifetime). */
static unsigned model_lifetime=A733_LIFETIME_RESIDENT;
unsigned board_session_lifetime(void){return model_lifetime;}
/* This fixture covers admission and startup, without running a return. */
int os_quiesce_register(struct os_quiesce *q){(void)q;return 1;}
int os_quiesce_requested(const struct os_quiesce *q){(void)q;return 0;}
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *q){(void)q;return OS_QUIESCE_PENDING;}
void os_quiesce_complete(struct os_quiesce *q){(void)q;}
int os_quiesce_fault(struct os_quiesce *q,uint32_t site,uint32_t value,const void *caller)
{(void)q;(void)site;(void)value;(void)caller;return 0;}
void board_fail_at(uint32_t site,uint32_t value,const void *caller){(void)site;(void)value;(void)caller;}
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t value,eNotifyAction action,uint32_t *previous)
{(void)task;(void)index;(void)value;(void)action;(void)previous;return pdPASS;}
int board_radio_shutdown(struct board_radio *r,uint64_t deadline){(void)r;(void)deadline;return 0;}
uint64_t a733_smp_counter(void){return 0;}
void os_wifi_credential_poll(void){}
int os_wifi_credential_wiped(void){return 1;}
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
    session.board.workload=1;session.board.generation=9;session.granted=OS_GRANT_RADIO;
    session.board.radio_profile_address=UINT64_C(0x43000000);session.board.radio_profile_bytes=sizeof(*PROFILE);
    session.thermal_owner=15;session.audio.dma.owner=17;
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
    /* With the I2S service granted, its DMA owner is refused too; without the grant its record is not read. */
    nominal();PROFILE->platform.owner=session.audio.dma.owner;reseal();check(os_radio_pins(&pins)==1,39);
    nominal();session.granted|=OS_GRANT_AUDIO;PROFILE->platform.owner=session.audio.dma.owner;reseal();
    check(!os_radio_pins(&pins),15);
    nominal();session.granted|=OS_GRANT_AUDIO;PROFILE->ce.owner=session.audio.dma.owner;reseal();
    check(!os_radio_pins(&pins),16);
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
    /* A session that does not grant the radio: no pin is authorized, no profile byte is read (a changed profile and an
     * unreadable translation do not matter) and the start entry has nothing to do. */
    for(unsigned w=1;w<=3;w+=2){   /* both radio workloads: the full station (1) and the scanning station (3) */
        nominal();session.board.workload=w;session.granted=0;attributes=0x44;PROFILE->ce.owner=33;
        check(os_radio_pins(&pins)==1&&!pins.authorized[A733_PIN_B]&&os_radio_start()==1,25);
    }
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
    /* A supervised session takes the board's bounds: a session that ends BOARD_RADIO_SESSION_US after it starts gets
     * the board's offsets, where they fit before the session's end, and an unbounded session is refused. */
    model_lifetime=A733_LIFETIME_SUPERVISED;
    const int fits=BOARD_RADIO_STARTUP_US<BOARD_RADIO_SESSION_US-BOARD_RADIO_CLIENT_MARGIN_US;
    nominal();b->session_start_ticks=24*UINT64_C(5000000);b->deadline_us=5000000+BOARD_RADIO_SESSION_US;
    check(board_radio_profile_runtime(&out,PROFILE,b,digest)==fits,40);
    check(!fits||(out.version==BOARD_RADIO_PROFILE_LISTENER_VERSION&&out.bootstrap_deadline_us==5000000+BOARD_RADIO_BOOTSTRAP_US&&
        out.startup_deadline_us==5000000+BOARD_RADIO_STARTUP_US&&
        out.client_deadline_us==b->deadline_us-BOARD_RADIO_CLIENT_MARGIN_US),41);
    b->deadline_us=UINT64_MAX/24;check(!board_radio_profile_runtime(&out,PROFILE,b,digest),42);
    os_check_result=1;
}
