/* SPDX-License-Identifier: MIT */
#include "reference.h"
#include "pvr_render_job.h"
#include "pvr_render_draw.h"
#include <stdio.h>
int main(void)
{
    struct pvr_render_program_image programs={0};
    struct pvr_render_job_image first,job;
    assert(pvr_render_job_build(&programs,0xc0900100,0xc0a00580,0xc0a00584,&first));
    const uint32_t sequences[]={1,2,3,12,PVR_RENDER_DRAW_MAX_FRAMES};
    for(unsigned i=0;i<sizeof(sequences)/sizeof(sequences[0]);++i) {
        uint32_t n=sequences[i],hwrt=0xc0900100+((n-1)&1)*384;
        assert(pvr_render_job_build_frame(&programs,hwrt,0xc0a00580,0xc0a00584,n,&job));
        struct rogue_fwif_cmd_geom geometry;struct rogue_fwif_cmd_frag fragment;
        memcpy(&geometry,first.geometry,sizeof(geometry));memcpy(&fragment,first.fragment,sizeof(fragment));
        geometry.cmd_shared.hwrt_data_fw_addr=fragment.cmd_shared.hwrt_data_fw_addr=hwrt;
        geometry.partial_render_geom_frag_fence.value=n-1;
        assert(geometry.flags==(ROGUE_GEOM_FLAGS_FIRSTKICK|ROGUE_GEOM_FLAGS_LASTKICK|ROGUE_GEOM_FLAGS_SINGLE_CORE));
        assert(!memcmp(&geometry,job.geometry,sizeof(geometry)) && !memcmp(&fragment,job.fragment,sizeof(fragment)));
        struct rogue_fwif_ufo gu={.addr=0xc0a00580,.value=n},fu={.addr=0xc0a00584,.value=n};
        assert(!memcmp(&gu,job.geometry_update,8) && !memcmp(&gu,job.fragment_fence,8) && !memcmp(&fu,job.fragment_update,8));
    }
    struct pvr_render_job_image saved=job;
    assert(!pvr_render_job_build_frame(&programs,0xc0900100,0xc0a00580,0xc0a00584,0,&job));
    assert(!memcmp(&saved,&job,sizeof(job)));
    assert(!pvr_render_job_build_frame(&programs,0xc0900100,0xc0a00580,0xc0a00584,PVR_RENDER_DRAW_MAX_FRAMES+1,&job));
    assert(!memcmp(&saved,&job,sizeof(job)));
    puts("PASS repeated draw reference: pinned firmware structures, complete frame flags, alternating HWRT, n/n-1 and nonwrap bounds");
    return 0;
}
