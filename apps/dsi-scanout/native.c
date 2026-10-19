#include "dsi.h"
#include "a733_media_clock_native.h"
#include "a733_memory_uncached.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include <string.h>
struct dsi_application dsi;
int dsi_live(void)
{return dsi.admitted&&board_live(NULL)&&!atomic_load_explicit(&dsi.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.locks.retained,memory_order_acquire)&&a733_smp_counter()/24<dsi.deadline;}
static int now(void *v,uint64_t *n){return v==&dsi&&dsi_live()&&a733_counter_clock_now(&providers.shared.counter,n)==1;}
static int enter(void *v,uint64_t deadline){return v==&dsi&&dsi_live()?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline):-1;}
static void leave(void *v){if(v==&dsi)a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v){if(v==&dsi)(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int mapping(uintptr_t a){return a733_display_native_mapping(a,a,0);}
static int barrier(void *v){return v==&dsi&&dsi_live()&&a733_display_native_barrier()&&dsi_live();}
static int media_read(void *v,uintptr_t a,uint32_t *out)
{return v==&dsi&&dsi_live()&&mapping(a)&&a733_media_clock_native_read(v,a,out)==1&&dsi_live();}
static int media_write(void *v,uintptr_t a,uint32_t value)
{return v==&dsi&&dsi_live()&&mapping(a)&&a733_media_clock_native_write(v,a,value)==1&&dsi_live();}
static int pll_address(uintptr_t a,unsigned write)
{
 if(!write)switch(a){
 case 0x02003588:case 0x0200358c:case 0x0200350c:case 0x02003500:case 0x02003508:case 0x02003510:case 0x02003580:case 0x020035c0:case 0x020035c4:
 case 0x0550000c:case 0x05500020:case 0x05502000:case 0x05502040:case 0x05502044:
 case 0x05508000:case 0x05508010:case 0x05508004:case 0x0550800c:
 case 0x05509000:case 0x05509004:case 0x055090e4:case 0x05509054:case 0x05509058:case 0x0550905c:case 0x05509110:case 0x05509060:case 0x05509064:case 0x05509068:case 0x055090e0:return 1;
 default:break;
 }
 return a==0x05509104||a==0x05509108||a==0x0550910c||a==0x05509124||a==0x05509130||a==0x05509134||a==0x05509138;
}
static int pll_read(void *v,uintptr_t a,uint32_t *out)
{return v==&dsi&&dsi_live()&&pll_address(a,0)&&mapping(a)&&a733_display_native_read(a,out)&&dsi_live();}
static int pll_write(void *v,uintptr_t a,uint32_t value)
{return v==&dsi&&dsi_live()&&pll_address(a,1)&&mapping(a)&&a733_display_native_write(a,value)&&dsi_live();}
/* One owner never releases/changes these pins. Other clients cannot mutate
 * their records; this metadata-only check is safe while the register lock is
 * already held by a producer. It performs no recursive provider locking. */
static int pins_owned(void)
{
 if(!dsi.pins.id||providers.shared.pins.failed)return 0;
 for(unsigned i=0;i<dsi.session.values[DSI_PIN_COUNT];++i){const struct dsi_pin *p=&dsi.session.pins[i];
  const struct a733_pin_state *s=&providers.shared.pins.pins[p->bank][p->pin];
  if(s->id!=dsi.pins.id||s->owner!=dsi.session.owner||memcmp(&s->current,p->desired,3))return 0;
 }return 1;
}
static int authority(void)
{return dsi_live()&&a733_display_native_owner(dsi.handle)&&pins_owned()&&
 a733_rail_status(&providers.shared.rails,&dsi.rail)==A733_RAIL_READY&&
 a733_gpu_resources_status(&providers.shared.gpu,(unsigned)dsi.session.pck_client,&dsi.pck)==A733_GPU_RESOURCE_OK;}
static int media_auth(void *v,const struct a733_media_clock_config *c,const struct a733_clock_token *token,const struct a733_gpu_resource_lease *pck)
{return v==&dsi&&c==&dsi.media.config&&token==&dsi.media.token&&pck==&dsi.media.pck&&authority()&&c->groups==(A733_MEDIA_DE|A733_MEDIA_VO0|A733_MEDIA_IOMMU)&&a733_clock_token_status(&providers.shared.clocks,token)==A733_CLOCK_OK?1:-1;}
static int pll_auth(void *v,const struct a733_display_pll_config *c)
{return v==&dsi&&c==&dsi.pll.config&&authority()&&a733_media_clock_prepared(&dsi.media)&&
 a733_clock_token_status(&providers.shared.clocks,&c->reservation.token)==A733_CLOCK_OK?1:-1;}
static int memory_admit(void *v,const struct a733_iommu_tables *t,uint64_t deadline,struct a733_iommu_admission *a)
{
 if(v!=&dsi||t!=&dsi.iommu.tables||deadline!=dsi.deadline||!authority()||!a733_media_clock_prepared(&dsi.media)||
 a733_clock_token_status(&providers.shared.clocks,&dsi.media.token)!=A733_CLOCK_OK)return -1;
 *a=(struct a733_iommu_admission){.token=dsi.session.authority,.excluded_masters=A733_IOMMU_REAL_MASTERS,
 .powered_masters=0x100,.inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
 /* These facts are authenticated by the exact full-session/entry/physical
  * records, not inferred from inactive registers or this fixed flush sequence. */
 return 1;
}
static int memory_io(void *v,uint64_t token,uint32_t off,uint32_t *value,uint64_t deadline,unsigned write)
{
 if(v!=&dsi||token!=dsi.session.authority||off>=0x20000||(off&3)||deadline!=dsi.deadline||!dsi_live()||!mapping(A733_IOMMU_BASE+off))return -1;
 int r=enter(v,deadline);if(r!=1)return -1;
 int ok=write?a733_display_native_write(A733_IOMMU_BASE+off,*value):a733_display_native_read(A733_IOMMU_BASE+off,value);
 if(!ok||!dsi_live()){retain(v);return -1;}leave(v);return dsi_live()?1:-1;
}
static int memory_read(void *v,uint64_t t,uint32_t o,uint32_t *p,uint64_t d){return memory_io(v,t,o,p,d,0);}
static int memory_write(void *v,uint64_t t,uint32_t o,uint32_t p,uint64_t d){return memory_io(v,t,o,&p,d,1);}
static int publish(void *v,uint64_t token,const void *p,size_t n,uint64_t deadline)
{return v==&dsi&&token==dsi.session.authority&&p==(void *)dsi.table_extent.cpu&&n==32768&&deadline==dsi.deadline&&barrier(v)?1:-1;}
static int scanout_auth(void *v,const struct a733_display_native_config *c,uint64_t deadline)
{return v==&dsi&&c==&dsi.native.config&&deadline==dsi.deadline&&authority()&&
 c->media==&dsi.media&&c->pll==&dsi.pll&&c->iommu==&dsi.iommu&&c->pool==dsi.pool&&
 c->request.owner==dsi.session.owner&&c->request.panel_id==dsi.session.panel_id&&c->request.frame.mapped_bytes==dsi_frame_bytes(&dsi.session)?1:-1;}
static int command_auth(void *v,unsigned controller,unsigned lanes,uint64_t lease)
{return v==&dsi&&controller==1&&lanes==4&&lease==dsi.native.binding.resources.id&&authority()&&
 dsi.native.binding.scanout.phase==DISPLAY_PREPARE_PANEL&&dsi.native.binding.scanout.phy.state==A733_DSI_PHY_READY;}
static int command_io(void *v,uintptr_t a,uint32_t *p,unsigned write)
{
 if(v!=&dsi||a<0x05508000||a>=0x05509000||(a&3)||!mapping(a)||!dsi_live())return 0;
 int r=enter(v,dsi.deadline);if(r!=1)return 0;
 int ok=write?a733_display_native_write(a,*p):a733_display_native_read(a,p);
 if(!ok||!dsi_live()){retain(v);return 0;}leave(v);return dsi_live();
}
static int command_read(void *v,uintptr_t a,uint32_t *p){return command_io(v,a,p,0);}
static int command_write(void *v,uintptr_t a,uint32_t p){return command_io(v,a,&p,1);}
static int panel_steps(unsigned phase)
{
 uint64_t n;if(!now(&dsi,&n)||!authority())return -1;
 if(dsi.panel_wait){if(n<dsi.panel_wait)return 0;dsi.panel_wait=0;}
 if(dsi.panel_index==dsi.session.values[DSI_STEP_COUNT]||dsi.session.steps[dsi.panel_index].phase!=phase)return 1;
 const struct dsi_step *p=&dsi.session.steps[dsi.panel_index];
 if(p->kind==DSI_STEP_DELAY){if(p->value>=dsi.deadline-n)return -1;dsi.panel_wait=n+p->value;++dsi.panel_index;return 0;}
 if(p->kind==DSI_STEP_GPIO){
  const struct dsi_pin *pin=0;for(unsigned i=0;i<dsi.session.values[DSI_PIN_COUNT];++i)if(dsi.session.pins[i].role==p->role)pin=&dsi.session.pins[i];
  if(!pin||a733_pins_write_outputs(&providers.shared.pins,&dsi.pins,pin->bank,1u<<pin->pin,p->value<<pin->pin,dsi.deadline)!=A733_PINS_OK)return -1;
  ++dsi.panel_index;return 0;
 }
 if(phase)return -1;
 if(!dsi.packet_pending){if(!a733_dsi_submit(&dsi.command,p->type,0,dsi.session.payload+p->offset,p->bytes,dsi.deadline))return -1;dsi.packet_pending=1;}
 enum a733_dsi_state s=a733_dsi_poll(&dsi.command);
 if(s==A733_DSI_RETAINED)return -1;
 if(s==A733_DSI_ENGINE_DONE){if(!a733_dsi_reap(&dsi.command))return -1;dsi.packet_pending=0;++dsi.panel_index;}
 return 0;
}
static int panel(void *v,const struct display_request *r,const struct display_ownership *o,const struct a733_dsi_phy *phy,uint64_t deadline)
{
 if(v!=&dsi||r!=&dsi.native.binding.scanout.request||o!=&dsi.native.binding.scanout.ownership||
 phy!=&dsi.native.binding.scanout.phy||deadline!=dsi.deadline||!authority())return -1;
 if(!dsi.command.initialized){struct a733_dsi_config c={{command_auth,command_read,command_write,barrier,now},&dsi,o->resources.id,1,4};if(!a733_dsi_init(&dsi.command,&c))return -1;}
 return panel_steps(0);
}
static int supplies(void)
{
 enum a733_rail_result rr;
 if(!dsi.rail.id){a733_rail_mask_t mask=0;for(unsigned i=0;i<dsi.session.values[DSI_SUPPLY_COUNT];++i)mask|=A733_RAIL_BIT(dsi.session.supplies[i].rail);
 rr=a733_rail_acquire(&providers.shared.rails,dsi.session.owner,mask,0,dsi.deadline,&dsi.rail);
 }else rr=a733_rail_poll(&providers.shared.rails,&dsi.rail);
 if(rr==A733_RAIL_BUSY||rr==A733_RAIL_PENDING)return 0;if(rr!=A733_RAIL_READY)return -1;
 unsigned i=dsi.supply_index;if(i==dsi.session.values[DSI_SUPPLY_COUNT])return 1;
 enum a733_pmic_status pr;
 if(!dsi.supply_request.id){pr=a733_pmic_submit(&providers.shared.pmic,dsi.session.owner,&dsi.rail,dsi.session.supplies[i].rail,dsi.deadline,&dsi.supply_request);return pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY?0:-1;}
 struct a733_pmic_snapshot *s=&dsi.supplies[i];pr=a733_pmic_result(&providers.shared.pmic,&dsi.supply_request,s);
 if(pr==A733_PMIC_PENDING||pr==A733_PMIC_BUSY)return 0;
 if(pr!=A733_PMIC_COMPLETE||s->rail!=dsi.session.supplies[i].rail||s->page_before||s->page_after||s->chip!=3||s->revision||!s->enabled||!s->decoded||s->programmed_uv!=dsi.session.supplies[i].uv)return -1;
 pr=a733_pmic_reap(&providers.shared.pmic,&dsi.supply_request);if(pr==A733_PMIC_BUSY)return 0;if(pr!=A733_PMIC_COMPLETE)return -1;
 dsi.supply_request=(struct a733_pmic_request){0};++dsi.supply_index;return 0;
}
static int setup_pins(void)
{
 struct a733_pin_request_banked requests[16]={0};struct a733_pin_output_condition condition={0};
 for(unsigned i=0;i<dsi.session.values[DSI_PIN_COUNT];++i){const struct dsi_pin *p=&dsi.session.pins[i];
 requests[i]=(struct a733_pin_request_banked){p->bank,p->pin,{p->expected[0],p->expected[1],p->expected[2]}, {p->desired[0],p->desired[1],p->desired[2]},p->output,0,0};
 if(p->expected[0]==1&&p->desired[0]==1){condition.masks[p->bank]|=1u<<p->pin;condition.levels[p->bank]|=(uint32_t)p->output<<p->pin;}}
 enum a733_pins_result r;
 unsigned any=0;for(unsigned i=0;i<A733_PIN_BANKS;++i)any|=condition.masks[i];
 if(any)r=a733_pins_acquire_banked_checked(&providers.shared.pins,dsi.session.owner,requests,dsi.session.values[DSI_PIN_COUNT],&condition,&dsi.pins,dsi.deadline);
 else r=a733_pins_acquire_banked(&providers.shared.pins,dsi.session.owner,requests,dsi.session.values[DSI_PIN_COUNT],&dsi.pins,dsi.deadline);
 return r==A733_PINS_BUSY?0:r==A733_PINS_OK?1:-1;
}
static int allocate(void)
{
 dsi.pool=a733_memory_uncached_init();if(!dsi.pool)return 0;
 if(!device_memory_acquire_aligned(dsi.pool,dsi.session.owner,32768,16384,&dsi.tables,&dsi.table_extent)||
 !device_memory_acquire(dsi.pool,dsi.session.owner,dsi_frame_bytes(&dsi.session),&dsi.frame,&dsi.frame_extent))return 0;
 memset((void *)dsi.frame_extent.cpu,0,dsi.frame_extent.bytes);
 uint32_t *pixels=(void *)dsi.frame_extent.cpu;unsigned w=dsi.session.values[DSI_WIDTH],h=dsi.session.values[DSI_HEIGHT],stride=dsi.session.values[DSI_PITCH]/4;
 static const uint32_t colors[]={0x00ffffff,0x00ffff00,0x0000ffff,0x0000ff00,0x00ff00ff,0x00ff0000,0x000000ff,0};
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)pixels[y*stride+x]=colors[((uint64_t)x*8/w+dsi.session.values[DSI_SEED])&7];
 storage_digest_bytes((void *)dsi.frame_extent.cpu,dsi.frame_extent.bytes,dsi.frame_hash);
 struct a733_iommu_span span={0x10000000,dsi.frame_extent.physical,dsi.frame_extent.bytes,A733_IOMMU_READ};
 struct a733_iommu_tables tables;if(a733_iommu_build(&tables,(void *)dsi.table_extent.cpu,dsi.table_extent.physical,&span,1)!=1||!barrier(&dsi))return 0;
 struct a733_iommu_ops ops={now,memory_admit,memory_read,memory_write,publish};
 if(a733_iommu_init_display(&dsi.iommu,&ops,&dsi,&tables)!=1||a733_iommu_open(&dsi.iommu,dsi.deadline)!=1)return 0;
 device_memory_lookup(dsi.pool,dsi.frame)->state=DEVICE_MEMORY_DEVICE;device_memory_lookup(dsi.pool,dsi.tables)->state=DEVICE_MEMORY_DEVICE;return 1;
}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static int observation(void)
{
 uint64_t n;if(!now(&dsi,&n)||dsi.iommu.fault.scanned_masters!=0x100||!authority())return 0;
 struct display_scanout *s=&dsi.native.binding.scanout;uint8_t *p=dsi.observation;
 memset(p,0,256);put32(p,0x52495344);put32(p+4,1);put32(p+8,256);put32(p+12,1);
 put64(p+16,dsi.session.board.epoch);put64(p+24,dsi.session.board.generation);put64(p+32,dsi.observed_start);put64(p+40,n);
 memcpy(p+48,dsi.profile_hash,32);memcpy(p+80,dsi.frame_hash,32);
 for(unsigned i=0;i<16;++i)put32(p+112+4*i,dsi.session.values[i]);
 put64(p+176,dsi.native.binding.resources.generation);put64(p+184,dsi.frame.generation);put64(p+192,dsi.tables.generation);
 put32(p+200,s->de.raw_status);put32(p+204,s->lcd.debug_status);put32(p+208,s->lcd.irq_status);put32(p+212,s->dsi.debug_status);
 put32(p+216,s->state);put32(p+220,s->phase);put32(p+224,s->de.state);put32(p+228,s->lcd.state);put32(p+232,s->dsi.state);put32(p+236,dsi.iommu.fault.scanned_masters);
 put64(p+240,dsi.frame_extent.bytes);put64(p+248,dsi.session.panel_id);
 if(!dsi_live())return 0;atomic_store_explicit(&dsi.result,1,memory_order_release);return 1;
}
static int step(void)
{
 const uint32_t *v=dsi.session.values;
 switch(dsi.stage){
 case 0:{int r=supplies();if(r!=1)return r;++dsi.stage;return 0;}
 case 1:{int r=setup_pins();if(r!=1)return r;++dsi.stage;return 0;}
 case 2:{struct a733_gpu_power_resources ready;enum a733_gpu_resource_result r=a733_gpu_resources_acquire(&providers.shared.gpu,(unsigned)dsi.session.pck_client,A733_GPU_RESOURCE_PCK,dsi.deadline,&dsi.pck_token,&ready);
 if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;
 dsi.pck=providers.shared.gpu.leases[dsi.session.pck_client];
 struct a733_media_clock_config c={.owner=dsi.session.owner,.handoff=dsi.session.board.generation,.deadline_us=dsi.deadline,.groups=A733_MEDIA_DE|A733_MEDIA_VO0|A733_MEDIA_IOMMU};
 memcpy(c.fabric,dsi.session.board.fabric,sizeof(c.fabric));memcpy(c.rate_hz,dsi.session.board.fabric_hz,sizeof(c.rate_hz));
 const struct a733_media_clock_ops ops={now,media_auth,enter,leave,retain,media_read,media_write};
 if(!a733_media_clock_init(&dsi.media,&c,&ops,&dsi,&providers.shared.clocks,&providers.shared.gpu,(unsigned)dsi.session.pck_client,&dsi.pck))return -1;++dsi.stage;return 0;}
 case 3:{enum a733_media_clock_result r=a733_media_clock_poll(&dsi.media);if(r==A733_MEDIA_PENDING||r==A733_MEDIA_BUSY)return 0;if(r!=A733_MEDIA_PREPARED)return -1;
 struct a733_display_pll_config c={.ops={now,enter,leave,retain,pll_auth,pll_read,pll_write,barrier},.context=&dsi,.registry=&providers.shared.clocks,
 .reservation={dsi.media.token,dsi.session.board.generation,dsi.session.inactive_epoch,dsi.session.reference_generation,dsi.session.rail_generation},
 .initial={.id=dsi.session.pll_profile_id,.generation=dsi.session.pll_profile_generation,.hs_hz=v[16],.ls_hz=v[17]},.hs_hz=v[1],.ls_hz=v[2],.deadline=dsi.deadline};
 memcpy(c.initial.words,v+18,sizeof(c.initial.words));if(!a733_display_pll_init(&dsi.pll,&c))return -1;++dsi.stage;return 0;}
 case 4:{enum a733_display_pll_state r=a733_display_pll_poll(&dsi.pll);if(r==A733_DISPLAY_PLL_PENDING)return 0;if(r!=A733_DISPLAY_PLL_READY_RETAINED||!allocate())return -1;++dsi.stage;return 0;}
 case 5:{int r=a733_iommu_poll(&dsi.iommu);if(!r)return 0;if(r!=1)return -1;
 struct display_request request;if(!dsi_request(&dsi.session,&request,0x10000000,dsi.frame_extent.cpu))return -1;
 struct a733_display_native_config c={.media=&dsi.media,.pll=&dsi.pll,.iommu=&dsi.iommu,.pool=dsi.pool,.frame=dsi.frame,.tables=dsi.tables,
 .request=request,.locks=&providers.shared.locks,.task=dsi.handle,.context=&dsi,.recovery_generation=dsi.session.recovery_generation,.now_us=now,.authorize=scanout_auth,.panel=panel};
 if(!a733_display_native_init(&dsi.native,&c)||!a733_display_native_prepare(&dsi.native,dsi.deadline))return -1;++dsi.stage;return 0;}
 case 6:{enum display_scanout_state r=a733_display_native_poll(&dsi.native);if(r==DISPLAY_SCANOUT_PENDING)return 0;if(r!=DISPLAY_SCANOUT_READY||!a733_display_native_request(&dsi.native,DISPLAY_SCANOUT_START,dsi.deadline))return -1;++dsi.stage;return 0;}
 case 7:{enum display_scanout_state r=a733_display_native_poll(&dsi.native);if(r==DISPLAY_SCANOUT_PENDING)return 0;if(r!=DISPLAY_SCANOUT_ACTIVE)return -1;++dsi.stage;return 0;}
 case 8:{int r=panel_steps(1);if(r!=1)return r;if(!now(&dsi,&dsi.observed_start)||!a733_display_native_request(&dsi.native,DISPLAY_SCANOUT_OBSERVE,dsi.deadline))return -1;++dsi.stage;return 0;}
 case 9:{enum display_scanout_state r=a733_display_native_poll(&dsi.native);if(r==DISPLAY_SCANOUT_PENDING)return 0;if(r!=DISPLAY_SCANOUT_ACTIVE||a733_iommu_scan(&dsi.iommu,dsi.deadline)!=1)return -1;++dsi.stage;return 0;}
 case 10:{int r=a733_iommu_poll(&dsi.iommu);if(!r)return 0;if(r!=1)return -1;
 if(!atomic_load_explicit(&dsi.result,memory_order_acquire)&&!observation())return -1;
 dsi.next_observe=a733_smp_counter()/24+100000;++dsi.stage;return 0;}
 case 11:if(a733_smp_counter()/24>=dsi.next_observe){if(!a733_display_native_request(&dsi.native,DISPLAY_SCANOUT_OBSERVE,dsi.deadline))return -1;dsi.stage=9;}return 0;
 default:return -1;
 }
}
void dsi_task(void *unused)
{
 (void)unused;uint64_t start=board.session.session_start_ticks/24;
 if(start>UINT64_MAX-dsi.session.values[DSI_TIMEOUT])goto fault;
 dsi.deadline=start+dsi.session.values[DSI_TIMEOUT];if(dsi.deadline>=board.session.deadline_us-10000)goto fault;
 for(;;){if(!dsi_live())break;if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&step()<0)break;vTaskDelay(1);}
 fault:
 atomic_store_explicit(&dsi.failed,1,memory_order_release);atomic_store_explicit(&dsi.result,2,memory_order_release);
 if(dsi.pool){if(dsi.frame.generation)(void)device_memory_retain(dsi.pool,dsi.frame);if(dsi.tables.generation)(void)device_memory_retain(dsi.pool,dsi.tables);}
 for(;;)vTaskDelay(1);
}
