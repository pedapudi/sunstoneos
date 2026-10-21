/* Application composition. Initial register and PMIC wire fixtures are reused;
 * actual media/PLL/pins/PMIC/allocator/IOMMU/scanout controllers execute. */
#include "../apps/dsi-scanout/dsi.h"
#define DISPLAY_NATIVE_REUSE
#include "display-clock-handoff.c"
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include <setjmp.h>
static struct pmic_model pm;
struct board_application board;struct board_provider providers;
static struct dsi_session input;
static struct device_memory_pool memory_pool;
static void *backing;
static uint32_t iommu_words[0x20000/4];
static unsigned owner_token,calls,fail_mode,injected,pll_yields,pll_ticks;
static uint64_t first_yield,last_yield;
static jmp_buf done;
const struct dsi_session *dsi_admitted_session(void){return &input;}
uint64_t a733_smp_counter(void){return pm.now*24;}
int board_live(void *unused){(void)unused;return pm.now<60000000;}
int a733_counter_clock_now(void *v,uint64_t *out){assert(v==&providers.shared.counter);*out=++pm.now;return 1;}
TaskHandle_t xTaskCreateStatic(TaskFunction_t entry,const char *name,configSTACK_DEPTH_TYPE depth,void *argument,UBaseType_t priority,StackType_t *stack,StaticTask_t *task)
{assert(entry==dsi_task&&!strcmp(name,"dsi")&&depth==8192&&!argument&&priority==4&&stack==dsi.stack&&task==&dsi.task);return (void *)&owner_token;}
int a733_display_native_owner(void *task){return task==&owner_token;}
int a733_display_native_mapping(uintptr_t cpu,uint64_t pa,unsigned attr)
{if(!attr)return cpu==pa;return attr==0x44&&cpu>=(uintptr_t)backing&&cpu<(uintptr_t)backing+0x1000000&&pa==0x78000000+cpu-(uintptr_t)backing;}
int a733_platform_lock_enter(struct a733_platform_locks *l,enum a733_platform_lock which,uint64_t deadline)
{assert(l==&providers.shared.locks&&which==A733_PLATFORM_REGISTER_LOCK);if(pm.register_held||pm.retained||pm.now>=deadline)return 0;pm.register_held=1;return 1;}
void a733_platform_lock_leave(struct a733_platform_locks *l,enum a733_platform_lock which)
{assert(l==&providers.shared.locks&&which==A733_PLATFORM_REGISTER_LOCK&&pm.register_held);pm.register_held=0;}
int a733_platform_lock_retain(struct a733_platform_locks *l,enum a733_platform_lock which)
{assert(l==&providers.shared.locks&&which==A733_PLATFORM_REGISTER_LOCK&&pm.register_held);pm.retained=1;atomic_store(&l->retained,1);return 1;}
int a733_display_native_enter(struct a733_platform_locks *l,uint64_t deadline){return a733_platform_lock_enter(l,A733_PLATFORM_REGISTER_LOCK,deadline);}
void a733_display_native_leave(struct a733_platform_locks *l){a733_platform_lock_leave(l,A733_PLATFORM_REGISTER_LOCK);}
void a733_display_native_retain(struct a733_platform_locks *l){(void)a733_platform_lock_retain(l,A733_PLATFORM_REGISTER_LOCK);}
int a733_display_native_barrier(void){return 1;}
static uint32_t *word(uintptr_t a)
{if(a>=A733_IOMMU_BASE&&a<A733_IOMMU_BASE+0x20000)return &iommu_words[(a-A733_IOMMU_BASE)/4];return source_word(a);}
int a733_display_native_read(uintptr_t a,uint32_t *out)
{
 assert(pm.register_held&&!pm.retained);++calls;
 if(a==0x05508010&&dsi.native.binding.scanout.phase==DISPLAY_PREPARE_PANEL)m.host[4]&=~1u;
 *out=*word(a);return 1;
}
int a733_display_native_write(uintptr_t a,uint32_t value)
{
 assert(pm.register_held&&!pm.retained);++calls;*word(a)=value;
 if(fail_mode==3&&dsi.stage==6&&!injected){injected=1;pm.now=dsi.deadline;}
 return 1;
}
int a733_media_clock_native_read(void *v,uintptr_t a,uint32_t *out){assert(v==&dsi&&pm.register_held);*out=*source_word(a);++calls;return 1;}
int a733_media_clock_native_write(void *v,uintptr_t a,uint32_t value){assert(v==&dsi&&pm.register_held);*source_word(a)=value;++calls;return 1;}
struct device_memory_pool *a733_memory_uncached_init(void){return &memory_pool;}
void vTaskDelay(TickType_t ticks)
{
 /* A zero delay is a yield while the PLL holds the register mutex. */
 if(!ticks){assert(dsi.stage==4&&dsi.pll.held&&pm.register_held);if(!pll_yields++)first_yield=pm.now;last_yield=pm.now;return;}
 pll_ticks+=dsi.pll.held;
 assert(ticks==1);pm.now+=1000;
 if(!injected&&fail_mode==1&&dsi.stage==6){injected=1;dsi.iommu.tables.spans[0].physical+=4096;}
 if(!injected&&fail_mode==2&&dsi.stage==10){injected=1;iommu_words[0x10088/4]=1;}
 if(atomic_load(&dsi.result)){longjmp(done,1);}
 assert(pm.now<60000000);
 pm.worker=1;enum a733_pmic_status r=a733_pmic_poll(&providers.shared.pmic);pm.worker=0;
 assert(r!=A733_PMIC_RETAINED);
}
static int gpu_read_model(void *v,uintptr_t a,uint32_t *out){assert(v==&pm);*out=*source_word(a);return 1;}
static int gpu_update_model(void *v,uintptr_t a,uint32_t required,uint32_t value,uint64_t deadline)
{assert(v==&pm&&pm.now<deadline);uint32_t *p=source_word(a);assert((*p&required)==required);*p|=value;return 1;}
static int gpu_rail_model(void *v,struct a733_gpu_resource_rail *out,uint64_t deadline){(void)v;(void)out;(void)deadline;assert(0);return 0;}
static void application_setup(void)
{
 /* Exact initial producer words from checks/display-clock-handoff.c, before
  * any media/PLL preparation. They are MODEL_ONLY, never a launch profile. */
 memset(&n,0,sizeof(n));memset(&m,0,sizeof(m));n.pck=1;n.osc=0x10000002;
 n.ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);n.ccu[0xa0/4]=0xfe000000u|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
 n.ccu[0x2e0/4]=0xf4000000u|(99u<<8)|(3u<<16);n.ccu[0x1908/4]=0x0fff0fff;n.ccu[0x1988/4]=0x0fff0000;
 n.ccu[0x1928/4]=0x00030003;n.ccu[0x19a8/4]=0x00030000;
 n.ccu[0x500/4]=0x03000003;n.ccu[0x580/4]=0x83000001;n.ccu[0x588/4]=0x81000001;
 n.ccu[0x5c0/4]=0xff;n.ccu[0x5e0/4]=0x41055803;n.ccu[0x5e4/4]=0x40302;
 for(unsigned i=0;i<10;++i)n.domains[i][0]=n.domains[i][2]=8;
 m.top[3]=0x20;m.phy[0xe0/4]=1u<<26;
 const unsigned offsets[]={0x104,0x108,0x10c,0x124,0x130,0x134,0x138};for(unsigned i=0;i<7;++i)m.phy[offsets[i]/4]=input.values[18+i];
 m.de[0x8000/4]=m.de[0x8004/4]=0x10010;m.de[0x8008/4]=0x11;
 pmic_model_setup(&pm);pm.now=100;pm.pmic[0x10]|=2;pm.pmic[0x13]=30;
 assert(a733_clock_leases_init(&providers.shared.clocks,pmic_model_now,&pm));
 const struct a733_gpu_resource_ops go={pmic_model_now,gpu_read_model,gpu_update_model,gpu_rail_model};assert(a733_gpu_resources_init(&providers.shared.gpu,&go,&pm));
 const struct a733_rail_binding rb={&providers.shared.clocks,&providers.shared.gpu,900,pmic_model_now,&pm};assert(a733_rail_leases_init(&providers.shared.rails,&rb));
 struct a733_pin_handoff handoff={0};handoff.authorized[A733_PIN_L]=3;
 assert(board_application_pins(&handoff));
 const struct a733_pins_operations po={pmic_model_pin_enter,pmic_model_pin_leave,pmic_model_now,pmic_model_pin_read,pmic_model_pin_write};
 assert(a733_pins_init_banks(&providers.shared.pins,&po,&pm,&handoff));
 for(unsigned i=0;i<input.values[DSI_PIN_COUNT];++i){const struct dsi_pin *p=&input.pins[i];struct a733_pin_fields f;assert(a733_pin_fields_banked(p->bank,p->pin,&f));
 *pmic_model_word(&pm,f.mux_address)|=(uint32_t)p->expected[0]<<f.mux_shift;*pmic_model_word(&pm,f.pull_address)|=(uint32_t)p->expected[1]<<f.pull_shift;*pmic_model_word(&pm,f.drive_address)|=(uint32_t)p->expected[2]<<f.drive_shift;}
 struct a733_pmic_config pc=pm.provider.config;pc.startup_deadline=50000000;
 const struct a733_pmic_ops ops={pmic_model_now,pmic_model_current,pmic_model_enter,pmic_model_leave,pmic_model_retain,pmic_model_read,pmic_model_write,pmic_model_outside};
 assert(a733_pmic_init(&providers.shared.pmic,&pc,&ops,&pm,&providers.shared.clocks,&providers.shared.rails,&providers.shared.pins));
 for(unsigned i=0;i<100;++i){pm.worker=1;enum a733_pmic_status r=a733_pmic_poll(&providers.shared.pmic);pm.worker=0;pm.now+=10;if(r==A733_PMIC_READY)break;assert(r!=A733_PMIC_RETAINED);}
 atomic_store(&providers.shared.pmic_ready,1);assert(board_application_start());
 assert(posix_memalign(&backing,16384,0x1000000)==0);assert(device_memory_pool_init(&memory_pool,(uintptr_t)backing,0x78000000,0x1000000));
}

