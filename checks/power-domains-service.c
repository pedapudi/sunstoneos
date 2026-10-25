/* SPDX-License-Identifier: Apache-2.0 */
/* The PMIC service task over the shared model (checks/power-domains-model.h).
 *
 * The model runs the actual provider, ledger, owner and GPU controller. Its
 * shim records what the service task's ulTaskNotifyTake() would do: the ticks
 * each step returns, wakes, and notifications from other tasks. Bus timing
 * follows the inherited S_TWI0 divisor: a byte with acknowledgement takes
 * nine bit times and a START, repeated START or STOP two, at 400 kHz
 * (divisor 0x28) and 100 kHz (divisor 0x2a). A time read costs 40 ns and a
 * register access 200 ns, except in the board-cost cases below. Every case
 * sets each bus flag at a sub-microsecond time: the completion time of the
 * write that starts the phase plus the phase's wire time.
 *
 * Cases, for each rate:
 * - a DCDC2 snapshot and a DCDC3 selector write from another task: the steps,
 *   the tick waits while the request is outstanding and the longest step;
 * - one modeled core, where another caller runs only between steps, and two,
 *   where it runs at every time read and register access and polls its own
 *   request once per tick at every phase offset: BUSY results it sees from
 *   a733_pmic_live (never), a733_pmic_result and a733_pmic_reap;
 * - wakes in one second with no work, with and without an idle cap, and the
 *   wake that a park, stop or revocation notification causes;
 * - the GPU controller and a domain-1 row: ticks from the row request until
 *   the consumer's poll sees ON, and the PMIC transactions of a GPU start;
 * - the GPU supply consumer, which completes from the boot record with no
 *   bus transaction, and refuses when DCDC4 was not verified;
 * - the GPU display park predicate during a transfer and a domain transition.
 * Board-cost sensitivity, 400 kHz: read, write, poll and time-read costs
 * are independent. Component means give illustrative 506/402/77/144 ns;
 * they omit wrapper work and do not independently predict a board limit.
 * Legacy 529/1026 ns rows preserve the rejected fit for comparison only;
 * the 380 ns poll row, a fit to the board's median, is printed without an
 * asserted step time. Separate read and write floors assume 1 ns for the
 * other access (zero write_ns would silently reuse the read cost); with
 * reads at 379 ns, writes of 1 and 150 ns at 22 us bytes bracket the
 * snapshot within 110% of its bit-time wire. Equal-cost rows at 22 and
 * 23 us per byte expose the rounding sensitivity. No row establishes a
 * hardware floor or the attainable benefit of changing barriers.
 * Slow bus, 400 kHz: every flag sets 10% later than the divisor implies
 * (25 us bytes, 6 us conditions). A snapshot and a selector write each take
 * one step within 110% of that wire time.
 * Output: one JSON line per rate, one per board-cost row and one for the
 * slow bus, then PASS. */
#include "power-domains-model.h"
#include "a733_pmic_consumer.h"

