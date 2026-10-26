#include "a733_cpu_start.h"
#include <stddef.h>

enum { OFF, CONTROL, MODE,
#ifdef SUNSTONE_BOARD_RESIDENT
    HELD_POLICY, HELD_STATUS,
#endif
    ARCH, ENTRY_LO, ENTRY_HI, ENTRY_LO_CHECK, ENTRY_HI_CHECK, ARCH_WRITE, ARCH_CHECK, PUBLISH,
#ifdef SUNSTONE_BOARD_RESIDENT
    DYNAMIC_RELEASE, DYNAMIC_POLICY, DYNAMIC_ACK,
#endif
    MODE_ON, CONTROL_ON,
    POWER_WAIT, CONTROL_CLEAR, MODE_CLEAR, CONTROL_CHECK, MODE_CHECK, FINISH };
static uintptr_t cfg(unsigned c) {return UINT32_C(0x08001000)+(uintptr_t)c*0x1000;}
static uintptr_t ctrl(unsigned c) {return UINT32_C(0x07050200)+(uintptr_t)c*4;}
static uintptr_t mode(unsigned c) {return UINT32_C(0x07050220)+(uintptr_t)c*4;}
static uintptr_t status(unsigned c) {return UINT32_C(0x07052008)+(uintptr_t)c*0x1000;}
int a733_cpu_index(uint64_t mpidr,unsigned *out)
{
    /* Affinity mask plus architectural U/MT/RES1 flags. */
    if(!out || (mpidr&~UINT64_C(0x000000ffC1ffffff)) ||
        (mpidr&UINT64_C(0x000000ff00ff00ff)) || ((mpidr>>8)&255)>=A733_CPU_COUNT)return 0;
    *out=(unsigned)((mpidr>>8)&255);return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_cpu_start_init(struct a733_cpu_start *s,const struct a733_cpu_start_config *c)
{
    unsigned boot;
    if(!separate(s,sizeof(*s),c,sizeof(*c)) || s->initialized ||
        !c->ops.read32 || !c->ops.write32 || !c->ops.barrier || !c->ops.now_us ||
        !a733_cpu_index(c->boot_mpidr,&boot) || boot!=0 || !c->core || c->core>=A733_CPU_COUNT ||
        !c->lease || !c->deadline || c->entry<UINT32_C(0x40000000) ||
        c->entry>UINT32_MAX || (c->entry&3))return 0;
    *s=(struct a733_cpu_start){.config=*c,.state=A733_CPU_START_PENDING,.initialized=1};return 1;
}
static int fail(struct a733_cpu_start *s,enum a733_cpu_start_error e)
{s->error=e;s->state=A733_CPU_START_RETAINED;return 0;}
static int time_ok(struct a733_cpu_start *s)
{
    uint64_t now;
    if(s->config.ops.now_us(s->config.context,&now)!=1 || now<s->last_now || now>=s->config.deadline)
        return fail(s,A733_CPU_START_TIME);
    s->last_now=now;return 1;
}
static int read_reg(struct a733_cpu_start *s,uintptr_t a,uint32_t *v)
{return s->config.ops.read32(s->config.context,a,v)==1 ? 1 : fail(s,A733_CPU_START_IO);}
static int write_reg(struct a733_cpu_start *s,uintptr_t a,uint32_t v)
{
    ++s->writes_attempted;
    return s->config.ops.write32(s->config.context,a,v)==1 ? 1 : fail(s,A733_CPU_START_IO);
}
enum a733_cpu_start_state a733_cpu_start_poll(struct a733_cpu_start *s)
{
    if(!s || !s->initialized)return A733_CPU_START_RETAINED;
    if(s->entered || s->state!=A733_CPU_START_PENDING)return s->state;
    s->entered=1;
    if(!time_ok(s))goto out;
    unsigned c=s->config.core;uint32_t v=0;int advance=1;
    switch(s->step) {
    case OFF:
        if(!read_reg(s,status(c),&s->power_status))break;
        if(s->power_status&15)fail(s,A733_CPU_START_INITIAL_STATE);
        break;
    case CONTROL:
        if(read_reg(s,ctrl(c),&s->control) && (s->control&1))fail(s,A733_CPU_START_INITIAL_STATE);
        break;
    case MODE:
        if(read_reg(s,mode(c),&s->mode) && (s->mode&1))fail(s,A733_CPU_START_INITIAL_STATE);
        break;
    case ARCH: (void)read_reg(s,cfg(c),&s->architecture);break;
    case ENTRY_LO: (void)write_reg(s,cfg(c)+4,(uint32_t)s->config.entry);break;
    case ENTRY_HI: (void)write_reg(s,cfg(c)+8,0);break;
    case ENTRY_LO_CHECK:
        if(read_reg(s,cfg(c)+4,&v) && v!=(uint32_t)s->config.entry)fail(s,A733_CPU_START_VERIFY);
        break;
    case ENTRY_HI_CHECK:
        if(read_reg(s,cfg(c)+8,&v) && v)fail(s,A733_CPU_START_VERIFY);
        break;
    case ARCH_WRITE: (void)write_reg(s,cfg(c),s->architecture|1);break;
    case ARCH_CHECK:
        if(read_reg(s,cfg(c),&v) && v!=(s->architecture|1))fail(s,A733_CPU_START_VERIFY);
        break;
    case PUBLISH: case FINISH:
        if(s->config.ops.barrier(s->config.context)!=1)fail(s,A733_CPU_START_IO);
        break;
#ifdef SUNSTONE_BOARD_RESIDENT
    /* Admission precedes every startup write, including RVBAR. */
    case HELD_POLICY:
        if(!(s->control&2)) {fail(s,A733_CPU_START_INITIAL_STATE);break;}
        if(read_reg(s,status(c)-8,&v) && v)fail(s,A733_CPU_START_INITIAL_STATE);
        break;
    case HELD_STATUS:
        if(read_reg(s,status(c),&v) && v)fail(s,A733_CPU_START_INITIAL_STATE);
        break;
    /* cpu_entry armed the mailbox before POWER. PUBLISH completed the entry
     * stores; an immediate wake parks until POWERED/request cleanup. */
    case DYNAMIC_RELEASE: (void)write_reg(s,status(c)-8,0x100);break;
    case DYNAMIC_POLICY:
        if(read_reg(s,status(c)-8,&v) && v!=0x100)fail(s,A733_CPU_START_VERIFY);
        break;
    case DYNAMIC_ACK:
        if(read_reg(s,status(c),&v)) {
            if(v&~0x108u)fail(s,A733_CPU_START_VERIFY);
            advance=!!(v&0x100);
        }
        break;
#endif
    case MODE_ON: (void)write_reg(s,mode(c),s->mode|1);break;
    case CONTROL_ON: (void)write_reg(s,ctrl(c),s->control|1);break;
    case POWER_WAIT:
        if(read_reg(s,status(c),&s->power_status))advance=(s->power_status&15)==8;
        break;
    case CONTROL_CLEAR: (void)write_reg(s,ctrl(c),s->control);break;
    case MODE_CLEAR: (void)write_reg(s,mode(c),s->mode);break;
    case CONTROL_CHECK:
        if(read_reg(s,ctrl(c),&v) && v!=s->control)fail(s,A733_CPU_START_VERIFY);
        break;
    case MODE_CHECK:
        if(read_reg(s,mode(c),&v) && v!=s->mode)fail(s,A733_CPU_START_VERIFY);
        break;
    default:fail(s,A733_CPU_START_VERIFY);break;
    }
    if(s->state==A733_CPU_START_PENDING && time_ok(s) && advance) {
        if(s->step==FINISH)s->state=A733_CPU_START_POWERED;
        else ++s->step;
    }
out:
    s->entered=0;return s->state;
}
