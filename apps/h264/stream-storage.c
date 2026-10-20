#include "stream.h"
#ifdef SUNSTONE_H264_FULLHD
#include "a733_memory_reservations.h"
#include "device_memory_pool.h"
#include <string.h>
const uint64_t h264_fullhd_geometry[8]={UINT64_C(0x4844464844323634),1,1920,1088,1920,1080,H264_STREAM_INPUT_BYTES,H264_STREAM_OUTPUT_BYTES};
#ifdef SUNSTONE_H264_HIGH
const uint64_t h264_high_geometry[8]={UINT64_C(0x4847494844323634),1,1920,1088,5,5,14749696,1048576};
#endif
static struct device_memory_pool *pool;
static struct device_memory_lease leases[2+H264_STREAM_COPIES
#ifdef SUNSTONE_H264_HIGH
+1
#endif
];
static uintptr_t pointers[sizeof(leases)/sizeof(*leases)];
static const size_t limits[]={H264_STREAM_INPUT_BYTES,H264_STREAM_OUTPUT_BYTES,
    H264_STREAM_OUTPUT_BYTES,H264_STREAM_OUTPUT_BYTES
#ifdef SUNSTONE_H264_HIGH
    ,H264_STREAM_OUTPUT_BYTES,H264_STREAM_OUTPUT_BYTES,H264_STREAM_OUTPUT_BYTES,H264_HIGH_METADATA_BYTES
#endif
};
int h264_stream_storage_valid(void)
{
    if(!pool||!pool->initialized)return 0;
    for(unsigned i=0;i<sizeof(leases)/sizeof(*leases);++i){
        struct device_memory_extent e;enum device_memory_state state;
        if(!device_memory_describe(pool,leases[i],&e,&state)||
           state!=DEVICE_MEMORY_RETAINED||
           e.cpu!=pointers[i]||e.physical!=e.cpu||e.bytes!=((limits[i]+4095)&~(size_t)4095))return 0;
        const volatile uint8_t *p=(const void *)e.cpu;
        for(size_t j=limits[i];j<e.bytes;++j)if(p[j]!=0xa5)return 0;
    }
    if((uintptr_t)h264_stream.input!=pointers[0]||(uintptr_t)h264_stream.oracle!=pointers[1])return 0;
    for(unsigned i=0;i<H264_STREAM_COPIES;++i)if((uintptr_t)h264_stream.output[i].payload!=pointers[2+i])return 0;
#ifdef SUNSTONE_H264_HIGH
    if((uintptr_t)h264_stream.high!=pointers[2+H264_STREAM_COPIES])return 0;
#endif
    return 1;
}
int h264_stream_storage_init(uint64_t owner)
{
    /* Before scheduler release: the existing extended provider has completed
     * both mapping scans; no USB/controller task can observe partial pointers. */
    if(pool||!owner||!(pool=a733_memory_reservations_init()))return 0;
    for(unsigned i=0;i<sizeof(leases)/sizeof(*leases);++i){
        struct device_memory_extent e;
        if(!device_memory_acquire(pool,owner,limits[i],&leases[i],&e))return 0;
        if(!device_memory_retain(pool,leases[i]))return 0;
        pointers[i]=e.cpu;
        memset((void *)(e.cpu+limits[i]),0xa5,e.bytes-limits[i]);
    }
    h264_stream.input=(void *)pointers[0];h264_stream.oracle=(void *)pointers[1];
    for(unsigned i=0;i<H264_STREAM_COPIES;++i)h264_stream.output[i].payload=(void *)pointers[i+2];

#ifdef SUNSTONE_H264_HIGH
    h264_stream.high=h264_high_init((void *)pointers[2+H264_STREAM_COPIES],H264_HIGH_METADATA_BYTES);if(!h264_stream.high)return 0;
#endif
    return h264_stream_storage_valid();
}
/* Ledger ownership is retained before scheduler release and never changes.
 * AU/output atomics still govern CPU payload access independently. */
void h264_stream_storage_retain(void){}
#else
int h264_stream_storage_init(uint64_t owner){return owner!=0;}
int h264_stream_storage_valid(void){return 1;}
void h264_stream_storage_retain(void){}
#endif
