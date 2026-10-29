/* This binding is private to the HDMI application. Publishing stream_owner
 * transfers every I2S3 access to the existing DMA worker. */
static int stream_time(uint64_t deadline)
{
    uint64_t t;
    return deadline && deadline<=app.stop_at && now(&audio,&t) && t<deadline;
}
static enum a733_i2s_resource_result stream_health(void *v,uint64_t deadline)
{
    if(v!=&audio || !atomic_load_explicit(&audio.stream_owner,memory_order_acquire) ||
       !current() || !stream_time(deadline) || !a733_audio_parent_ready(&audio.parent))return A733_I2S_RESOURCE_RETAINED;
    enum a733_clock_result r=a733_clock_token_status(&app.clocks,&audio.local);
    if(!stream_time(deadline))return A733_I2S_RESOURCE_RETAINED;
    if(r==A733_CLOCK_BUSY)return A733_I2S_RESOURCE_BUSY;
    if(r!=A733_CLOCK_OK || audio.local.owner!=app.session.audio.authority ||
       audio.local.exclusive!=A733_CLOCK_I2S3 ||
       audio.local.shared!=(A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_AUDIO1|A733_CLOCK_AUDIO5))return A733_I2S_RESOURCE_RETAINED;
    audio.stream_deadline=deadline;return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result stream_acquire_poll(void *v,uint64_t deadline)
{
    enum a733_i2s_resource_result h=stream_health(v,deadline);
    if(h!=A733_I2S_RESOURCE_READY)return h;
    int r=i2s_prepare();
    if(!stream_time(deadline)||r<0)return A733_I2S_RESOURCE_RETAINED;
    return r?A733_I2S_RESOURCE_READY:A733_I2S_RESOURCE_PENDING;
}
static enum a733_i2s_resource_result stream_acquire(void *v,const struct a733_i2s_plan *p,uint64_t deadline)
{
    if(!p || p->pb_mask || p->format.module_hz!=24576000 || p->format.rate_hz!=48000 ||
       p->format.channels!=2 || p->format.container_bits!=16 || p->format.slot_bits!=32 ||
       p->format.mclk_hz || p->format.first_slot || audio.index)return A733_I2S_RESOURCE_RETAINED;
    return stream_acquire_poll(v,deadline);
}
static enum a733_i2s_resource_result stream_release(void *v,uint64_t deadline)
{(void)v;(void)deadline;return A733_I2S_RESOURCE_RETAINED;}
static int stream_register(void *v,uintptr_t a,uint32_t *value,int writing)
{
    if(v!=&audio || !value || a<I2S || a>=I2S+0x80 || !stream_time(audio.stream_deadline))return 0;
    if(enter(&audio,audio.stream_deadline)!=1)return 0;
    uint32_t got=0;
    int ok=writing?wr(a,*value):rd(a,value);
    if(ok&&writing&&a!=I2S+12){
        uint32_t ignore=a==I2S+20?(3u<<24):0;
        ok=rd(a,&got)&&((got^*value)&~ignore)==0;
    }
    if(!ok || !stream_time(audio.stream_deadline)){
        /* Keep an actually acquired mutex on uncertain access. */
        (void)a733_platform_lock_retain(&app.locks,A733_PLATFORM_REGISTER_LOCK);
        atomic_store_explicit(&audio.failed,1,memory_order_release);return 0;
    }
    leave(&audio);return stream_time(audio.stream_deadline);
}
static int stream_read(void *v,uintptr_t a,uint32_t *value)
{return stream_register(v,a,value,0);}
static int stream_write(void *v,uintptr_t a,uint32_t value)
{return stream_register(v,a,&value,1);}
static struct a733_i2s_stream_operations stream_operations(void)
{
    return (struct a733_i2s_stream_operations){
        {stream_acquire,stream_acquire_poll,stream_release,now,stream_read,stream_write},stream_health};
}
int hdmi_audio_stream_request(enum dma_service_operation op,uint64_t timeout,uint64_t period,struct dma_service_reply *reply)
{
    uint64_t t;
    if(xTaskGetCurrentTaskHandle()!=app.handles[1] || !hdmi_live(NULL) ||
       atomic_load_explicit(&audio.failed,memory_order_acquire) ||
       !a733_counter_clock_now(&app.counter,&t) || t>=app.stop_at)return -1;
    if(!stream.request_until){
        if(t>UINT64_MAX-100000)return -1;
        stream.request_until=t+100000;if(stream.request_until>app.stop_at)stream.request_until=app.stop_at;
        stream.request_op=op;
    }
    if(stream.request_op!=(unsigned)op || t>=stream.request_until)return -1;
    if(!stream.request_ticket){
        const struct dma_service_request r={.operation=op,.job=0,.timeout=timeout,.period_ticket=period};
        if(!dma_service_send(&audio.service,&r,0,&stream.request_ticket))return 0;
        stream.request_op=op;
    }
    if(stream.request_op!=(unsigned)op || t>=stream.request_until)return -1;
    if(!dma_service_collect(&audio.service,stream.request_ticket,reply,0))return 0;
    if(!a733_counter_clock_now(&app.counter,&t) || t>=stream.request_until || !hdmi_live(NULL) ||
       atomic_load_explicit(&audio.failed,memory_order_acquire))return -1;
    stream.request_ticket=0;
    if(reply->result!=DMA_SERVICE_BUSY)stream.request_until=0;
    return reply->result==DMA_SERVICE_OK?1:reply->result==DMA_SERVICE_BUSY?0:-1;
}
int hdmi_audio_stream_publish(unsigned stage)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[1])return -1;
    if(a733_hdmi_audio_mute(&audio.sampler,1)!=1)return fail();
    hdmi_audio_result.source_consumed_bytes=atomic_load_explicit(&stream.consumed,memory_order_acquire)*4;
    hdmi_audio_result.dma_result=stage==HDMI_AUDIO_INPUT_ENDED_RETAINED?DMA_SERVICE_COMPLETE:DMA_SERVICE_CANCELED;
    hdmi_audio_result.fifo_empty=stage==HDMI_AUDIO_INPUT_ENDED_RETAINED;
    return publish(stage);
}
int hdmi_audio_step(void)
{
    if(audio.published)return atomic_load_explicit(&audio.failed,memory_order_acquire)?-1:1;
    uint64_t t;
    if(xTaskGetCurrentTaskHandle()!=app.handles[1] || !hdmi_live(NULL) ||
       !a733_counter_clock_now(&app.counter,&t) || t>=app.stop_at || !parent_memory())return fail();
    int r;
    switch(audio.stage){
    case 0:
        r=a733_dma_startup_poll(&audio.startup);
        if(r==A733_DMA_PLATFORM_RETAINED)return fail();
        if(r!=A733_DMA_PLATFORM_READY)return 0;
        r=dma_service_activate(&audio.service);if(r==DMA_SERVICE_BUSY)return 0;
        if(r!=DMA_SERVICE_OK)return fail();audio.stage=1;return 0;
    case 1:
        r=a733_audio_parent_poll(&audio.parent);
        if(r==A733_I2S_RESOURCE_RETAINED)return fail();
        if(r!=A733_I2S_RESOURCE_READY)return 0;audio.stage=2;return 0;
    case 2:
        r=a733_clock_acquire(&app.clocks,app.session.audio.authority,A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_AUDIO5,
            A733_CLOCK_I2S3,app.stop_at,&audio.local);
        if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return fail();
        atomic_store_explicit(&audio.stream_owner,1,memory_order_release);
        audio.stage=3;return 0;
    case 3:
        r=a733_hdmi_audio_prepare(&audio.sampler);if(r<0)return fail();if(!r)return 0;
        audio.stage=4;return 0;
    case 4:
        return hdmi_stream_step();
    default:return fail();
    }
}