struct caller {
    struct a733_pmic_request request;
    uint64_t next;
    unsigned active,completed,reaped,result_busy,reap_busy,live_busy,calls;
};
static struct caller other;
static void caller_poll(struct caller *c)
{
    struct a733_pmic_snapshot s;
    ++c->calls;
    if(!c->completed) {
        enum a733_pmic_status r=a733_pmic_result(&m.provider,&c->request,&s);
        if(r==A733_PMIC_BUSY)++c->result_busy;
        else if(r==A733_PMIC_COMPLETE)c->completed=1;
        else assert(r==A733_PMIC_PENDING);
    } else if(!c->reaped) {
        enum a733_pmic_status r=a733_pmic_reap(&m.provider,&c->request);
        if(r==A733_PMIC_BUSY)++c->reap_busy;else{assert(r==A733_PMIC_COMPLETE);c->reaped=1;}
    }
}
/* The second core: live() at every hook, the request once per tick. */
static void second_core(struct pmic_model *p)
{
    assert(p==&m);
    if(!other.active)return;
    if(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_BUSY)++other.live_busy;
    if(m.now>=other.next){other.next+=TICK_US;caller_poll(&other);}
}
static int gpu_now(void *v,uint64_t *out){(void)v;return pmic_model_now(&m,out);}
static int gpu_acquire(void *v,struct a733_gpu_power_resources *out)
{(void)v;*out=(struct a733_gpu_power_resources){A733_GPU_INITIAL_HZ,A733_GPU_INITIAL_UV};return 1;}
static int gpu_release(void *v){(void)v;return 1;}
/* The GPU CORE PPU: a policy write sets the status at its next read. */
static uint32_t gpu_core[0xc14/4]; /* policy..PWCR and the delay row, by word */
static unsigned gpu_core_pending;
static int gpu_read(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t *out)
{
    (void)v;
    if(bank==A733_GPU_CORE) {
        assert(off==0 || off==8 || off==0x14 || off==0x20 || off==0xfb0 ||
               off==0x170 || off==0x174 || off==0xc00 || off==0xc04 || off==0xc10);
        if(off==8 && gpu_core_pending){gpu_core[2]=gpu_core[0];gpu_core_pending=0;}
        *out=off==0xfb0 ? 0 : gpu_core[off/4];return 1;
    }
    uint32_t *w=level_word((bank==A733_GPU_CCU ? CCU_BASE : RCCU_BASE)+off);assert(w);*out=*w;return 1;
}
static int gpu_update(void *v,enum a733_gpu_power_bank bank,uint32_t off,uint32_t clear,uint32_t set)
{
    (void)v;
    if(bank==A733_GPU_CORE) {
        assert(clear==UINT32_MAX && ((off==0 && (set==0 || set==8)) || (off==0x20 && !set) ||
               (off==0x170 && set==0x1f1f1f) || (off==0x174 && set==0x1f1f)
               || (off==0xc00 && set==0x08080808) || (off==0xc04 && set==0x0808) || (off==0xc10 && set==8)));
        gpu_core[off/4]=set;if(!off)gpu_core_pending=1;
        return 1;
    }
    assert(bank==A733_GPU_CCU && (off==0xb20 || off==0xb24));
    uint32_t *w=level_word(CCU_BASE+off);*w=(*w&~clear)|set;
    if(off==0xb20)*w&=~(1u<<27);
    return 1;
}
static const struct a733_gpu_power_ops gpu_ops={gpu_now,gpu_acquire,gpu_release,gpu_read,gpu_update};
static struct a733_gpu_power gpu;
static struct a733_rail_token gpu_rail,fb_rail,submitter;
static struct a733_power_domain_row fb_row;

struct rate { const char *name; uint32_t divider; uint64_t wire_us,edge_us; };
struct request_cost { unsigned steps,tick_waits,submitter_polls; uint64_t longest_step_us,elapsed_us; };
static void setup(const struct rate *r,uint64_t access_ns)
{
    domains_model_setup(r->divider,r->wire_us);
    m.now_ns=40;m.access_ns=access_ns;m.edge_us=r->edge_us;
    gpu=(struct a733_gpu_power){0};fb_row=(struct a733_power_domain_row){0};
    memset(gpu_core,0,sizeof(gpu_core));gpu_core_pending=0;
    gpu_rail=fb_rail=submitter=(struct a733_rail_token){0};
    assert(a733_gpu_power_init(&gpu,&gpu_ops,0) && a733_power_domain_row_init(&gpu.row,&d.owner,5,9,&gpu_rail));
    assert(a733_power_domain_row_init(&fb_row,&d.owner,1,11,&fb_rail));
    domains_boot();
    assert(a733_rail_acquire(&m.rails,10,0,A733_RAIL_BIT(A733_RAIL_DCDC3),m.now+10000000,&submitter)==A733_RAIL_PENDING);
}
/* One request from another task. The submitter polls its result once per
 * tick; the service wakes on the submit notification or its own ticks. With
 * two cores the other caller runs inside the service's steps. */
