/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PCK/reset/clock sequences adapted from pinned pck600_domains.c,
 * ccu-sun60iw2.c and sunxi_tcon.c. See apps/displayport/sources.json.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#include "application.h"
#include "aarch64_device_memory.h"
#include "a733_clock_reference.h"
#include <string.h>
#define app dp_app
#define n (dp_app.native)
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x0706a000)
#define EXCLUSIVE (A733_CLOCK_VO1|A733_CLOCK_COMB0|A733_CLOCK_SERDES_COMMON)
#define SHARED (A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC)
#define RAILS (A733_RAIL_BIT(A733_RAIL_DCDC2)|A733_RAIL_BIT(A733_RAIL_CLDO5))
static int intact(void)
{return app.initialized&&displayport_admitted_session()&&
 !memcmp(&app.session,displayport_admitted_session(),sizeof(app.session));}
int dp_current(void)
{return xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==app.handles[0];}
int dp_now(void *v,uint64_t *out)
{(void)v;uint64_t t;if(!intact()||!dp_current()||!board_live(NULL)||!out||a733_counter_clock_now(&providers.shared.counter,&t)!=1||t<n.last_now||t>=app.deadline)return 0;n.last_now=t;*out=t;return 1;}
static int timely(void){uint64_t t;return dp_now(NULL,&t)&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire);}
/* Retry only read-only authority observations. No device operation or partial
 * controller sequence is repeated, and the original deadline remains binding. */
