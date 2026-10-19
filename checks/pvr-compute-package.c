int fixed_fixture_main(int,char **);
#define main fixed_fixture_main
#include "pvr-compute-job.c"
#undef main
static struct pvr_compute_package package;
static void load(const char *path,void *p,size_t n)
{FILE *f=fopen(path,"rb");assert(f&&fread(p,n,1,f)==1&&fgetc(f)==EOF);assert(!fclose(f));}
int main(int argc,char **argv)
{
    assert(argc==3);load(argv[1],&package,sizeof(package));assert(pvr_compute_package_valid(&package));
    setup(0);config.package=&package;
    for(unsigned i=0;i<PVR_JOB_REGIONS;++i)config.buffers[i].bytes=pvr_compute_job_region_bytes(&config,i);
    assert(pvr_compute_job_build(&config,&built));
    assert(!memcmp(config.buffers[PVR_JOB_USC].cpu,package.code,package.code_bytes));
    for(unsigned i=package.code_bytes;i<4096;++i)assert(!((unsigned char *)config.buffers[PVR_JOB_USC].cpu)[i]);
    assert(!memcmp(config.buffers[PVR_JOB_INPUT].cpu,package.input,package.input_bytes));
#ifdef UPSTREAM
    const struct pvr_device_info *dev=&pvr_device_info_36_56_104_183;
    uint32_t data[128]={0};
    struct pvr_pds_shared_storing_program p={.cc_enable=true,.doutw_control={.dest_store=PDS_UNIFIED_STORE,
      .num_const64=2,.doutw_data={config.buffers[PVR_JOB_INPUT].gpu,config.buffers[PVR_JOB_OUTPUT].gpu+64}}};
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
    memcpy((unsigned char *)config.buffers[PVR_JOB_OUTPUT].cpu+64,expected,package.output_bytes);
    put(config.buffers[PVR_JOB_UFO].cpu,0,1);assert(tick()==0);assert(tick()==1);
    assert(!memcmp(job.results,expected,package.output_bytes));
    unsigned before=package.count;package.count=UINT32_MAX;assert(!pvr_compute_package_valid(&package));package.count=before;
    ++package.input_bytes;assert(!pvr_compute_package_valid(&package));--package.input_bytes;
    ++package.output_bytes;assert(!pvr_compute_package_valid(&package));--package.output_bytes;
    package.precomp[2]=1;assert(!pvr_compute_package_valid(&package));package.precomp[2]=0;
    assert(pvr_compute_package_valid(&package));
    runtime();until(PVR_JOB_WAIT_FENCE);put(config.buffers[PVR_JOB_UFO].cpu,0,1);
    put(config.buffers[PVR_JOB_OUTPUT].cpu,64+package.output_bytes,0);assert(tick()==0);assert(tick()==-1);
    assert(job.error==PVR_JOB_RESULT);
    puts("PASS package copied modeled outputs, complete-count bounds and retained guard fault");
#endif
    return 0;
}
