/* Reuse the existing physical-admission/resource model only as constructor
 * setup. No capture poll, sensor workload or prior component case is executed. */
#define main old_camera_component_main
#include "a733-camera-native.c"
#undef main
int main(void)
{
    pm.now=100;
    WORD(0x07090160)=0x10000002;WORD(0x070101ac)=1;
    WORD(0x02002000)=0xf8000000u|(99u<<8)|(99u<<16);
    WORD(0x020020a0)=0xfe000000u|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
    WORD(0x02002120)=0xf8000000u|(53u<<8)|(3u<<20);
    WORD(0x02003908)=0x0fff0fff;WORD(0x02003988)=0x0fff0000;
    WORD(0x02003910)=0x00770077;WORD(0x02003990)=0x00770000;
    WORD(0x02002500)=0x03000003;WORD(0x02002580)=0x83000001;WORD(0x02002588)=0x81000001;
    WORD(0x020025c0)=0x010000ff;WORD(0x020025e0)=0x41055803;WORD(0x020025e4)=0x40302;
    for(unsigned i=0;i<10;i++){WORD(0x07060000+i*0x1000)=8;WORD(0x07060008+i*0x1000)=8;}
    WORD(0x0701019c)=0x10001;WORD(0x07025000)=0x22;WORD(0x07025014)=0x11;WORD(0x07025024)=5;
    WORD(0x0708300c)=0x40;WORD(0x07083010)=0xf8;WORD(0x07083014)=0x28;WORD(0x07083020)=0x30;
    WORD(0x07083200)=0xf81000;WORD(0x07083210)=0x80c0;WORD(0x07083218)=0x100010;
    pm.pmic[0x0e]=3;pm.pmic[0x10]=0xff;pm.pmic[0x21]=0xff;
    pm.pmic[0x12]=23;pm.pmic[0x19]=100;pm.pmic[0x2e]=13;
    WORD(A733_TWI11_BASE+0x10)=0xf8;WORD(A733_TWI11_BASE+0x20)=0x30;
    WORD(A733_TWI11_BASE+0x200)=0xf81000;WORD(A733_TWI11_BASE+0x210)=0x80c0;WORD(A733_TWI11_BASE+0x218)=0x100010;
    sensor[0x300a]=0x56;sensor[0x300b]=0x40;common[0]=0x360500;common[1]=0xa5;
    for(unsigned i=1;i<=3;i++)common[i*0x100/4]=0x40000;
    locks.initialized=1;locks.now=a733_counter_clock_now;locks.context=&counter;
    locks.mutex[0]=(void *)1;locks.mutex[1]=(void *)2;
    assert(a733_clock_leases_init(&pm.clocks,a733_counter_clock_now,&counter));
    struct a733_gpu_resource_ops gpu={pmic_model_now,generic_read,pmic_model_gpu_write,pmic_model_gpu_rail};
    assert(a733_gpu_resources_init(&pm.gpu,&gpu,&pm));
    struct a733_rail_binding binding={&pm.clocks,&pm.gpu,900,a733_counter_clock_now,&counter};
    assert(a733_rail_leases_init(&pm.rails,&binding));
    struct a733_pins_operations pins={pmic_model_pin_enter,pmic_model_pin_leave,pmic_model_now,pin_read,pin_write};
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;b++)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned i=0;i<2;i++)handoff.latch[i]=(struct a733_pin_latch_handoff){100+i,900,a733_pin_bank_mask(A733_PIN_L+i),0};
    assert(a733_pins_init_banks(&pm.pins,&pins,&pm,&handoff));
    struct a733_pmic_config pc={777,888,1000000,{{2,1,1},{2,1,1}}};
    struct a733_pmic_ops po={pmic_model_now,pmic_model_current,pmic_model_enter,pmic_model_leave,
        pmic_model_retain,pmic_model_read,pmic_model_write,provider_outside};
    assert(a733_pmic_init(&pm.provider,&pc,&po,&pm,&pm.clocks,&pm.rails,&pm.pins));
    assert(a733_pmic_identity(&pm.provider,&pm.lifetime)==A733_PMIC_READY);
    while(a733_pmic_live(&pm.provider,&pm.lifetime)!=A733_PMIC_READY)assert(pmic_model_step(&pm)!=A733_PMIC_RETAINED);
    boot=(struct a733_camera_boot_record){.version=1,.bytes=sizeof(boot),.owner=50,.epoch=2,.generation=3,
        .lease=99,.deadline_us=1000000,.build={1,2,3,4},.evidence={5,6,7,8},.proof=A733_CAMERA_REQUIRED_PROOF,
        .origin=A733_CAMERA_COLD_RESET,.excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=A733_IOMMU_REAL_MASTERS,
        .raw_trim=0xb00,.parameter_guard_bytes=4096,.plane_guard_bytes=4096,.apb1=0,.apb1_hz=24000000,
        .settle=20,.deskew=2,.reset_bank=A733_PIN_G,.reset_pin=10,
        .rails={A733_RAIL_DCDC1,A733_RAIL_DCDC8,A733_RAIL_BLDO5},.programmed_uv={3300000,1800000,1800000}};
    for(unsigned i=0;i<15;i++)boot.inherited[i]=(struct a733_pin_config){15,0,0};
    boot.inherited[11].function=boot.inherited[14].function=1;
    for(unsigned i=0;i<10;i++)pin_config(A733_PIN_K,i,boot.inherited[i]);
    for(unsigned i=10;i<14;i++)pin_config(A733_PIN_E,i-5,boot.inherited[i]);
    pin_config(A733_PIN_G,10,boot.inherited[14]);WORD(A733_PINS_BASE+0x80+A733_PIN_E*0x80+0x10)=1u<<6;
    a733_rail_mask_t rails=0;for(unsigned i=0;i<3;i++)rails|=A733_RAIL_BIT(boot.rails[i]);
    assert(a733_rail_acquire(&pm.rails,boot.owner,rails,0,boot.deadline_us,&supply)==A733_RAIL_PENDING);
    while(a733_rail_poll(&pm.rails,&supply)!=A733_RAIL_READY){}
    uint64_t token;struct a733_gpu_power_resources ready;
    assert(a733_gpu_resources_acquire(&pm.gpu,2,A733_GPU_RESOURCE_PCK,boot.deadline_us,&token,&ready)==A733_GPU_RESOURCE_OK);
    struct a733_gpu_resource_lease lease=pm.gpu.leases[2];
    struct a733_media_clock_config mc={.owner=boot.owner,.handoff=boot.lease,.deadline_us=boot.deadline_us,
        .groups=A733_MEDIA_VI|A733_MEDIA_IOMMU,.camera_mclk_mask=2,
        .fabric={0x03000003,0x83000001,0x81000001},.rate_hz={150000000,300000000,300000000}};
    struct a733_media_clock_ops mo={pmic_model_now,media_auth,pmic_model_pin_enter,pmic_model_pin_leave,
        media_retain,generic_read,generic_write};
    assert(a733_media_clock_init(&media,&mc,&mo,&pm,&pm.clocks,&pm.gpu,2,&lease));
    void *ram=mmap((void *)0x48000000,0x200000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(ram==(void *)0x48000000);assert(device_memory_pool_init(&pool,(uintptr_t)ram,(uintptr_t)ram,0x200000));
    struct a733_camera_native_config config={&boot,&media,&pm.clocks,&pm.pins,&pm.rails,&supply,&pm.provider,&locks,camera_task};

    boot.version=2;boot.proof=A733_CAMERA_SUPERVISED_PROOF;
    struct device_memory_pool prior=pool;
    assert(a733_camera_native_init(&camera,&config)==0 && !camera.self && !memcmp(&pool,&prior,sizeof(pool)));
    boot.proof&=~A733_CAMERA_TRANSACTIONS_DRAINED;
    assert(a733_camera_native_supervised_init(&camera,&config)==0 && !camera.self && !memcmp(&pool,&prior,sizeof(pool)));
    boot.proof=A733_CAMERA_SUPERVISED_PROOF;
    assert(a733_camera_native_supervised_init(&camera,&config)==1);
    assert(camera.capture.analog.required_proof==A733_CAMERA_SUPERVISED_PROOF);
    assert(camera.capture.iommu.camera_supervised==1 && camera.capture.iommu.translated_masters==2 &&
        camera.capture.iommu.accessible_masters==A733_IOMMU_REAL_MASTERS);
    assert(camera.state==A733_CAMERA_NATIVE_CLOCK && !camera.entered && !frames && !cache_syncs);
    assert(!device_memory_release(&pool,camera.memory));
    struct device_memory_lease output;struct device_memory_extent bytes;
    assert(device_memory_acquire(&pool,boot.owner,614400,&output,&bytes));
    assert(bytes.bytes==614400 && bytes.cpu!=camera.extent.cpu);
    puts("PASS explicit supervised native constructor and strict default/prior-drain refusal");
    return 0;
}