static struct request_cost request(unsigned write,unsigned cores,uint64_t phase,unsigned with_other)
{
    struct request_cost c={0};
    struct a733_pmic_request q;struct a733_pmic_snapshot s;
    other=(struct caller){0};
    struct a733_rail_token t=write ? submitter : domains_rail(12,A733_RAIL_DCDC2);
    /* The other caller's request is queued first and completes first. */
    struct a733_rail_token o=domains_rail(13,A733_RAIL_DCDC1);
    if(with_other)assert(a733_pmic_submit(&m.provider,13,&o,A733_RAIL_DCDC1,m.now+1000000,&other.request)==A733_PMIC_PENDING);
    else other.completed=other.reaped=1;
    const unsigned notifies=d.notifies;
    assert((write ? a733_pmic_submit_write(&m.provider,write ? 10 : 12,&t,A733_RAIL_DCDC3,0,30,m.now+1000000,&q) :
        a733_pmic_submit(&m.provider,12,&t,A733_RAIL_DCDC2,m.now+1000000,&q))==A733_PMIC_PENDING);
    assert(d.notifies==notifies+1 && d.notified);
    const uint64_t start=m.now;uint64_t next_poll=start+TICK_US;
    other.active=with_other;other.next=start+phase;
    if(cores==2)m.concurrent=second_core;
    unsigned done=0;
    while(!done || !other.reaped) {
        if(d.notified || m.now>=d.wake_at) {
            const uint64_t before=m.now;
            const uint32_t ticks=service_step();
            if(!done) {
                ++c.steps;if(ticks==1)++c.tick_waits;
                if(m.now-before>c.longest_step_us)c.longest_step_us=m.now-before;
            }
            continue;
        }
        const uint64_t next=next_poll<d.wake_at ? next_poll : d.wake_at;
        if(next>m.now)m.now=next;
        if(m.now>=next_poll) {
            next_poll+=TICK_US;
            if(!done) {
                ++c.submitter_polls;
                enum a733_pmic_status r=a733_pmic_result(&m.provider,&q,&s);
                if(r==A733_PMIC_COMPLETE){done=1;c.elapsed_us=m.now-start;}
                else assert(r==A733_PMIC_PENDING);
            }
            if(cores==1 && with_other)caller_poll(&other);
        }
        if(cores==2 && with_other && m.now>=other.next){other.next+=TICK_US;caller_poll(&other);}
        assert(m.now-start<100000);
    }
    m.concurrent=0;other.active=0;
    assert(s.programmed_uv==800000 && a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    assert(a733_rail_release(&m.rails,&o,m.now+1000)==A733_RAIL_READY);
    if(!write)assert(a733_rail_release(&m.rails,&t,m.now+1000)==A733_RAIL_READY);
    return c;
}
static void print_cost(const char *name,struct request_cost c)
{
    printf("\"%s\":{\"steps\":%u,\"tick_waits\":%u,\"longest_step_us\":%llu,\"elapsed_us\":%llu,\"submitter_polls\":%u},",
        name,c.steps,c.tick_waits,(unsigned long long)c.longest_step_us,(unsigned long long)c.elapsed_us,c.submitter_polls);
}
/* GPU or framebuffer consumer: one poll per tick until the row is ON. */
static unsigned consumer_ticks(int gpu_consumer,unsigned on)
{
    unsigned ticks=0;
    if(gpu_consumer)assert((on ? a733_gpu_power_on(&gpu,m.now+1000000) : a733_gpu_power_off(&gpu,m.now+1000000))==A733_GPU_POWER_AGAIN);
    else assert(a733_power_domain_request(&fb_row,1));
    for(;;) {
        if(gpu_consumer) {
            enum a733_gpu_power_result r=a733_gpu_power_poll(&gpu);
            if(r==A733_GPU_POWER_OK)return ticks;
            assert(r==A733_GPU_POWER_AGAIN);
        } else if(a733_power_domain_state(&fb_row)==A733_POWER_ROW_ON)return ticks;
        /* The consumer blocks one tick; the notified service runs first. */
        const uint64_t tick=m.now+TICK_US;
        while(d.notified || d.wake_at<=tick) {
            if(d.wake_at>m.now && !d.notified)m.now=d.wake_at;
            service_step();
        }
        if(tick>m.now)m.now=tick;
        assert(++ticks<20);
    }
}
int main(int argc,char **argv)
{
    const unsigned board_only=argc==2 && !strcmp(argv[1],"--board-costs");
    assert(argc==1 || board_only);
    static const struct rate rates[]={{"400kHz",0x28,23,5},{"100kHz",0x2a,90,20}};
    for(unsigned k=0;k<2 && !board_only;++k) {
        const struct rate *r=&rates[k];
        printf("{\"rate\":\"%s\",",r->name);
        /* Boot: provider startup and supply verification. */
        setup(r,200);
        const uint64_t first_step=d.owner.verify_at-A733_POWER_SUPPLY_SETTLE_US;
        printf("\"boot\":{\"steps\":%u,\"tick_waits\":%u,\"self_notifies\":%u,\"verified_at_us\":%llu},",
            d.steps,d.tick_waits,d.self_notifies,(unsigned long long)(m.now-first_step));
        assert(m.now>=d.owner.verify_at);
        /* Requests on one core and on two, at every phase of the other caller. */
        setup(r,200);struct request_cost snapshot=request(0,1,0,0);
        setup(r,200);struct request_cost write=request(1,1,0,0);
        unsigned busy[2][3]={{0}},live=0;
        for(unsigned cores=1;cores<=2;++cores)for(uint64_t phase=0;phase<TICK_US;phase+=97)
            for(unsigned kind=0;kind<2;++kind) {
                setup(r,200);
                (void)request(kind,cores,phase,1);
                if(other.result_busy>busy[cores-1][0])busy[cores-1][0]=other.result_busy;
                if(other.reap_busy>busy[cores-1][1])busy[cores-1][1]=other.reap_busy;
                live+=other.live_busy;
            }
        print_cost("snapshot",snapshot);print_cost("selector_write",write);
        printf("\"busy\":{\"one_core\":{\"result\":%u,\"reap\":%u},\"two_cores\":{\"result\":%u,\"reap\":%u},\"live\":%u},",
            busy[0][0],busy[0][1],busy[1][0],busy[1][1],live);
        fflush(stdout);
        assert(!live && !busy[0][0] && !busy[0][1] && busy[1][0]<=1 && busy[1][1]<=1);
        /* Wire time: a read transaction has three conditions and four bytes,
         * a write two and three; a write request is seven reads and a write. */
        const uint64_t read_us=3*r->edge_us+4*r->wire_us,wire_snapshot=6*read_us,
            wire_write=7*read_us+2*r->edge_us+3*r->wire_us;
        printf("\"wire_us\":{\"snapshot\":%llu,\"selector_write\":%llu},",
            (unsigned long long)wire_snapshot,(unsigned long long)wire_write);fflush(stdout);
        if(k==0) {
            /* 400 kHz: each request in one step with no tick wait, within 10%
             * of its wire time. */
            assert(snapshot.steps==1 && !snapshot.tick_waits && snapshot.longest_step_us*10<=wire_snapshot*11);
            assert(write.steps==1 && !write.tick_waits && write.longest_step_us*10<=wire_write*11);
        } else {
            /* 100 kHz: no step spins more than 1 ms; 2 and 3 tick waits. */
            assert(snapshot.tick_waits==2 && write.tick_waits==3);
            assert(snapshot.longest_step_us<=A733_POWER_PMIC_SPIN_US+100 && write.longest_step_us<=A733_POWER_PMIC_SPIN_US+100);
        }
        /* No work: no wake for one second; a notification wakes it once. */
        setup(r,200);
        unsigned idle=service_until(m.now+1000000);
        d.owner.ops.notify(d.owner.context);unsigned notified=service_until(m.now+1);
        /* An image whose service reports progress to a watchdog caps the block. */
        setup(r,200);d.owner.config.idle_us=500000;service_step();
        unsigned capped=service_until(m.now+1000001);
        printf("\"idle_wakes_per_second\":%u,\"notified_wakes\":%u,\"capped_wakes_per_second\":%u,",idle,notified,capped);
        assert(idle==0 && notified==1 && capped==2);
        /* GPU and framebuffer rows: ON is visible at the consumer's next poll. */
        setup(r,200);
        gpu_rail=domains_rail(9,A733_RAIL_DCDC4);fb_rail=domains_rail(11,A733_RAIL_DCDC2);
        const unsigned twi=m.writes;
        unsigned gpu_on=consumer_ticks(1,1),gpu_twi=m.writes-twi;
        assert(gpu.phase==A733_GPU_POWER_ON && (d.ppu[5][2]&15)==8 && (d.ccu_b24&0x10001)==0x10001);
        unsigned fb_on=consumer_ticks(0,1);
        unsigned gpu_off=consumer_ticks(1,0);
        assert(gpu.phase==A733_GPU_POWER_OFF && (d.ppu[5][2]&15)==0 && !(d.ccu_b24&0x10001) && !(d.ccu_b20&(1u<<31)));
        printf("\"gpu_on_ticks\":%u,\"gpu_off_ticks\":%u,\"framebuffer_on_ticks\":%u,\"gpu_start_transactions\":%u,",
            gpu_on,gpu_off,fb_on,gpu_twi);
        assert(gpu_on==1 && gpu_off==1 && fb_on==1 && !gpu_twi);
        /* Park predicate: not idle during a transfer or a transition. */
        setup(r,200);
        struct a733_rail_token pt=domains_rail(14,A733_RAIL_DCDC2);struct a733_pmic_request pq;
        assert(a733_power_service_idle(&d.owner));
        assert(a733_pmic_submit(&m.provider,14,&pt,A733_RAIL_DCDC2,m.now+1000000,&pq)==A733_PMIC_PENDING);
        assert(!a733_power_service_idle(&d.owner));
        unsigned park_steps=0;
        while(!a733_power_service_idle(&d.owner)){service_step();++park_steps;assert(park_steps<10);}
        gpu_rail=domains_rail(9,A733_RAIL_DCDC4);d.stall[5]=1;
        assert(a733_gpu_power_on(&gpu,m.now+1000000)==A733_GPU_POWER_AGAIN && a733_gpu_power_poll(&gpu)==A733_GPU_POWER_AGAIN);
        assert(!a733_power_service_idle(&d.owner));service_step();assert(!a733_power_service_idle(&d.owner));
        d.stall[5]=0;m.now=d.wake_at;service_step();assert(a733_power_service_idle(&d.owner));
        /* The GPU supply consumer takes the boot record of DCDC4. */
        for(unsigned verified=0;verified<2;++verified) {
            domains_model_setup(r->divider,r->wire_us);if(!verified)m.pmic[0x15]=31;
            static struct a733_power_domain_row row;row=(struct a733_power_domain_row){0};
            assert(a733_power_domain_row_init(&row,&d.owner,5,15,&gpu_rail));domains_boot();
            static struct a733_pmic_consumer c;c=(struct a733_pmic_consumer){0};
            const struct a733_pmic_consumer_config config={&m.provider,m.lifetime,15,A733_RAIL_DCDC4};
            assert(a733_pmic_consumer_init(&c,&config));
            const unsigned before=m.writes;const uint64_t until=m.now+100000;int result=0;
            for(unsigned i=0;i<4 && !result;++i)result=a733_pmic_consumer_sample(&c,until);
            assert(m.writes==before);
            if(verified) {
                assert(result==1 && !c.request.id && c.snapshot.programmed_uv==800000 && (c.snapshot.selector&127)==30);
                assert(a733_pmic_consumer_health(&c)==1 && a733_pmic_consumer_refresh(&c,until)==1);
                for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)assert(m.provider.slots[i].state==0);
            } else assert(result==-1 && c.failed);
        }
        /* A fault that a client latches while the service holds the I2C
         * mutex for a transfer: the next step polls once and retains the
         * mutex, then the service stops ticking. */
        setup(r,200);
        struct a733_rail_token ft=domains_rail(16,A733_RAIL_DCDC2);struct a733_pmic_request fq;struct a733_pmic_snapshot fs;
        assert(a733_pmic_submit(&m.provider,16,&ft,A733_RAIL_DCDC2,m.now+1000000,&fq)==A733_PMIC_PENDING);
        m.edge_us=m.wire_us=100000; /* the transfer stays active across steps */
        service_step();assert(m.provider.held && m.i2c_held && !m.retained);
        a733_power_authority_set(&m.rails.authority,A733_POWER_CLOCK_FAULT);
        assert(a733_pmic_result(&m.provider,&fq,&fs)==A733_PMIC_RETAINED && m.provider.failed && !m.provider.retained);
        assert(a733_pmic_work(&m.provider,0,NULL)==(A733_PMIC_WORK_FAILED|A733_PMIC_WORK_RETAIN));
        const uint32_t retain_ticks=service_step();
        assert(m.provider.retained && m.retained && m.i2c_held);
        assert(a733_pmic_work(&m.provider,0,NULL)==A733_PMIC_WORK_FAILED && retain_ticks!=1);
        m.now+=TICK_US;assert(service_step()!=1 && a733_power_service_ready(&d.owner)==-1);
        printf("\"park_steps_after_transfer\":%u,\"gpu_supply_transactions\":0,\"retain_steps\":1}\n",park_steps);
    }
    {
        /* 252 nominal bit times; the board bound is empirical and is
         * exercised by board-bundles-apps-model.c against measured wire. */
        static const struct {
            const char *name;
            uint64_t read_ns,write_ns,poll_ns,now_ns,byte_us;
            unsigned write_steps;
            uint64_t snapshot_us;
        } rows[]={
            {"component means, incomplete",506,402,77,144,23,1,731},
            {"legacy fit at 250 ns poll",529,1026,250,150,23,1,757},
            {"legacy median fit, step not asserted",529,1026,380,150,23,1,0},
            {"legacy selector split",529,1026,386,150,23,2,781},
            {"legacy base remainder",529,1026,2340,150,23,2,850},
            {"read floor, assumed 1 ns write",379,1,0,40,23,1,688},
            {"read floor, assumed 1 ns write, 22 us byte",379,1,0,40,22,1,664},
            {"read floor, assumed 150 ns write, 22 us byte",379,150,0,40,22,1,692},
            {"write floor, assumed 1 ns read",1,379,0,40,23,1,685},
            {"assumed equal costs, 22 us byte",379,379,0,40,22,1,694},
            {"assumed equal costs, 23 us byte",379,379,0,40,23,1,712}};
        const uint64_t wire_snapshot_ns=6*(3*2+4*9)*UINT64_C(2500);
        for(unsigned i=0;i<sizeof(rows)/sizeof(rows[0]);++i) {
            const struct rate r={"400kHz",0x28,rows[i].byte_us,5};
            setup(&r,rows[i].read_ns);m.now_ns=rows[i].now_ns;m.write_ns=rows[i].write_ns;m.poll_ns=rows[i].poll_ns;
            struct request_cost snapshot=request(0,1,0,0);
            setup(&r,rows[i].read_ns);m.now_ns=rows[i].now_ns;m.write_ns=rows[i].write_ns;m.poll_ns=rows[i].poll_ns;
            struct request_cost write=request(1,1,0,0);
            printf("{\"case\":\"%s\",\"read_ns\":%llu,\"write_ns\":%llu,\"poll_ns\":%llu,\"now_ns\":%llu,\"byte_us\":%llu,",
                rows[i].name,(unsigned long long)rows[i].read_ns,(unsigned long long)rows[i].write_ns,
                (unsigned long long)rows[i].poll_ns,(unsigned long long)rows[i].now_ns,(unsigned long long)rows[i].byte_us);
            print_cost("snapshot",snapshot);print_cost("selector_write",write);
            printf("\"snapshot_permille_of_bit_time_wire\":%llu}\n",
                (unsigned long long)(snapshot.longest_step_us*1000000/wire_snapshot_ns));
            assert(snapshot.steps==1 && !snapshot.tick_waits &&
                (!rows[i].snapshot_us || snapshot.longest_step_us==rows[i].snapshot_us));
            assert(write.steps==rows[i].write_steps && write.tick_waits==rows[i].write_steps-1 &&
                write.longest_step_us<=A733_POWER_PMIC_SPIN_US+100);
        }
    }
    if(!board_only) {
        /* Slow bus: flags 10% later than the 400 kHz divisor implies. */
        static const struct rate slow={"400kHz divisor, 25 us bytes",0x28,25,6};
        const uint64_t read_us=3*slow.edge_us+4*slow.wire_us,wire_snapshot=6*read_us,
            wire_write=7*read_us+2*slow.edge_us+3*slow.wire_us;
        setup(&slow,200);struct request_cost snapshot=request(0,1,0,0);
        setup(&slow,200);struct request_cost write=request(1,1,0,0);
        printf("{\"rate\":\"%s\",",slow.name);
        print_cost("snapshot",snapshot);print_cost("selector_write",write);
        printf("\"wire_us\":{\"snapshot\":%llu,\"selector_write\":%llu}}\n",
            (unsigned long long)wire_snapshot,(unsigned long long)wire_write);
        assert(snapshot.steps==1 && !snapshot.tick_waits && snapshot.longest_step_us*10<=wire_snapshot*11);
        assert(write.steps==1 && !write.tick_waits && write.longest_step_us*10<=wire_write*11);
    }
    if(board_only) {puts("PASS PMIC board-cost sensitivity rows");return 0;}
    puts("PASS PMIC service task: bounded byte spins, no lock in live(), at most one BUSY result per caller, "
         "no idle wakes, one-tick row visibility, no PMIC transaction per GPU start, park predicate");
    return 0;
}
