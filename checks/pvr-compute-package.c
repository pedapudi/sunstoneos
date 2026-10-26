int fixed_fixture_main(int,char **);
#define main fixed_fixture_main
#include "pvr-compute-job.c"
/* A 64-bit value with its 32-bit halves exchanged. */
#define HALVES(v) ((uint64_t)(v)<<32|(uint64_t)(v)>>32)
#undef main
#ifdef UPSTREAM
#include "pvr_rogue_pds_encode.h"
#endif
static struct pvr_compute_package package;
static void load(const char *path,void *p,size_t n)
{FILE *f=fopen(path,"rb");assert(f&&fread(p,n,1,f)==1&&fgetc(f)==EOF);assert(!fclose(f));}
int main(int argc,char **argv)
{
    assert(argc==3||argc==4);load(argv[1],&package,sizeof(package));assert(pvr_compute_package_valid(&package));
    setup(argc==4?(unsigned)strtoul(argv[3],NULL,0):0);config.package=&package;
    if(package.profile>=2)config.buffers[PVR_JOB_FENCE_PDS].gpu=programs.pds.base+0x8000;
    for(unsigned i=0;i<PVR_JOB_REGIONS;++i)config.buffers[i].bytes=pvr_compute_job_region_bytes(&config,i);
    assert(pvr_compute_job_build(&config,&built));
    assert(!memcmp(config.buffers[PVR_JOB_USC].cpu,pvr_compute_package_code(&package),package.code_bytes));
    for(unsigned i=package.code_bytes;i<4096;++i)assert(!((unsigned char *)config.buffers[PVR_JOB_USC].cpu)[i]);
    assert(!memcmp(config.buffers[PVR_JOB_INPUT].cpu,package.input,package.input_bytes));
#ifdef UPSTREAM
    const struct pvr_device_info *dev=&pvr_device_info_36_56_104_183;
    if(package.profile==3) {
        uint32_t pds[1024]={0},raw[1024]={0},code[1024]={0},stream[1024]={0};
        struct pvr_pds_compute_shader_program p;pvr_pds_compute_shader_program_init(&p);
        p.local_input_regs[0]=4;p.flattened_work_groups=true;p.kick_usc=true;
        for(unsigned i=0;i<3;++i){p.work_group_input_regs[i]=i;p.num_work_groups_regs[i]=4+i;}
        pvr_pds_setup_doutu(&p.usc_task_control,config.buffers[0].gpu-programs.usc.base,package.precomp[0],0,false);
        pvr_pds_compute_shader(&p,NULL,PDS_GENERATE_SIZES,dev);
        assert(p.data_size==18&&p.code_size==52&&p.temps_used==3&&p.highest_temp==3&&p.coeff_update_task_branch_size==10);
        unsigned ordinary_entry=p.coeff_update_task_branch_size;
        pvr_pds_compute_shader(&p,pds,PDS_GENERATE_DATA_SEGMENT,dev);
        uint32_t *end=pvr_pds_compute_shader(&p,raw,PDS_GENERATE_CODE_SEGMENT,dev);
        assert(end-raw==13); /* code_size reports bytes; end pointer is authoritative. */
        for(unsigned i=0;i<3;++i){
            assert(p.base_workgroup_constant_offset_in_dwords[i]==8+i);
            assert(p.num_workgroups_constant_offset_in_dwords[i]==(i==2?14u:12u));
            pds[12+i]=package.grid.groups[i];
        }
        assert(p.num_workgroups_indirect_src==16&&p.num_workgroups_indirect_src_dma==6);
        /* Halves exchanged: Mesa's DOUTW generator stores the high word
         * first, and DOUTW writes word 0 to the even (low) register
         * (pvr_compute_job.c doutw_pair). */
        struct pvr_pds_doutw_control pointers={.dest_store=PDS_UNIFIED_STORE,.num_const64=2,
            .doutw_data={(uint64_t)config.buffers[PVR_JOB_INPUT].gpu<<32|config.buffers[PVR_JOB_INPUT].gpu>>32,
                (uint64_t)(config.buffers[PVR_JOB_OUTPUT].gpu+64)<<32|(config.buffers[PVR_JOB_OUTPUT].gpu+64)>>32}};
        pvr_pds_generate_doutw(&pointers,pds+20,PDS_GENERATE_DATA_SEGMENT,dev);
        assert(pointers.data_size==8);
        pds[26]&=~PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTW_SRC1_LAST_EN;
        memcpy(code,raw,10*4);
        code[10]=pvr_pds_inst_encode_dout(0,0,22,10,PVR_ROGUE_PDSINST_DSTDOUT_DOUTW);
        code[11]=pvr_pds_inst_encode_dout(0,0,26,12,PVR_ROGUE_PDSINST_DSTDOUT_DOUTW);
        memcpy(code+12,raw+10,3*4);memcpy(pds+32,code,15*4);
        assert(code[0]==raw[0]&&code[9]==pvr_pds_inst_encode_halt(0)&&code[12]==raw[10]&&code[13]==raw[11]);
        /* Both entry paths: coefficient initialization halts before pointers;
         * ordinary branch targets both pointer writes then local-index/DOUTU. */
        unsigned writes[2]={0};
        for(unsigned coefficient=0;coefficient<2;++coefficient) {
            unsigned pc=coefficient?1:ordinary_entry;
            for(;pc<15;++pc){if(pc==10||pc==11)++writes[coefficient];if(code[pc]==pvr_pds_inst_encode_halt(0))break;}
        }
        assert(writes[0]==2&&!writes[1]);
        struct pvr_csb csb={stream};struct pvr_sub_cmd_compute sub={0};
        uint32_t a=(uint32_t)(config.buffers[PVR_JOB_PDS].gpu-programs.pds.base);
        uint32_t b=(uint32_t)(config.buffers[PVR_JOB_FENCE_PDS].gpu-programs.pds.base);
        struct pvr_compute_kernel_info work={.usc_unified_size=2,.usc_common_size=1,.pds_temp_size=1,.pds_data_size=7,
            .usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,.pds_data_offset=a,.pds_code_offset=a+128,
            .global_size={package.grid.groups[0],package.grid.groups[1],1},.local_size={32,1,1},.max_instances=32};
        pvr_compute_generate_control_stream(&csb,&sub,&work);
        struct pvr_compute_kernel_info fence={.pds_data_size=1,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,.is_fence=true,
            .pds_data_offset=b,.pds_code_offset=b+16,.sd_type=ROGUE_CDMCTRL_SD_TYPE_PDS,.usc_common_shared=true,
            .global_size={1,1,1},.local_size={1,1,1},.max_instances=ROGUE_CDM_MAX_PACKED_WORKGROUPS_PER_TASK};
        pvr_compute_generate_control_stream(&csb,&sub,&fence);pvr_csb_emit(&csb,CDMCTRL_STREAM_TERMINATE,term){}
        assert(csb.cursor-stream==15&&!memcmp(stream,config.buffers[PVR_JOB_CDM].cpu,sizeof(stream)));
        assert(!memcmp(pds,config.buffers[PVR_JOB_PDS].cpu,sizeof(pds)));
        puts("PASS grid PDS entries, compiler sysval allocations, emitted end13 and complete CDM bytes");return 0;
    }
    if(package.profile==2) {
        uint32_t pds[1024]={0},stream[1024]={0};
        struct pvr_csb csb={stream};struct pvr_sub_cmd_compute sub={0};
        uint32_t a=(uint32_t)(config.buffers[PVR_JOB_PDS].gpu-programs.pds.base);
        uint32_t b=(uint32_t)(config.buffers[PVR_JOB_FENCE_PDS].gpu-programs.pds.base);
        for(unsigned group=0;group<package.dispatch_count;++group) {
            unsigned first=group*32,count=package.count-first;
            if(count>32)count=32;
            uint32_t code[16]={0};
            /* Halves exchanged: Mesa's save-restore generator stores the high
             * word first, and DOUTW writes word 0 to the even (low) register
             * (pvr_compute_job.c doutw_pair). */
            struct pvr_pds_shared_storing_program p={.cc_enable=true,.doutw_control={.dest_store=PDS_UNIFIED_STORE,
                .num_const64=3,.doutw_data={HALVES(config.buffers[PVR_JOB_INPUT].gpu),
                    HALVES(config.buffers[PVR_JOB_OUTPUT].gpu+64),HALVES(((uint64_t)count<<32)|first)}}};
            pvr_pds_setup_doutu(&p.usc_task.usc_task_control,config.buffers[0].gpu-programs.usc.base,package.precomp[0],0,false);
            pvr_pds_generate_shared_storing_program(&p,pds+group*32,PDS_GENERATE_DATA_SEGMENT,dev);
            uint32_t *end=pvr_pds_generate_compute_barrier_conditional(code,PDS_GENERATE_CODE_SEGMENT);
            end=pvr_pds_generate_shared_storing_program(&p,end,PDS_GENERATE_CODE_SEGMENT,dev);
            assert(p.data_size==20&&end-code==7);
            memcpy(pds+PVR_COMPUTE_DISPATCH_PDS_CODE/4,code,28);
            struct pvr_compute_kernel_info work={.usc_unified_size=2,.pds_data_size=5,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,
                .pds_data_offset=a+group*128,.pds_code_offset=a+PVR_COMPUTE_DISPATCH_PDS_CODE,
                .global_size={1,1,1},.local_size={32,1,1},.max_instances=32};
            pvr_compute_generate_control_stream(&csb,&sub,&work);
        }
        struct pvr_compute_kernel_info fence={.pds_data_size=1,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,.is_fence=true,
            .pds_data_offset=b,.pds_code_offset=b+16,.sd_type=ROGUE_CDMCTRL_SD_TYPE_PDS,.usc_common_shared=true,
            .global_size={1,1,1},.local_size={1,1,1},.max_instances=ROGUE_CDM_MAX_PACKED_WORKGROUPS_PER_TASK};
        pvr_compute_generate_control_stream(&csb,&sub,&fence);
        pvr_csb_emit(&csb,CDMCTRL_STREAM_TERMINATE,term){}
        assert(csb.cursor-stream==(ptrdiff_t)(package.dispatch_count*7+8));
        assert(!memcmp(pds,config.buffers[PVR_JOB_PDS].cpu,sizeof(pds)));
        assert(!memcmp(stream,config.buffers[PVR_JOB_CDM].cpu,sizeof(stream)));
        puts("PASS all multidispatch PDS/CDM bytes and final fence match upstream");return 0;
    }
    uint32_t data[128]={0};
    /* Halves exchanged, as above (pvr_compute_job.c doutw_pair). */
    struct pvr_pds_shared_storing_program p={.cc_enable=true,.doutw_control={.dest_store=PDS_UNIFIED_STORE,
      .num_const64=2,.doutw_data={HALVES(config.buffers[PVR_JOB_INPUT].gpu),HALVES(config.buffers[PVR_JOB_OUTPUT].gpu+64)}}};
    pvr_pds_setup_doutu(&p.usc_task.usc_task_control,config.buffers[0].gpu-programs.usc.base,package.precomp[0],0,false);
    pvr_pds_generate_shared_storing_program(&p,data,PDS_GENERATE_DATA_SEGMENT,dev);
    uint32_t *end=pvr_pds_generate_compute_barrier_conditional(data+ALIGN_POT(p.data_size,4),PDS_GENERATE_CODE_SEGMENT);
    end=pvr_pds_generate_shared_storing_program(&p,end,PDS_GENERATE_CODE_SEGMENT,dev);
    assert(end-data==22&&!memcmp(data,config.buffers[PVR_JOB_PDS].cpu,88));
    uint32_t stream[64]={0};struct pvr_csb csb={stream};struct pvr_sub_cmd_compute sub={0};
    uint32_t a=(uint32_t)(config.buffers[PVR_JOB_PDS].gpu-programs.pds.base);
    struct pvr_compute_kernel_info work={.usc_unified_size=1,.pds_data_size=4,.usc_target=ROGUE_CDMCTRL_USC_TARGET_ANY,
      .pds_data_offset=a,.pds_code_offset=a+64,.global_size={1,1,1},.local_size={32,1,1},.max_instances=32};
    pvr_compute_generate_control_stream(&csb,&sub,&work);
    assert(csb.cursor-stream==7&&!memcmp(stream,config.buffers[PVR_JOB_CDM].cpu,28));
    puts("PASS package PDS/CDM upstream metadata-derived byte comparison");
#else
    uint32_t expected[PVR_COMPUTE_PACKAGE_OUTPUT/4];
    load(argv[2],expected,package.output_bytes);
    /* Firmware/GPU writes are modeled. The compiler-produced USC is not run. */
    runtime();until(PVR_JOB_WAIT_FENCE);
    if(package.profile==3) {
        unsigned covered=0;unsigned char seen[992]={0};
        for(unsigned gy=0;gy<package.grid.groups[1];++gy)for(unsigned gx=0;gx<package.grid.groups[0];++gx) {
            for(unsigned lane=0;lane<32;++lane) {
                unsigned x=gx*package.local_size[0]+lane%package.local_size[0];
                unsigned y=gy*package.local_size[1]+lane/package.local_size[0];
                if(x>=package.grid.global_extent[0]||y>=package.grid.global_extent[1])continue;
                unsigned i=y*package.grid.global_extent[0]+x;assert(i<package.count&&!seen[i]);seen[i]=1;++covered;
                memcpy((unsigned char *)config.buffers[PVR_JOB_OUTPUT].cpu+64+4*i,expected+i,4);
            }
            assert(tick()==0&&job.phase==PVR_JOB_WAIT_FENCE);
        }
        assert(covered==package.count&&kicks==1);
    } else if(package.profile==2) {
        unsigned covered=0;
        for(unsigned group=0;group<package.dispatch_count;++group) {
            size_t slot=(size_t)group*128;
            unsigned first=get(config.buffers[PVR_JOB_PDS].cpu,slot+32);
            unsigned count=get(config.buffers[PVR_JOB_PDS].cpu,slot+36);
            assert(first==covered&&count>0&&count<=32&&count<=package.count-first);
            assert(first*package.output_element_bytes<=package.output_bytes&&
                count*package.output_element_bytes<=package.output_bytes-first*package.output_element_bytes);
            memcpy((unsigned char *)config.buffers[PVR_JOB_OUTPUT].cpu+64+first*package.output_element_bytes,
                (unsigned char *)expected+first*package.output_element_bytes,count*package.output_element_bytes);
            covered+=count;
            /* Partial workgroups do not advance the single final timeline. */
            assert(tick()==0&&job.phase==PVR_JOB_WAIT_FENCE);
        }
        assert(covered==package.count&&kicks==1);
    } else memcpy((unsigned char *)config.buffers[PVR_JOB_OUTPUT].cpu+64,expected,package.output_bytes);
    put(config.buffers[PVR_JOB_UFO].cpu,0,1);assert(tick()==0);assert(tick()==1);
    assert(!memcmp(job.results,expected,package.output_bytes));
    unsigned before=package.count;package.count=UINT32_MAX;assert(!pvr_compute_package_valid(&package));package.count=before;
    ++package.input_bytes;assert(!pvr_compute_package_valid(&package));--package.input_bytes;
    ++package.output_bytes;assert(!pvr_compute_package_valid(&package));--package.output_bytes;
    unsigned coefficient=package.precomp[2];package.precomp[2]=1;assert(!pvr_compute_package_valid(&package));package.precomp[2]=(unsigned char)coefficient;
    if(package.profile==3) {
        struct pvr_compute_package bad=package;
        ++bad.grid.groups[0];assert(!pvr_compute_package_valid(&bad));bad=package;
        bad.grid.global_extent[0]=UINT32_MAX;assert(!pvr_compute_package_valid(&bad));bad=package;
        bad.grid.reserved=1;assert(!pvr_compute_package_valid(&bad));bad=package;
        bad.grid.sysval_abi=2;assert(!pvr_compute_package_valid(&bad));bad=package;
        bad.version=2;assert(!pvr_compute_package_valid(&bad));bad=package;
        bad.profile=2;assert(!pvr_compute_package_valid(&bad));bad=package;
        bad.local_size[0]=7;assert(!pvr_compute_package_valid(&bad));bad=package;
        put(bad.input,4,package.grid.global_extent[0]+1);
        assert(!pvr_compute_package_input_valid(&package,bad.input,package.input_bytes));
        assert(!pvr_compute_package_valid(&bad));
    }
    if(package.profile==2) {
        struct pvr_compute_package over_capacity=package;
        memset(over_capacity.input,0,sizeof(over_capacity.input));put(over_capacity.input,0,1024);
        over_capacity.count=1024;over_capacity.input_header_bytes=4;over_capacity.input_element_bytes=1;
        over_capacity.output_element_bytes=1;over_capacity.input_bytes=1028;over_capacity.output_bytes=1024;
        over_capacity.dispatch_count=32;
        assert(!pvr_compute_package_valid(&over_capacity));
        unsigned groups=package.dispatch_count;
        package.dispatch_count=0;assert(!pvr_compute_package_valid(&package));
        package.dispatch_count=32;assert(!pvr_compute_package_valid(&package));
        package.dispatch_count=groups;package.tile_elements=31;assert(!pvr_compute_package_valid(&package));package.tile_elements=32;
        size_t bytes=config.buffers[PVR_JOB_PDS].bytes;config.buffers[PVR_JOB_PDS].bytes=128;
        struct pvr_compute_job_image preserved=built;assert(!pvr_compute_job_build(&config,&built));assert(!memcmp(&preserved,&built,sizeof(built)));
        config.buffers[PVR_JOB_PDS].bytes=bytes;
    }
    assert(pvr_compute_package_valid(&package));
    runtime();until(PVR_JOB_WAIT_FENCE);put(config.buffers[PVR_JOB_UFO].cpu,0,1);
    put(config.buffers[PVR_JOB_OUTPUT].cpu,64+package.output_bytes,0);assert(tick()==0);assert(tick()==-1);
    assert(job.error==PVR_JOB_RESULT);
    puts("PASS package copied modeled outputs, complete-count bounds and retained guard fault");
#endif
    return 0;
}
