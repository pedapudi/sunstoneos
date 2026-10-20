#ifndef SUNSTONE_HDMI_AUDIO_NATIVE_H
#define SUNSTONE_HDMI_AUDIO_NATIVE_H
#include "a733_hdmi_audio.h"
#include "a733_audio_leases.h"
#include "a733_dma_native.h"
#include "dma_service.h"
#include "a733_memory_uncached.h"
#include "session.h"
struct hdmi_audio_native {
    struct a733_audio_parent parent;
    struct a733_audio_leases leases;
    struct a733_clock_token local;
    struct a733_hdmi_audio sampler;
    struct a733_dma_startup startup;
    struct a733_dma_native dma;
    struct dma_service service;
    struct device_memory_pool *pool;
    struct device_memory_lease memory;
    struct device_memory_extent extent;
    struct device_memory_allocation record;
    struct a733_dma_segment segment;
    struct a733_dma_mapping mapping;
    struct dma_service_job job;
    struct a733_i2s_plan plan;
    uint64_t ticket,last_now,wait_until;
    size_t pcm_span;
    atomic_uint failed;
    unsigned initialized,stage,index,held,published;
};
extern struct hdmi_audio_native audio;
extern struct hdmi_audio_result hdmi_audio_result;
extern atomic_uint hdmi_audio_result_ready;
int hdmi_audio_session_valid(const struct hdmi_audio_session *);
int hdmi_audio_boot(void);
int hdmi_audio_step(void); /*1 input ended and retained,0 pending,-1 retained fault*/
void hdmi_audio_retain(void);
#endif
