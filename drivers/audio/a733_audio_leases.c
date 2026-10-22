#include "a733_audio_leases.h"
#include "a733_range.h"

static enum a733_i2s_resource_result fault(struct a733_audio_leases *b)
{b->failed=1;return A733_I2S_RESOURCE_RETAINED;}
static enum a733_i2s_resource_result acquire(void *ctx,enum a733_audio_scope scope,
    struct a733_audio_token *out,uint64_t deadline)
{
    struct a733_audio_leases *b=ctx;
    if(!b || !b->initialized || b->failed || scope<A733_AUDIO_REFERENCE || scope>A733_AUDIO_LOCAL)
        return A733_I2S_RESOURCE_RETAINED;
    if(!a733_ranges_disjoint(b,sizeof(*b),out,sizeof(*out)) ||
        !a733_ranges_disjoint(b->registry,sizeof(*b->registry),out,sizeof(*out)))return fault(b);
    struct a733_clock_token *t=&b->tokens[scope-1];
    if(out->id || t->id)return fault(b); /* No asynchronous registry acquisition. */
    a733_clock_mask_t shared=scope==A733_AUDIO_REFERENCE ? A733_CLOCK_REF :
                    scope==A733_AUDIO_STABLE ? A733_CLOCK_AUDIO5 : 0;
    a733_clock_mask_t exclusive=scope==A733_AUDIO_CHANGE ? A733_CLOCK_AUDIO_CHANGE :
                       scope==A733_AUDIO_LOCAL ? A733_CLOCK_I2S0 : 0;
    enum a733_clock_result r=a733_clock_acquire(b->registry,b->owner,shared,exclusive,deadline,t);
    if(r==A733_CLOCK_BUSY || r==A733_CLOCK_EXPIRED)return A733_I2S_RESOURCE_BUSY;
    if(r!=A733_CLOCK_OK)return fault(b);
    *out=(struct a733_audio_token){t->id,scope};return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result publish(void *ctx,const struct a733_audio_token *old,
    struct a733_audio_token *out,uint64_t deadline)
{
    struct a733_audio_leases *b=ctx;
    if(!b || !b->initialized || b->failed)return A733_I2S_RESOURCE_RETAINED;
    if(!a733_ranges_disjoint(b,sizeof(*b),old,sizeof(*old)) || !a733_ranges_disjoint(b,sizeof(*b),out,sizeof(*out)) ||
        !a733_ranges_disjoint(old,sizeof(*old),out,sizeof(*out)) ||
        !a733_ranges_disjoint(b->registry,sizeof(*b->registry),out,sizeof(*out)) ||
        old->scope!=A733_AUDIO_CHANGE || old->id!=b->tokens[A733_AUDIO_CHANGE-1].id ||
        out->id || b->tokens[A733_AUDIO_STABLE-1].id)return fault(b);
    enum a733_clock_result r=a733_clock_publish_audio(b->registry,&b->tokens[A733_AUDIO_CHANGE-1],
        deadline,&b->tokens[A733_AUDIO_STABLE-1]);
    if(r==A733_CLOCK_BUSY)return A733_I2S_RESOURCE_BUSY;
    if(r!=A733_CLOCK_OK)return fault(b);
    b->tokens[A733_AUDIO_CHANGE-1]=(struct a733_clock_token){0};
    *out=(struct a733_audio_token){b->tokens[A733_AUDIO_STABLE-1].id,A733_AUDIO_STABLE};
    return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result release(void *ctx,const struct a733_audio_token *old,uint64_t deadline)
{
    struct a733_audio_leases *b=ctx;
    if(!b || !b->initialized || b->failed)return A733_I2S_RESOURCE_RETAINED;
    if(!a733_ranges_disjoint(b,sizeof(*b),old,sizeof(*old)) || old->scope!=A733_AUDIO_LOCAL ||
        old->id!=b->tokens[A733_AUDIO_LOCAL-1].id)return fault(b);
    enum a733_clock_result r=a733_clock_release(b->registry,&b->tokens[A733_AUDIO_LOCAL-1],deadline);
    if(r==A733_CLOCK_BUSY)return A733_I2S_RESOURCE_BUSY;
    if(r!=A733_CLOCK_OK)return fault(b);
    b->tokens[A733_AUDIO_LOCAL-1]=(struct a733_clock_token){0};return A733_I2S_RESOURCE_READY;
}
static int enter(void *ctx,uint64_t deadline)
{struct a733_audio_leases *b=ctx;return !b->failed && b->backend.enter(b->context,deadline);}
static void leave(void *ctx)
{struct a733_audio_leases *b=ctx;b->backend.leave(b->context);}
static int now(void *ctx,uint64_t *out)
{struct a733_audio_leases *b=ctx;return !b->failed && b->backend.now_us(b->context,out);}
static int read_reg(void *ctx,uintptr_t a,uint32_t *v)
{struct a733_audio_leases *b=ctx;return !b->failed && b->backend.read(b->context,a,v);}
static int write_reg(void *ctx,uintptr_t a,uint32_t v)
{struct a733_audio_leases *b=ctx;return !b->failed && b->backend.write(b->context,a,v);}
int a733_audio_leases_init(struct a733_audio_leases *b,struct a733_clock_leases *m,
    uint64_t owner,const struct a733_audio_operations *ops,void *ctx)
{
    if(!a733_ranges_disjoint(b,sizeof(*b),m,sizeof(*m)) || !a733_ranges_disjoint(b,sizeof(*b),ops,sizeof(*ops)) ||
        !m->initialized || b->initialized || !owner || !ops->enter || !ops->leave ||
        !ops->now_us || !ops->read || !ops->write)return 0;
    *b=(struct a733_audio_leases){.registry=m,.backend=*ops,.context=ctx,.owner=owner,.initialized=1,
        .operations={acquire,publish,release,enter,leave,now,read_reg,write_reg}};
    return 1;
}
