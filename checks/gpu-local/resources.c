/* SPDX-License-Identifier: MIT */
int fixed_fixture_main(int,char **);
#define main fixed_fixture_main
#include "../pvr-compute-job.c"
#undef main
#include "gpu_queue.h"
int main(void)
{
    struct gpu_queue_program p={.abi=GPU_QUEUE_ABI_GRID,.coefficients=3,.local_size={4,8,1}};
    assert(pvr_compute_job_common_words(&p)==3);
    p.local_words=1;assert(!pvr_compute_job_common_words(&p));p.local_words=0;
    p.barriers=1;assert(!pvr_compute_job_common_words(&p));p.barriers=0;
    p.coefficients=7;assert(pvr_compute_job_common_words(&p)==7);
    p.abi=GPU_QUEUE_ABI_LOCAL;assert(!pvr_compute_job_common_words(&p));
    p.local_words=96;p.barriers=1;assert(pvr_compute_job_common_words(&p)==103);
    p.coefficients=3;assert(pvr_compute_job_common_words(&p)==99);
    p.local_words=240;assert(pvr_compute_job_common_words(&p)==243);
    p.local_words=241;assert(!pvr_compute_job_common_words(&p));p.local_words=96;
    p.barriers=2;assert(!pvr_compute_job_common_words(&p));p.barriers=1;
    p.coefficients=4;assert(!pvr_compute_job_common_words(&p));p.coefficients=3;
    p.local_size[2]=2;assert(!pvr_compute_job_common_words(&p));p.local_size[2]=1;
    p.local_size[0]=0;assert(!pvr_compute_job_common_words(&p));p.local_size[0]=4;
    p.local_size[1]=9;assert(!pvr_compute_job_common_words(&p));p.local_size[1]=8;
    unsigned char stream[64],saved[64],legacy[64];const uint32_t groups[3]={3,2,1};
    memset(stream,0xa5,sizeof(stream));memcpy(saved,stream,sizeof(stream));
    assert(!pvr_compute_job_grid_cdm_resources(stream,0x8000,0x8400,groups,0));
    assert(!memcmp(stream,saved,sizeof(stream)));
    assert(!pvr_compute_job_grid_cdm_resources(stream,0x8000,0x8400,groups,248));
    assert(!memcmp(stream,saved,sizeof(stream)));
    memset(stream,0,sizeof(stream));memset(legacy,0,sizeof(legacy));
    pvr_compute_job_grid_cdm(legacy,0x8000,0x8400,groups);
    assert(pvr_compute_job_grid_cdm_resources(stream,0x8000,0x8400,groups,3));
    assert(!memcmp(stream,legacy,sizeof(stream)));
    for(unsigned common=1;common<=247;++common) {
        memset(stream,0,sizeof(stream));assert(pvr_compute_job_grid_cdm_resources(stream,0x8000,0x8400,groups,common));
        assert(!(get(stream,4)&2)); /* Coefficient bank, per workgroup. */
#ifdef UPSTREAM
        uint32_t expected[16]={0};
        struct pvr_csb csb={expected};struct pvr_sub_cmd_compute sub={0};
        struct pvr_compute_kernel_info work={.usc_unified_size=2,.usc_common_size=(common+15)/16,
            .pds_temp_size=1,.pds_data_size=7,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,
            .pds_data_offset=0x8000,.pds_code_offset=0x8080,
            .global_size={3,2,1},.local_size={32,1,1},.max_instances=32};
        pvr_compute_generate_control_stream(&csb,&sub,&work);
        struct pvr_compute_kernel_info fence={.pds_data_size=1,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,
            .is_fence=true,.pds_data_offset=0x8400,.pds_code_offset=0x8410,.sd_type=ROGUE_CDMCTRL_SD_TYPE_PDS,
            .usc_common_shared=true,.global_size={1,1,1},.local_size={1,1,1},
            .max_instances=ROGUE_CDM_MAX_PACKED_WORKGROUPS_PER_TASK};
        pvr_compute_generate_control_stream(&csb,&sub,&fence);pvr_csb_emit(&csb,CDMCTRL_STREAM_TERMINATE,term){}
        assert(csb.cursor-expected==15&&!memcmp(stream,expected,sizeof(stream)));
#endif
    }
    puts("PASS descriptor refusals, legacy bytes, immutable refusal and all common allocations");return 0;
}