#ifndef DSI_NATIVE_REUSE
int a733_display_native_rcq_submit(void){assert(!"version-1 sessions never replay");return -1;}
int main(int argc,char **argv)
{
 assert(argc==4);FILE *f=fopen(argv[1],"rb");assert(f&&fread(&input,1,sizeof(input),f)==sizeof(input)&&!fclose(f));assert(dsi_profile_valid(&input));
 if(!strcmp(argv[2],"mapping"))fail_mode=1;else if(!strcmp(argv[2],"iommu"))fail_mode=2;else if(!strcmp(argv[2],"late"))fail_mode=3;else if(!strcmp(argv[2],"nolock"))fail_mode=4;else assert(!strcmp(argv[2],"success"));
 board.admitted_session=input.board;board.session=input.board;board.session.session_start_ticks=0;board.session.deadline_us=60000000;
 application_setup();if(fail_mode==4)m.phy[0xe0/4]=0;if(!setjmp(done))dsi_task(0);
 /* A PLL that never locks keeps the mutex until the deadline, but the task
  * yields only for the first 5 ms of the hold and then sleeps a tick per poll. */
 if(fail_mode==4){assert(atomic_load(&dsi.result)==2&&!dsi.pool&&pm.register_held&&dsi.stage==4);
 assert(pll_yields>100&&last_yield-first_yield<5000&&pll_ticks>1000);}
 else if(fail_mode){assert(atomic_load(&dsi.result)==2);assert(device_memory_lookup(&memory_pool,dsi.frame)->state==DEVICE_MEMORY_RETAINED&&device_memory_lookup(&memory_pool,dsi.tables)->state==DEVICE_MEMORY_RETAINED);}
 else {assert(atomic_load(&dsi.result)==1&&dsi.native.binding.transferred&&dsi.iommu.state==A733_IOMMU_ACTIVE&&dsi.iommu.fault.scanned_masters==0x100);
 /* No tick passes while the PLL holds the common register mutex. */
 assert(pll_yields>100&&!pll_ticks);
 assert(dsi.media.config.groups==(A733_MEDIA_DE|A733_MEDIA_VO0|A733_MEDIA_IOMMU));assert(device_memory_lookup(&memory_pool,dsi.frame)->state==DEVICE_MEMORY_DEVICE);
 f=fopen(argv[3],"wb");assert(f&&fwrite(dsi.observation,1,256,f)==256&&!fclose(f));}
 printf("PASS native application %s %ux%u; actual producers/PMIC/pins/IOMMU/scanout; callbacks%u; physical authority modeled\n",argv[2],input.values[3],input.values[4],calls);free(backing);return 0;
}

#endif