int dp_clock_live(const struct a733_clock_token *token)
{
 for(;;){
  enum a733_clock_result r=a733_clock_token_status(&providers.shared.clocks,token);
  if(r!=A733_CLOCK_BUSY)return r==A733_CLOCK_OK;
  if(!timely())return 0;taskYIELD();
 }
}
static int tokens(void)
{
 return intact()&&!n.failed&&n.clock.id&&n.clock.owner==app.session.owner&&n.clock.shared==SHARED&&n.clock.exclusive==EXCLUSIVE&&
 dp_clock_live(&n.clock);
}
int dp_live(void)
{return tokens()&&board_live(NULL)&&!n.failed;}
void dp_retain(void)
{
 n.failed=1;
 if(n.held&&a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK)==1)n.held=0;
 if(n.clock.id)(void)a733_clock_retain(&providers.shared.clocks,&n.clock);
 if(n.rail.id)(void)a733_rail_retain(&providers.shared.rails,&n.rail);
 a733_display_framebuffer_retain(&dp_app.memory);
}
static int bad(void){dp_retain();return -1;}
static int address(uintptr_t a,unsigned width)
{
 if((width!=2&&width!=4)||(a&(width-1)))return 0;
 if(width==2)return a>=0x06c80000&&a<0x06ca0000;
 return (a>=CCU&&a<CCU+0x2000)||(a>=PCK&&a<PCK+0x1000)||
 (a>=A733_DP_BASE&&a<A733_DP_BASE+0x1000)||(a>=A733_DP_PAD_BASE&&a<A733_DP_PAD_BASE+0x20)||
 (a>=A733_DP_TOP_BASE&&a<A733_DP_TOP_BASE+0x1000)||(a>=A733_DP_TV_BASE&&a<A733_DP_TV_BASE+0x1000)||
 (a>=0x06c01000&&a<0x06c02000)||(a>=0x06c06000&&a<0x06c08000)||
 a==0x07090160||a==0x0709016c||a==0x070101ac;
}
static int mapped(uintptr_t a)
{struct aarch64_translation r,w;return aarch64_device_translate(a,0,&r)&&aarch64_device_translate(a,1,&w)&&r.physical==a&&w.physical==a&&r.attributes==0&&w.attributes==0;}
static int enter(void)
{
 if(!tokens()||!timely()||n.held)return 0;
 int r=a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,app.deadline);
 if(r!=1)return 0;n.held=1;
 if(!timely())return bad();return 1;
}
static int leave(void)
{if(!timely())return bad();a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);n.held=0;return timely();}
static int raw_read(uintptr_t a,unsigned width,uint32_t *out)
{
 if(!out||!n.held||!address(a,width)||!mapped(a)||!timely())return 0;
 __asm__ volatile("dsb sy":::"memory");*out=width==2?*(volatile const uint16_t *)a:*(volatile const uint32_t *)a;
 __asm__ volatile("dsb sy":::"memory");return timely();
}
static int raw_write(uintptr_t a,unsigned width,uint32_t v)
{
 if(!n.held||!address(a,width)||!mapped(a)||!timely())return 0;
 __asm__ volatile("dsb sy":::"memory");if(width==2)*(volatile uint16_t *)a=(uint16_t)v;else *(volatile uint32_t *)a=v;
 __asm__ volatile("dsb sy":::"memory");return timely();
}
static int read_io(void *v,uintptr_t a,unsigned w,uint32_t *out)
{(void)v;if(enter()!=1)return 0;if(!raw_read(a,w,out)){bad();return 0;}return leave();}
static int write_io(void *v,uintptr_t a,unsigned w,uint32_t value)
{(void)v;if(enter()!=1)return 0;if(!raw_write(a,w,value)){bad();return 0;}return leave();}
static int update(uintptr_t a,uint32_t mask,uint32_t value)
{
 uint32_t old,got;if(enter()!=1)return 0;
 if(!raw_read(a,4,&old)||!raw_write(a,4,(old&~mask)|value)||!raw_read(a,4,&got)||got!=((old&~mask)|value)){bad();return 0;}
 return leave();
}
static int observed(uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return read_io(NULL,a,4,&v)&&((v&mask)==expected);}
static int barrier(void *v)
{(void)v;if(!tokens()||!timely())return 0;__asm__ volatile("dsb sy":::"memory");return timely();}
static int wait_io(void *v,uint32_t us,uint64_t deadline)
{
 (void)v;uint64_t start,t;if(deadline!=app.deadline||!dp_now(NULL,&start)||us>deadline-start)return 0;
 do {if(us>=1000)vTaskDelay(1);else taskYIELD();if(!dp_now(NULL,&t))return 0;}while(t-start<us);
 return 1;
}
static int supplies(void)
{
 for(;;){
  enum a733_rail_result rail=a733_rail_status(&providers.shared.rails,&n.rail);
  if(rail!=A733_RAIL_BUSY&&rail!=A733_RAIL_READY)return 0;
  if(rail==A733_RAIL_BUSY)goto busy;
  enum a733_pmic_status pmic=a733_pmic_live(&providers.shared.pmic,&n.pmic_lifetime);
  if(pmic!=A733_PMIC_BUSY&&pmic!=A733_PMIC_READY)return 0;
  if(pmic==A733_PMIC_BUSY)goto busy;
  enum a733_gpu_resource_result gpu=a733_gpu_resources_status(&providers.shared.gpu,app.session.pck_client,&n.pck_lease);
  if(gpu!=A733_GPU_RESOURCE_BUSY)return gpu==A733_GPU_RESOURCE_OK;
 busy:
  if(!timely())return 0;taskYIELD();
 }
}
int dp_native_source(void *v,unsigned starting)
{
 (void)v;if(!tokens()||!timely()||!supplies()||!n.token.generation||!n.token.hpd)return 0;
 for(;;){int valid=a733_typec_dp_validate(&app.typec.provider,&n.token,app.deadline);
  if(valid<0)return 0;if(valid==1)break;if(!timely())return 0;taskYIELD();}
 if(starting==1)return n.tv_prepared&&a733_display_framebuffer_source(&app.memory,1)==1;
 return starting==0;
}
/* Reserve the complete COMB0/SERDES lifecycle before PD Configure can switch
 * the partner. USB3 and PCIe sharing the reset/clock group remain excluded. */
