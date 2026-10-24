/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "application.h"
#include "a733_storage_digest.h"
#include <string.h>
#define FRAME_OFFSET 36864u
#define PIXEL_BYTES (1280u*720u*2u)
static int current(void *v){return v==&dp_app&&dp_live()&&dp_current();}
static int output_owned(void *v){return v==&dp_app&&dp_native_source(NULL,0);}
static int token_live(void *v,const struct a733_clock_token *t){return v==&dp_app&&dp_clock_live(t);}
static int admit(void *v,enum a733_de_action action)
{
 enum a733_dp_state h=dp_app.native.controller.result.state;
 if(v!=&dp_app)return 0;
 if(action==A733_DE_PREPARE||action==A733_DE_START)return h==A733_DP_READY;
 if(action==A733_DE_STOP)return h==A733_DP_STOPPED_RETAINED;
 return h==A733_DP_ACTIVE;
}
int dp_memory_boot(void)
{
 if(!a733_displayport_session_valid(&dp_app.session))return 0;
 struct a733_display_framebuffer_config c={.counter=&providers.shared.counter,
  .clocks=&providers.shared.clocks,.locks=&providers.shared.locks,.pck=&providers.shared.gpu,
  .pck_lease=&dp_app.native.pck_lease,.pck_client=dp_app.session.pck_client,.owner=dp_app.session.owner,
  .authority=dp_app.session.authority,.deadline=dp_app.deadline,.parent_bytes=2097152,
  .frame_span=2097152-32768,.output=A733_DE_TV1_RGB565,.context=&dp_app,.current=current,
  .output_owned=output_owned,.token_live=token_live,.admit=admit,
  .domains=&providers.domains,.rail=&dp_app.native.rail};
 memcpy(c.fabric,dp_app.session.board.fabric,sizeof(c.fabric));
 memcpy(c.fabric_hz,dp_app.session.board.fabric_hz,sizeof(c.fabric_hz));
 memcpy(c.iommu_gates,dp_app.session.iommu_gates,sizeof(c.iommu_gates));
 if(!a733_display_framebuffer_boot(&dp_app.memory,&c,1))return 0;
 storage_digest_bytes((const void *)(dp_app.memory.extent.cpu+FRAME_OFFSET),PIXEL_BYTES,dp_app.pixel_digest);
 return 1;
}
int dp_memory_prepare(void)
{return dp_app.native.controller.result.state==A733_DP_READY?a733_display_framebuffer_prepare(&dp_app.memory):-1;}
int dp_memory_stop(void)
{return dp_app.native.controller.result.state==A733_DP_STOPPED_RETAINED?a733_display_framebuffer_stop(&dp_app.memory):-1;}

/* Acknowledge only the source-defined frame-end W1C bit while preserving
 * non-W1C status controls; then require an observed clear before output start.
 * A free-running frame-end can latch between the clear and its readback, so
 * a set readback clears again, at most three times. */
int dp_memory_frame(unsigned clear)
{
 uint32_t status;
 if(!a733_display_framebuffer_ready(&dp_app.memory)||
 !a733_display_framebuffer_read(&dp_app.memory,A733_DE_BASE+0x8144,&status))return -1;
 if(clear){
  for(unsigned attempt=0;attempt<3;++attempt){
   if(!a733_display_framebuffer_write(&dp_app.memory,A733_DE_BASE+0x8144,(status&~13u)|1u)||
      !a733_display_framebuffer_read(&dp_app.memory,A733_DE_BASE+0x8144,&status))return -1;
   if(!(status&1))return 1;
  }
  return -1;
 }
 if(status&2)return -1;return !!(status&1);
}
int dp_memory_guards(void)
{
 if(dp_app.memory.failed||!current(&dp_app)||!a733_display_framebuffer_memory(&dp_app.memory)||
 a733_iommu_validate(&dp_app.memory.tables)!=1)return 0;
 const uint8_t *p=(const void *)dp_app.memory.extent.cpu;
 for(unsigned i=32768;i<FRAME_OFFSET;i++)if(p[i]!=0xa5)return 0;
 for(unsigned i=FRAME_OFFSET+PIXEL_BYTES;i<2097152;i++)if(p[i]!=0xa5)return 0;
 uint8_t hash[32];storage_digest_bytes(p+FRAME_OFFSET,PIXEL_BYTES,hash);
 return !memcmp(hash,dp_app.pixel_digest,32);
}
