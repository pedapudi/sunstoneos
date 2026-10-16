#include "a733_pins_native.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static struct a733_pins manager;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static uint32_t words[0x600/4];
static uint64_t now;
static unsigned held, reads, writes, fail_read, fail_write, ignore_write, expire_write, refuse_lock;
static uint32_t expected_masks[5];
static const unsigned offsets[]={0x100,0x104,0x120,0x124,0x130};
static int enter(void *cookie,uint64_t deadline)
{
    assert(cookie==&manager && now<deadline);
    if (refuse_lock || pthread_mutex_trylock(&mutex)) return 0;
    assert(!held); held=1; return 1;
}
static void leave(void *cookie)
{ assert(cookie==&manager && held); held=0; assert(!pthread_mutex_unlock(&mutex)); }
static int clock_now(void *cookie,uint64_t *out)
{ assert(cookie==&manager); *out=now; return 1; }
static int read_reg(void *cookie,uintptr_t address,uint32_t *out)
{
    assert(cookie==&manager && held && a733_pins_native_address(address,0));
    if (++reads==fail_read) return 0;
    *out=words[(address-A733_PINS_BASE)/4]; return 1;
}
static int write_reg(void *cookie,uintptr_t address,uint32_t value)
{
    assert(cookie==&manager && held && a733_pins_native_address(address,1));
    unsigned offset=address-A733_PINS_BASE, index=0;
    while (offsets[index]!=offset) { ++index; assert(index<5); }
    assert(!((words[offset/4]^value)&~expected_masks[index]));
    ++writes;
    if (writes!=ignore_write) words[offset/4]=value;
    if (writes==expire_write) now=100;
    return writes!=fail_write;
}
static void configure_field(unsigned pin,unsigned function,unsigned pull,unsigned drive)
{
    struct a733_pin_fields f; assert(a733_pin_fields(pin,&f));
    unsigned mux=(f.mux_address-A733_PINS_BASE)/4;
    unsigned dr=(f.drive_address-A733_PINS_BASE)/4, pu=(f.pull_address-A733_PINS_BASE)/4;
    words[mux]=(words[mux]&~(15u<<f.mux_shift))|(function<<f.mux_shift);
    words[dr]=(words[dr]&~(15u<<f.drive_shift))|(drive<<f.drive_shift);
    words[pu]=(words[pu]&~(3u<<f.pull_shift))|(pull<<f.pull_shift);
}
static void expected_writes(uint32_t mask)
{
    memset(expected_masks,0,sizeof(expected_masks));
    for (unsigned pin=4;pin<=8;++pin) if (mask&(1u<<pin)) {
        expected_masks[pin/8]|=15u<<((pin%8)*4);
        expected_masks[pin/8+2]|=15u<<((pin%8)*4);
        expected_masks[4]|=3u<<(pin*2);
    }
}
static void setup(void)
{
    assert(!held);
    memset(&manager,0,sizeof(manager));
    for (unsigned i=0;i<sizeof(words)/sizeof(*words);++i) words[i]=0xa5a55a5a;
    for (unsigned pin=4;pin<=8;++pin) configure_field(pin,15,1,2);
    words[0x150/4]=0x80000003; /* Other pins' interrupts stay enabled. */
    now=reads=writes=fail_read=fail_write=ignore_write=expire_write=refuse_lock=0;
    expected_writes(A733_PINS_PB_MASK);
    struct a733_pins_operations ops={enter,leave,clock_now,read_reg,write_reg};
    assert(a733_pins_init(&manager,&ops,&manager,A733_PINS_PB_MASK));
}
static struct a733_pin_request request(unsigned pin,unsigned function)
{ return (struct a733_pin_request){pin,{15,1,2},{function,0,1}}; }
static void check_held(const struct a733_pin_lease *token,int expected)
{
    int value=-1;
    assert(a733_pins_held(&manager,token,&value,1000)==A733_PINS_OK && value==expected);
}
static void check_failed(struct a733_pin_lease *token)
{
    check_held(token,1);
    unsigned old_reads=reads,old_writes=writes;
    assert(a733_pins_release(&manager,token,1,1000)==A733_PINS_RECOVERY);
    struct a733_pin_request r=request(4,0); struct a733_pin_lease other={0};
    assert(a733_pins_acquire(&manager,8,&r,1,&other,1000)==A733_PINS_RECOVERY);
    assert(!other.id && reads==old_reads && writes==old_writes);
}
static pthread_barrier_t start_barrier;
struct contender {uint64_t owner; struct a733_pin_lease token; enum a733_pins_result result;};
static void *claim_same_pin(void *arg)
{
    struct contender *c=arg; struct a733_pin_request r=request(5,3);
    int result=pthread_barrier_wait(&start_barrier);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    c->result=a733_pins_acquire(&manager,c->owner,&r,1,&c->token,100);
    return NULL;
}
int main(void)
{
    setup(); struct a733_pin_request r[3]={request(5,3),request(6,3),request(8,3)};
    struct a733_pin_lease audio={0}, gpio={0}, stale;
    uint32_t baseline[sizeof(words)/sizeof(*words)]; memcpy(baseline,words,sizeof(words));
    expected_writes((1u<<5)|(1u<<6)|(1u<<8));
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_OK); check_held(&audio,1);
    unsigned old_writes=writes;
    struct a733_pin_request conflict=request(6,0);
    assert(a733_pins_acquire(&manager,2,&conflict,1,&gpio,100)==A733_PINS_CONFLICT);
    assert(!gpio.id && writes==old_writes);
    struct a733_pin_request input=request(4,0); expected_writes(1u<<4);
    assert(a733_pins_acquire(&manager,2,&input,1,&gpio,100)==A733_PINS_OK);
    uint32_t shared_cfg=words[0x100/4],shared_drive=words[0x120/4],shared_pull=words[0x130/4];
    stale=audio; expected_writes(audio.pb_mask);
    assert(a733_pins_release(&manager,&audio,0,100)==A733_PINS_INVALID);
    assert(a733_pins_release(&manager,&audio,1,100)==A733_PINS_OK); check_held(&stale,0);
    assert(((words[0x100/4]^shared_cfg)&(15u<<16))==0);
    assert(((words[0x120/4]^shared_drive)&(15u<<16))==0);
    assert(((words[0x130/4]^shared_pull)&(3u<<8))==0);
    words[0x110/4]=1u<<4; uint32_t value=0;
    assert(a733_pins_read_inputs(&manager,&gpio,1u<<4,&value,100)==A733_PINS_OK && value==(1u<<4));
    expected_writes(gpio.pb_mask); assert(a733_pins_release(&manager,&gpio,1,100)==A733_PINS_OK);
    for (unsigned i=0;i<5;++i) assert(words[offsets[i]/4]==baseline[offsets[i]/4]);
    expected_writes((1u<<5)|(1u<<6)|(1u<<8));
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_OK && audio.id!=stale.id);
    old_writes=writes;
    assert(a733_pins_release(&manager,&stale,1,100)==A733_PINS_CONFLICT && writes==old_writes);
    struct a733_pin_lease subset=audio;subset.pb_mask=1u<<5;
    assert(a733_pins_release(&manager,&subset,1,100)==A733_PINS_CONFLICT);
    assert(a733_pins_release(&manager,&audio,1,100)==A733_PINS_OK);

    setup(); audio=(struct a733_pin_lease){0}; refuse_lock=1;
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_BUSY && !reads && !writes && !audio.id);
    refuse_lock=0; now=100;
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_BUSY && !reads && !writes);
    now=0; configure_field(6,0,1,2);
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_CONFLICT && !writes && !audio.id);
    setup(); words[0x150/4]|=1u<<6;
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_CONFLICT && !writes);
    setup(); struct a733_pin_request invalid=request(5,2);
    assert(a733_pins_acquire(&manager,1,&invalid,1,&audio,100)==A733_PINS_INVALID && !reads);
    invalid=request(5,3); invalid.desired.drive=4;
    assert(a733_pins_acquire(&manager,1,&invalid,1,&audio,100)==A733_PINS_INVALID && !reads);
    invalid.desired.drive=1;invalid.desired.pull=3;
    assert(a733_pins_acquire(&manager,1,&invalid,1,&audio,100)==A733_PINS_INVALID && !reads);

    /* A failed write may have taken effect. Keep all requested pins and stop
     * after the failure; no subsequent call attempts automatic restoration. */
    setup(); fail_write=1;
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_RECOVERY);
    assert(audio.id && writes==1); check_failed(&audio);
    setup(); audio=(struct a733_pin_lease){0}; ignore_write=1;
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_RECOVERY && writes==1);
    check_failed(&audio);
    setup(); audio=(struct a733_pin_lease){0}; expire_write=1;
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_RECOVERY && writes==1);
    check_failed(&audio);
    setup(); audio=(struct a733_pin_lease){0}; fail_read=1;
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_RECOVERY && !audio.id && !writes);
    setup(); audio=(struct a733_pin_lease){0};
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_OK);
    fail_write=writes+1;
    assert(a733_pins_release(&manager,&audio,1,100)==A733_PINS_RECOVERY); check_failed(&audio);
    setup(); audio=(struct a733_pin_lease){0};
    assert(a733_pins_acquire(&manager,1,r,3,&audio,100)==A733_PINS_OK);
    configure_field(6,0,0,1); old_writes=writes;
    assert(a733_pins_release(&manager,&audio,1,100)==A733_PINS_RECOVERY && writes==old_writes);
    check_failed(&audio);

    setup(); expected_writes(1u<<5);
    struct contender contenders[2]={{.owner=10},{.owner=11}}; pthread_t threads[2];
    assert(!pthread_barrier_init(&start_barrier,NULL,2));
    for (unsigned i=0;i<2;++i) assert(!pthread_create(&threads[i],NULL,claim_same_pin,&contenders[i]));
    for (unsigned i=0;i<2;++i) assert(!pthread_join(threads[i],NULL));
    unsigned winners=0;
    for (unsigned i=0;i<2;++i) {
        if (contenders[i].result==A733_PINS_OK) {
            ++winners;check_held(&contenders[i].token,1);
            assert(a733_pins_release(&manager,&contenders[i].token,1,100)==A733_PINS_OK);
        } else assert(contenders[i].result==A733_PINS_CONFLICT || contenders[i].result==A733_PINS_BUSY);
    }
    assert(winners==1); assert(!pthread_barrier_destroy(&start_barrier));
    assert(!a733_pins_native_address(0x02000110,1));
    assert(!a733_pins_native_address(0x02000150,1));
    assert(!a733_pins_native_address(0x02000048,1));
    assert(!a733_pins_native_address(0x02000101,0));
    assert(!a733_pins_native_address(0x02000200,1));
    puts("Pin ownership: shared-word preservation, stale/conflicting leases, concurrent claims, deadlines, and retained failures passed");
}
