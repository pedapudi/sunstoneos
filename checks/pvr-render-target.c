#include "reference.h"
#include "pvr_render_target.h"
#include <stdio.h>

static void compare(uint32_t width, uint32_t height, uint32_t maximum)
{
    struct pvr_render_profile profile = {width, height, maximum};
    struct pvr_render_requirements r;
    assert(pvr_render_target_require(&profile, &r));
    struct pvr_rt_mtile_info mi;
    struct pvr_device_info info = {0};
    assert(rogue_get_render_size_max(&info) == 8192);
    struct pvr_physical_device physical = {info};
    struct pvr_device device = {&physical};
    pvr_arch_rt_mtile_info_init(&info, &mi, width, height, 1);
    uint64_t stride, bytes;
    pvr_rt_get_tail_ptr_stride_size(&device, &mi, 1, &stride, &bytes);
    assert(!stride && bytes == r.bytes[PVR_RENDER_TPC]);
    pvr_rt_get_region_headers_stride_size(&device, &mi, 1, &stride, &bytes);
    assert(stride == bytes && bytes == r.bytes[PVR_RENDER_REGION0]);
    struct pvr_free_list local = {.size = 28 * 4096}, global = {.size = maximum * 4096};
    assert(pvr_rt_get_mlist_size(&global, &local) == r.bytes[PVR_RENDER_MLIST0]);
    assert(pvr_rt_get_isp_region_size(&device, &mi) == r.region_headers);
    _Alignas(64) unsigned char common[88], data[2][384];
    struct pvr_render_target target = {.common = common, .data = {data[0], data[1]},
        .common_firmware = 0xc0900080, .data_firmware = {0xc0900100, 0xc0900280},
        .local_firmware = 0xc0900000, .global_firmware = 0xc0900040};
    uint64_t offset = 0;
    for (unsigned i = 0; i < PVR_RENDER_REGIONS; ++i) {
        target.gpu[i] = UINT64_C(0x8004000000) + offset; offset += r.allocation[i];
    }
    assert(offset == r.payload_bytes && pvr_render_target_build(&profile, &target));
    struct pvr_fw_object local_object = {.address = target.local_firmware}, global_object = {.address = target.global_firmware};
    local.fw_obj = &local_object; global.fw_obj = &global_object;
    struct pvr_hwrt_dataset hwrt = {.free_lists = {&local, &global}};
    struct pvr_file file = {&device};
    struct drm_pvr_ioctl_create_hwrt_dataset_args args = {.width = width, .height = height, .samples = 1, .layers = 1,
        .region_header_size = r.region_headers,
        .geom_data_args = {.tpc_dev_addr = target.gpu[PVR_RENDER_TPC], .tpc_size = r.bytes[PVR_RENDER_TPC],
                          .vheap_table_dev_addr = target.gpu[PVR_RENDER_VHEAP]}};
    reference_address = target.common_firmware;
    assert(!hwrt_init_common_fw_structure(&file, &args, &hwrt));
    assert(sizeof(hwrt.common) == sizeof(common) && !memcmp(common, &hwrt.common, sizeof(common)));
    for (unsigned i = 0; i < 2; ++i) {
        struct pvr_hwrt_data rt = {0};
        struct drm_pvr_create_hwrt_rt_data_args rt_args = {.pm_mlist_dev_addr = target.gpu[PVR_RENDER_MLIST0 + i],
            .region_header_dev_addr = target.gpu[PVR_RENDER_REGION0 + i]};
        reference_address = target.data_firmware[i];
        assert(!hwrt_data_init_fw_structure(&file, &hwrt, &args, &rt_args, &rt));
        assert(sizeof(rt.data) == sizeof(data[i]) && !memcmp(data[i], &rt.data, sizeof(data[i])));
        pvr_fw_object_destroy(rt.fw_obj);
    }
    pvr_fw_object_destroy(hwrt.common_fw_obj);
    unsigned char old[88]; memcpy(old, common, 88);
    target.gpu[PVR_RENDER_TPC] = target.gpu[PVR_RENDER_VHEAP];
    assert(!pvr_render_target_build(&profile, &target) && !memcmp(old, common, 88));
}
int main(void)
{
    compare(1, 1, 28); compare(64, 64, 32); compare(3840, 2160, 16384); compare(8192, 8192, 16384);
    struct pvr_render_requirements untouched, old;
    memset(&untouched, 0xa5, sizeof(untouched)); old = untouched;
    struct pvr_render_profile invalid = {8193, 1, 28};
    assert(!pvr_render_target_require(&invalid, &untouched) && !memcmp(&old, &untouched, sizeof(old)));
    puts("PASS render target: pinned common/HWRT initializers, Mesa geometry sizing and bounded refusal");
}
