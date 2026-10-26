#include "pvr_cccb.h"
#include "reference.h"
#include <stdio.h>

static struct pvr_cccb client;
static struct pvr_ccb queue;
static unsigned char control[32], ring[1088], visible[1024], expected[1024];
static unsigned char qmem[5][128*88], last_kick[88], payload[400];
static struct rogue_fwif_cccb_ctl ref_control;
static struct reference_cccb ref;
static uint64_t now;
static unsigned io, writes, kicks, hold_return, status, inject_write, inject_barrier;
static unsigned late_write, clock_fail, clock_back, fail_queue_kick;
static void put(unsigned char *p, uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i)); }
static uint32_t get(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int cr(void *ctx,enum pvr_cccb_region r,uint32_t off,void *out,size_t n)
{assert(ctx==&client && r==PVR_CCCB_CONTROL && n==4 && !(off&3) && off+4<=32);++io;memcpy(out,control+off,n);return 1;}
static int cw(void *ctx,enum pvr_cccb_region r,uint32_t off,const void *in,size_t n)
{
    assert(ctx==&client);++io;++writes;
    if(r==PVR_CCCB_CONTROL) {assert(client.phase==PVR_CCCB_FORMAT && !off && n==32);memcpy(control,in,n);}
    else {assert(off+n<=1024 && n<=256);memcpy(ring+32+off,in,n);}
    if(late_write)now=client.deadline;
    return inject_write ? 0 : 1;
}
static int cb(void *ctx,enum pvr_ccb_barrier b)
{assert(ctx==&client);++io;if(b==PVR_CCB_RELEASE)memcpy(visible,ring+32,1024);return inject_barrier ? 0 : 1;}
static int ct(void *ctx,uint64_t *out)
{assert(ctx==&client);++io;*out=clock_back ? 0 : now;return !clock_fail;}
static const struct pvr_cccb_ops ops={ct,cr,cw,cb};
static int qr(void *ctx,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{assert(ctx==&queue && off+n<=pvr_ccb_region_bytes(r));++io;memcpy(out,qmem[r]+off,n);return 1;}
static int qw(void *ctx,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{assert(ctx==&queue && off+n<=pvr_ccb_region_bytes(r));++io;memcpy(qmem[r]+off,in,n);return 1;}
static int qb(void *ctx,enum pvr_ccb_barrier b)
{assert(ctx==&queue);(void)b;++io;return 1;}
static int qk(void *ctx,uint32_t off,uint32_t value)
{
    assert(ctx==&queue && off==0xb00 && !value);++io;
    unsigned next=get(qmem[0]),slot=(next-1)&127;
    const unsigned char *cmd=qmem[1]+slot*88;
    if(get(cmd)==(PVR_CCB_MAGIC|101u)) {
        assert(!memcmp(visible,ring+32,1024));
        memcpy(last_kick,cmd,88);++kicks;
        put(control,get(cmd+12)); /* Firmware applies the cursor update. */
        if(!hold_return) {put(qmem[0]+128,next);put(qmem[2]+slot*4,status);}
    } else assert(get(cmd)==PVR_CCB_HEALTH);
    return !fail_queue_kick;
}
static void setup(void)
{
    memset(&client,0,sizeof(client));memset(&queue,0,sizeof(queue));memset(qmem,0,sizeof(qmem));
    memset(control,0xa5,sizeof(control));memset(ring,0xa5,sizeof(ring));memset(expected,0xa5,sizeof(expected));
    memset(visible,0xa5,sizeof(visible));memset(last_kick,0,sizeof(last_kick));
    memset(&ref_control,0,sizeof(ref_control));
    ref=(struct reference_cccb){&ref_control,expected,1024,0,1023};cccb_ctrl_init(&ref_control,&ref);
    for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(unsigned char)(i*19);
    now=1;io=writes=kicks=hold_return=inject_write=inject_barrier=late_write=clock_fail=clock_back=fail_queue_kick=0;status=1;
    const struct pvr_ccb_ops qops={qr,qw,qb,qk};
    assert(pvr_ccb_init(&queue,&qops,&queue,7)==PVR_CCB_OK);
    assert(pvr_cccb_init(&client,&ops,&client,&queue,1024,0xc0300000,10000));
    assert(pvr_cccb_poll(&client)==PVR_CCCB_OK);
    assert(!memcmp(control,&ref_control,32));
    assert(pvr_ccb_start(&queue,++now)==PVR_CCB_OK);
}
static enum pvr_cccb_result tick(void)
{
    ++now;
    if(queue.phase==PVR_CCB_ACTIVE)assert(pvr_ccb_poll(&queue,now)==PVR_CCB_OK);
    return pvr_cccb_poll(&client);
}
static void finish(void)
{
    for(unsigned n=0;n<64;++n) {enum pvr_cccb_result r=tick();if(r==PVR_CCCB_OK)return;assert(r==PVR_CCCB_AGAIN);}
    assert(0);
}
static void until(unsigned phase)
{for(unsigned n=0;n<64 && client.phase!=phase;++n)assert(tick()==PVR_CCCB_AGAIN);assert(client.phase==phase);}
static struct pvr_cccb_command command(unsigned bytes)
{return (struct pvr_cccb_command){PVR_CCB_MAGIC|PVR_CCCB_TASK|205u,bytes,11,22,payload};}
static void begin(unsigned bytes)
{struct pvr_cccb_command cmd=command(bytes);assert(pvr_cccb_begin(&client,&cmd,1,NULL,0,now+1000));}
static void terminal(unsigned error)
{
    assert(client.phase==PVR_CCCB_FAILED && client.error==error && queue.phase==PVR_CCB_FAILED);
    unsigned before=io;assert(pvr_cccb_poll(&client)==PVR_CCCB_FAULT);
    assert(pvr_cccb_abort(&client)==PVR_CCCB_FAULT);assert(io==before);
}
static void reference_cases(void)
{
    setup();
    _Static_assert(sizeof(struct rogue_fwif_ccb_cmd_header)==40,"header ABI");
    _Static_assert(sizeof(struct rogue_fwif_cccb_ctl)==32,"control ABI");
    _Static_assert(sizeof(struct rogue_fwif_kccb_cmd)==88,"kernel command ABI");
    _Static_assert(ROGUE_FWIF_KCCB_CMD_KICK_DATA_MAX_NUM_CLEANUP_CTLS==3,"cleanup ABI");
    for(unsigned n=0;n<18;++n) {
        struct pvr_cccb_command cmds[]={command(n%2 ? 320 : 104),
            {PVR_CCB_MAGIC|213u,8,33,44,payload}};
        unsigned count=n%2 ? 1 : 2, bytes=0;
        uint32_t cleanup=0xc0310000+offsetof(struct rogue_fwif_hwrtdata,cleanup_state);
        ref_control.read_offset=ref.write_offset;put(control+4,ref.write_offset);
        for(unsigned i=0;i<count;++i)bytes+=pvr_cccb_get_size_of_cmd_with_hdr(cmds[i].bytes);
        assert(pvr_cccb_cmdseq_can_fit(&ref,bytes) && pvr_cccb_cmdseq_fits(&ref,bytes));
        assert(pvr_cccb_begin(&client,cmds,count,&cleanup,1,now+1000));
        for(unsigned i=0;i<count;++i)pvr_cccb_write_command_with_header(&ref,cmds[i].type,cmds[i].bytes,
            (void *)cmds[i].payload,cmds[i].external_reference,cmds[i].internal_reference);
        finish();assert(client.status==1 && client.write==ref.write_offset);
        assert(!memcmp(ring+32,expected,1024));
        struct rogue_fwif_kccb_cmd k={.cmd_type=ROGUE_FWIF_KCCB_CMD_KICK};
        struct pvr_hwrt_data hwrt={0xc0310000};fill_cmd_kick_data(&ref,0xc0300000,&hwrt,&k.cmd_data.cmd_kick_data);
        assert(!memcmp(&k,last_kick,88));
        for(unsigned i=0;i<32;++i)assert(ring[i]==0xa5 && ring[1056+i]==0xa5);
    }
    assert(kicks==18);
    setup();begin(0);finish();assert(client.write==40 && get(ring+36)==0);
}
/* The eight-job session (docs/designs/gpu-job-session.md): each job is the
 * 96-byte CDM command and the 8-byte timeline UPDATE, 184 bytes with their
 * headers, with no cleanup controls. Five jobs fill 920 bytes; before the
 * sixth the ring holds a padding command (type 221, size 64) at 920 and the
 * sixth job starts at 0, as upstream writes it. */
static void session_cases(void)
{
    setup();
    for(unsigned n=0;n<8;++n) {
        struct pvr_cccb_command cmds[]={command(96),{PVR_CCB_MAGIC|213u,8,0,0,payload}};
        ref_control.read_offset=ref.write_offset;put(control+4,ref.write_offset);
        assert(pvr_cccb_cmdseq_fits(&ref,184));
        assert(pvr_cccb_begin(&client,cmds,2,NULL,0,now+1000));
        for(unsigned i=0;i<2;++i)pvr_cccb_write_command_with_header(&ref,cmds[i].type,cmds[i].bytes,
            (void *)cmds[i].payload,cmds[i].external_reference,cmds[i].internal_reference);
        finish();assert(client.status==1 && client.write==ref.write_offset);
        assert(client.write==(n<5 ? 184u*(n+1) : 184u*(n-4)));
        assert(!memcmp(ring+32,expected,1024) && get(last_kick+12)==client.write);
        if(n==5) {
            assert(get(ring+32+920)==(PVR_CCB_MAGIC|221u) && get(ring+32+924)==64);
            assert(get(ring+32)==(PVR_CCB_MAGIC|PVR_CCCB_TASK|205u) && get(ring+32+136)==(PVR_CCB_MAGIC|213u));
        }
    }
    assert(kicks==8);
}
static void pressure_cases(void)
{
    setup();begin(320);finish();begin(320);finish(); /* 720 bytes occupied. */
    begin(320);unsigned before=writes;
    assert(tick()==PVR_CCCB_AGAIN && client.phase==PVR_CCCB_SPACE && writes==before);
    put(control+4,720);finish();assert(client.write==360 && kicks==3);
    setup();
    unsigned char health[88]={0};put(health,PVR_CCB_HEALTH);
    struct pvr_ccb_token tokens[127];
    for(unsigned i=0;i<127;++i)assert(pvr_ccb_submit(&queue,health,now,10000,&tokens[i])==PVR_CCB_OK);
    begin(24);until(PVR_CCCB_SUBMIT);before=writes;
    assert(tick()==PVR_CCCB_AGAIN && client.phase==PVR_CCCB_SUBMIT && writes==before && !kicks);
    put(qmem[0]+128,127);for(unsigned i=0;i<127;++i)put(qmem[2]+i*4,1);
    assert(pvr_ccb_poll(&queue,++now)==PVR_CCB_OK);
    for(unsigned i=0;i<127;++i) {uint32_t v;assert(pvr_ccb_collect(&queue,tokens[i],&v)==PVR_CCB_OK && v==1);}
    finish();assert(kicks==1 && writes==before);
    setup();hold_return=1;begin(24);until(PVR_CCCB_WAIT);
    assert(tick()==PVR_CCCB_AGAIN && client.phase==PVR_CCCB_WAIT && kicks==1);
    put(qmem[2],1);finish();assert(kicks==1 && client.status==1);
    /* The open firmware consumes a KICK without writing its return word
     * (board-session-20261025-18): accepted with status 0 once the queue's
     * poll, which each tick makes first, has sampled the read offset past it. */
    setup();hold_return=1;begin(24);until(PVR_CCCB_WAIT);
    put(qmem[0]+128,get(qmem[0]));
    assert(tick()==PVR_CCCB_OK && kicks==1 && client.status==0 && client.phase==PVR_CCCB_IDLE);
}
static void failure_cases(void)
{
    setup();begin(24);put(control+12,511);assert(tick()==PVR_CCCB_FAULT);terminal(PVR_CCCB_CURSOR);
    setup();begin(24);put(control+4,8);assert(tick()==PVR_CCCB_FAULT);terminal(PVR_CCCB_CURSOR);
    setup();begin(24);put(control+4,1);assert(tick()==PVR_CCCB_FAULT);terminal(PVR_CCCB_CURSOR);
    setup();begin(24);until(PVR_CCCB_WRITE);inject_write=1;
    assert(tick()==PVR_CCCB_FAULT && get(ring+32)==command(24).type);terminal(PVR_CCCB_IO);
    setup();begin(24);until(PVR_CCCB_PUBLISH);inject_barrier=1;
    assert(tick()==PVR_CCCB_FAULT);terminal(PVR_CCCB_IO);
    setup();begin(24);until(PVR_CCCB_WRITE);late_write=1;
    assert(tick()==PVR_CCCB_FAULT && !kicks);terminal(PVR_CCCB_TIMEOUT);
    setup();begin(24);clock_fail=1;assert(tick()==PVR_CCCB_FAULT);terminal(PVR_CCCB_IO);
    setup();begin(24);clock_back=1;assert(tick()==PVR_CCCB_FAULT);terminal(PVR_CCCB_CLOCK);
    setup();begin(24);now=client.deadline;assert(pvr_cccb_poll(&client)==PVR_CCCB_FAULT);terminal(PVR_CCCB_TIMEOUT);
    setup();begin(24);queue.last_now=now+1;assert(pvr_cccb_poll(&client)==PVR_CCCB_FAULT);terminal(PVR_CCCB_CLOCK);
    setup();begin(24);++queue.epoch;assert(pvr_cccb_poll(&client)==PVR_CCCB_FAULT);terminal(PVR_CCCB_QUEUE);
    setup();begin(24);until(PVR_CCCB_SUBMIT);fail_queue_kick=1;
    assert(tick()==PVR_CCCB_FAULT && kicks==1);terminal(PVR_CCCB_QUEUE);
    setup();status=3;begin(24);until(PVR_CCCB_WAIT);assert(tick()==PVR_CCCB_FAULT);terminal(PVR_CCCB_COMMAND);
    setup();hold_return=1;begin(24);until(PVR_CCCB_WAIT);now=client.deadline;
    assert(pvr_cccb_poll(&client)==PVR_CCCB_FAULT);terminal(PVR_CCCB_TIMEOUT);
    setup();assert(pvr_cccb_abort(&client)==PVR_CCCB_FAULT);terminal(PVR_CCCB_QUEUE);
}
static void refusal_cases(void)
{
    setup();struct pvr_cccb saved=client;unsigned before=io;
    struct pvr_cccb_command cmd=command(24);
    assert(!pvr_cccb_begin(&client,&cmd,0,NULL,0,100));
    cmd.type=PVR_CCB_MAGIC|221u;assert(!pvr_cccb_begin(&client,&cmd,1,NULL,0,100));
    cmd=command(7);assert(!pvr_cccb_begin(&client,&cmd,1,NULL,0,100));
    cmd=command(440);assert(!pvr_cccb_begin(&client,&cmd,1,NULL,0,100));
    cmd=command(24);cmd.payload=&client;assert(!pvr_cccb_begin(&client,&cmd,1,NULL,0,100));
    cmd.payload=&queue;assert(!pvr_cccb_begin(&client,&cmd,1,NULL,0,100));
    cmd=command(24);uint32_t cleanup=1;assert(!pvr_cccb_begin(&client,&cmd,1,&cleanup,1,100));
    assert(!pvr_cccb_begin(&client,&cmd,1,NULL,4,100));
    assert(!pvr_cccb_begin(&client,&cmd,1,NULL,0,now));
    assert(!pvr_cccb_begin(&client,(void *)&client,1,NULL,0,100));
    assert(!memcmp(&client,&saved,sizeof(client)) && io==before);
    struct pvr_cccb empty={0}, copy=empty;
    assert(!pvr_cccb_init(&empty,&ops,&client,&queue,1000,0xc0300000,100));
    assert(!pvr_cccb_init(&empty,&ops,&client,&queue,1024,1,100));
    assert(!memcmp(&empty,&copy,sizeof(empty)) && io==before);
}
int main(void)
{
    reference_cases();session_cases();pressure_cases();failure_cases();refusal_cases();
    puts("PASS GPU client commands: reference ring/KICK bytes, wrap, the eight-job session wrap, pressure, delayed return, KICK consumed without a return word, retained faults and atomic refusals");
}
