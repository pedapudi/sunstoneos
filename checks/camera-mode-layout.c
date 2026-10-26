/* Cheap mode/source/layout coverage; native execution uses representative sizes. */
#include "a733_camera_profile.h"
#include "a733_iommu.h"
#include <assert.h>
#include <stdio.h>
static _Alignas(16384) uint32_t words[A733_IOMMU_LARGE_TABLE_BYTES/4];
int main(void)
{
    struct ov5640_mode mode;
    assert(a733_camera_selected_mode(&mode));
    assert(mode.size==SUNSTONE_CAMERA_SIZE && mode.format==OV5640_UYVY &&
        mode.lanes==2 && mode.xclk_hz==24000000 && mode.pixel_hz && mode.link_hz);
    for(unsigned stream=0;stream<2;++stream){
        uint64_t span=A733_CAMERA_SLOT_BYTES*(stream?2:1),source=A733_CAMERA_TABLE_BYTES+8192+2*span;
        struct a733_iommu_span maps[]={
            {0x10000000,0x48000000+A733_CAMERA_TABLE_BYTES,8192,1},
            {0x10002000,0x48000000+A733_CAMERA_TABLE_BYTES+8192,span,2},
            {0x10002000+span,0x48000000+A733_CAMERA_TABLE_BYTES+8192+span,span,2}};
        struct a733_iommu_tables table={0};
        assert(a733_iommu_build_sized(&table,words,A733_CAMERA_TABLE_BYTES,0x48000000,maps,3)==1);
        uint64_t pa;
        for(unsigned plane=1;plane<3;++plane){
            assert(a733_iommu_translate(&table,maps[plane].iova+span-1,2,&pa)==1);
            assert(pa==maps[plane].physical+span-1);
        }
        assert(a733_iommu_translate(&table,0x10002000+2*span,2,&pa)==-1);
        assert(source+A733_CAMERA_PAGE_ROUND(A733_CAMERA_FRAME_BYTES)<=33554432);
        assert(A733_CAMERA_SLOT_BYTES-A733_CAMERA_NATIVE_PLANE_BYTES>=4096);
        printf("size=%u %ux%u stream=%u source=%llu output=%u total=%llu table=%u\n",
            mode.size,mode.width,mode.height,stream,(unsigned long long)source,A733_CAMERA_FRAME_BYTES,
            (unsigned long long)(source+A733_CAMERA_PAGE_ROUND(A733_CAMERA_FRAME_BYTES)),A733_CAMERA_TABLE_BYTES);
    }
}