int dp_native_reserve(void)
{
 if(!intact()||!dp_current()||!timely()||n.failed)return -1;
 if(n.clock.id)return tokens()?1:bad();
 enum a733_clock_result r=a733_clock_acquire(&providers.shared.clocks,app.session.owner,SHARED,EXCLUSIVE,app.deadline,&n.clock);
 if(r==A733_CLOCK_BUSY)return 0;return r==A733_CLOCK_OK?1:bad();
}
static int supply(unsigned index)
{
 const enum a733_rail ids[]={A733_RAIL_DCDC2,A733_RAIL_CLDO5};
 const uint32_t uv[]={800000,1800000};enum a733_pmic_status s;
 if(!n.requests[index].id){s=a733_pmic_submit(&providers.shared.pmic,app.session.owner,&n.rail,ids[index],app.deadline,&n.requests[index]);
  if(s==A733_PMIC_BUSY||s==A733_PMIC_PENDING)return 0;if(s!=A733_PMIC_READY)return -1;}
 s=a733_pmic_result(&providers.shared.pmic,&n.requests[index],&n.supplies[index]);
 if(s==A733_PMIC_BUSY||s==A733_PMIC_PENDING)return 0;
 return s==A733_PMIC_COMPLETE&&n.supplies[index].chip==3&&n.supplies[index].rail==ids[index]&&!n.supplies[index].page_before&&!n.supplies[index].page_after&&!n.supplies[index].revision&&n.supplies[index].enabled&&n.supplies[index].decoded&&n.supplies[index].programmed_uv==uv[index]?1:-1;
}
static int baseline(void)
{
 if(!observed(0x07090160,UINT32_MAX,app.session.oscillator)||!observed(CCU,UINT32_MAX,app.session.reference)||
 !observed(CCU+0x140,UINT32_MAX,app.session.video1_pll)||!observed(CCU+0x148,1u<<31,0)||
 !observed(CCU+0x14c,0x0e000000,0)||!observed(CCU+0x13c0,UINT32_MAX,app.session.serdes_cfg)||
 !observed(0x0709016c,UINT32_MAX,app.session.rtc_serdes0))return 0;
 uint32_t control,status;if(!read_io(NULL,PCK,4,&control)||!read_io(NULL,PCK+8,4,&status)||
 (control&15)!=(status&15)||((control&15)!=0&&(control&15)!=8))return 0;
 n.domain_on=(control&15)==8;
 const unsigned bus[]={0x16cc,0x1604,0x160c,0x164c,0x168c};
 for(unsigned i=0;i<sizeof(bus)/sizeof(*bus);i++)if(!observed(CCU+bus[i],1,0))return 0;
 if(!observed(CCU+0x1640,1u<<31,0)||!observed(CCU+0x1690,1u<<31,0))return 0;
 return 1;
}
static int power_prepare(void)
{
 const unsigned resets[]={0x16ec,0x16cc,0x160c,0x164c};
 for(unsigned i=0;i<4;i++)if(!update(CCU+resets[i],1u<<16,0))return 0;
 if(!wait_io(NULL,10,app.deadline))return 0;
 if(!n.domain_on){const unsigned off[]={0x170,0x174,0xc00,0xc04,0xc10};const uint32_t val[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
  for(unsigned i=0;i<5;i++)if(!update(PCK+off[i],UINT32_MAX,val[i]))return 0;}
 if(!update(PCK,15,8))return 0;
 unsigned ready=0;for(unsigned i=0;i<20;i++){if(observed(PCK+8,15,8)){ready=1;break;}if(!wait_io(NULL,500,app.deadline))return 0;}
 if(!ready)return 0;
 if(!update(CCU+0x5c0,(1u<<4)|(1u<<8),(1u<<4)|(1u<<8))||!update(CCU+0x13c0,0x8700001f,0x80000000)||
 !update(0x0709016c,16,16)||!update(CCU+0x13c4,1u<<16,1u<<16))return 0;
 for(unsigned i=0;i<4;i++)if(!update(CCU+resets[i],1u<<16,1u<<16))return 0;
 if(!update(CCU+0x1640,0x87001f1f,0x8100000f)||!update(CCU+0x16cc,1,1)||!update(CCU+0x160c,1,1)||!update(CCU+0x164c,1,1))return 0;
 /* Dedicated VIDEO1 branch No-Auto and enable are adopted, not rewritten.
  * Register output distribution is checked by the qualified profile and
  * expected live PLL image; no separate measurement is inferred. */
 return 1;
}
static int tv_prepare(void)
{
 /* A733 manual15.2 strict720p60 onepixel, DE1 source, TV1->eDP route.
  * DE1 selection is exclusively the DE mux, value5; TV source is DE. */
 const struct {unsigned off;uint32_t mask,value;} r[]={
 {0x90,0x800ffff0,25u<<4},{0,0x31,0},{0x40,7,0},
 {0x98,0x8001ffff,1500},{0x9c,0x7fff7fff,(1279u<<16)|719},
 {0xa0,0xffff7fff,(1649u<<16)|259},{0xa4,0x7fff,24},
 {0xa8,0x0fff0fff,(39u<<16)|4},{0x88,0x0f000000,0x07000000},
 {0x8c,0x0f000000,0x0f000000},{0x300,1u<<31,0}};
 for(unsigned i=0;i<sizeof(r)/sizeof(*r);i++)if(!update(A733_DP_TV_BASE+r[i].off,r[i].mask,r[i].value))return 0;
 if(!update(A733_DP_TOP_BASE,1u<<5,1u<<5)||!update(A733_DP_TOP_BASE+0x20,1u<<21,1u<<21))return 0;
 n.tv_prepared=1;return 1;
}
int dp_native_prepare(void)
{
 if(!tokens()||!timely())return bad();int r;
 switch(n.stage){
 case 0:{enum a733_rail_result s=a733_rail_acquire(&providers.shared.rails,app.session.owner,RAILS,0,app.deadline,&n.rail);if(s==A733_RAIL_BUSY)return 0;if(s!=A733_RAIL_PENDING)return bad();++n.stage;return 0;}
 case 1:{enum a733_rail_result s=a733_rail_poll(&providers.shared.rails,&n.rail);if(s==A733_RAIL_PENDING||s==A733_RAIL_BUSY)return 0;if(s!=A733_RAIL_READY)return bad();++n.stage;return 0;}
 case 2:{enum a733_pmic_status s=a733_pmic_identity(&providers.shared.pmic,&n.pmic_lifetime);if(s==A733_PMIC_BUSY)return 0;if(s!=A733_PMIC_READY)return bad();s=a733_pmic_live(&providers.shared.pmic,&n.pmic_lifetime);if(s==A733_PMIC_BUSY||s==A733_PMIC_PENDING)return 0;if(s!=A733_PMIC_READY)return bad();++n.stage;return 0;}
 case 3:case 4:r=supply(n.stage-3);if(r<0)return bad();if(r)++n.stage;return 0;
 case 5:n.pck_lease.mask=A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK;n.pck_lease.mode=A733_GPU_RESOURCE_STABLE;
  {enum a733_gpu_resource_result s=a733_gpu_resources_acquire(&providers.shared.gpu,app.session.pck_client,n.pck_lease.mask,app.deadline,&n.pck_lease.token,&n.pck_ready);if(s==A733_GPU_RESOURCE_BUSY)return 0;if(s!=A733_GPU_RESOURCE_OK)return bad();}++n.stage;return 0;
 case 6:if(!dp_native_source(NULL,0)||!baseline()||!power_prepare()||!tv_prepare())return bad();
  {struct a733_dp_config c={.ops={dp_native_source,read_io,write_io,barrier,dp_now,wait_io},.context=&n,.owner=app.session.owner,.deadline=app.deadline,.polarity=n.token.polarity,.apb_hz=app.session.apb_hz};
   if(!a733_dp_init(&n.controller,&c)||!a733_dp_prepare(&n.controller))return bad();}
  ++n.stage;return 1;
 case 7:return dp_native_source(NULL,0)?1:bad();
 default:return bad();
 }
}
int dp_native_start(void)
{if(!dp_native_source(NULL,1)||!update(A733_DP_TV_BASE+0x90,1u<<31,1u<<31)||!a733_dp_start(&n.controller))return bad();n.tv_started=1;return 1;}
int dp_native_stop(void)
{if(!a733_dp_stop(&n.controller)||!update(A733_DP_TV_BASE+0x90,1u<<31,0))return bad();n.tv_started=0;return 1;}
