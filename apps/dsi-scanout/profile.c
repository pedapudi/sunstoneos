#include "profile.h"
#include <string.h>
static int zero(const void *v,size_t z){const uint8_t *p=v;unsigned n=0;for(size_t i=0;i<z;++i)n|=p[i];return !n;}
uint64_t dsi_frame_bytes(const struct dsi_session *s)
{
 uint64_t used=(uint64_t)s->values[DSI_PITCH]*s->values[DSI_HEIGHT];
 if(used>UINT64_MAX-s->values[DSI_PADDING]-4095)return 0;
 return (used+s->values[DSI_PADDING]+4095)&~UINT64_C(4095);
}
int dsi_request(const struct dsi_session *s,struct display_request *r,uint64_t pa,uintptr_t cpu)
{
 const uint32_t *v=s->values;
 struct display_request q={.owner=s->owner,.board_id=s->board_id,.panel_id=s->panel_id,
 .timing={.dsi={.pixel_hz=v[0],.hs_hz=v[1],.ls_hz=v[2],.width=v[3],.height=v[4],.hsync=v[5],.hback=v[6],.hfront=v[7],.vsync=v[8],.vback=v[9],.vfront=v[10],.lanes=v[11],.format=v[12],.mode=v[13]},.actual_dclk_hz=v[14]},
 .frame={cpu,pa,dsi_frame_bytes(s),s->board.generation,v[15]}};
 for(unsigned i=3;i<=10;++i)if(v[i]>65535)return 0;
 struct a733_de_plan p;struct a733_de_surface f={pa,q.frame.mapped_bytes,s->owner,v[15]};
 if(!a733_de_plan(&q.timing,&f,&p))return 0;*r=q;return 1;
}
int dsi_profile_valid(const struct dsi_session *s)
{
 if(!s||s->magic!=DSI_PROFILE_MAGIC||s->version!=1||s->bytes!=4096||s->board.workload!=2||
 !s->owner||!s->board_id||!s->panel_id||!s->authority||!s->pll_profile_id||!s->pll_profile_generation||
 !s->reference_generation||!s->rail_generation||!s->inactive_epoch||!s->recovery_generation||s->pck_client>=16||
 s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||!zero(s->reserved,sizeof(s->reserved)))return 0;
 for(unsigned i=0;i<4;++i)if(zero(s->facts[i],32))return 0;
 const uint32_t *v=s->values;uint64_t z=dsi_frame_bytes(s);
 if(v[DSI_TIMEOUT]<1000||v[DSI_TIMEOUT]>50000000||!z||z>UINT64_C(0x1000000)-32768||
 !v[DSI_STEP_COUNT]||v[DSI_STEP_COUNT]>DSI_STEPS||v[DSI_PAYLOAD_BYTES]>DSI_PAYLOAD||
 v[DSI_PIN_COUNT]<10||v[DSI_PIN_COUNT]>16||!v[DSI_SUPPLY_COUNT]||v[DSI_SUPPLY_COUNT]>8)return 0;
 struct display_request r;if(!dsi_request(s,&r,0x10000000,0x78008000))return 0;
 struct a733_display_pll_profile p={s->pll_profile_id,s->pll_profile_generation,v[16],v[17],{0}};
 memcpy(p.words,v+18,sizeof(p.words));struct a733_display_pll_plan plan;
 if(!a733_display_pll_plan(&p,v[DSI_HS],v[DSI_LS],&plan))return 0;
 unsigned lanes=0,roles=0;
 for(unsigned i=0;i<16;++i){const struct dsi_pin *p=&s->pins[i];
  if(i>=v[DSI_PIN_COUNT]){if(!zero(p,sizeof(*p)))return 0;continue;}
  if(p->output>1||p->role>3||!zero(p->reserved,sizeof(p->reserved))||
   !a733_pin_config_valid_banked(p->bank,p->pin,(struct a733_pin_config){p->expected[0],p->expected[1],p->expected[2]})||
   !a733_pin_config_valid_banked(p->bank,p->pin,(struct a733_pin_config){p->desired[0],p->desired[1],p->desired[2]}))return 0;
  for(unsigned j=0;j<i;++j)if(p->bank==s->pins[j].bank&&p->pin==s->pins[j].pin)return 0;
  if(!p->role){if(p->bank!=A733_PIN_D||p->pin<10||p->pin>19||p->desired[0]!=4||p->output)return 0;lanes|=1u<<(p->pin-10);}
  else {if(p->bank<A733_PIN_B||p->bank>A733_PIN_K||p->desired[0]!=1||roles&(1u<<p->role))return 0;roles|=1u<<p->role;}
 }
 if(lanes!=1023)return 0;
 unsigned core_supply=0;
 for(unsigned i=0;i<8;++i){const struct dsi_supply *p=&s->supplies[i];
  if(i>=v[DSI_SUPPLY_COUNT]){if(p->rail||p->uv)return 0;continue;}
  if(p->rail>=A733_RAIL_COUNT||!p->uv)return 0;if(p->rail==A733_RAIL_DCDC2)core_supply=1;
  for(unsigned j=0;j<i;++j)if(p->rail==s->supplies[j].rail)return 0;
 }
 if(!core_supply)return 0;
 unsigned packets=0,phase=0;uint64_t waits=0;
 for(unsigned i=0;i<DSI_STEPS;++i){const struct dsi_step *p=&s->steps[i];
  if(i>=v[DSI_STEP_COUNT]){if(!zero(p,sizeof(*p)))return 0;continue;}
  if(p->phase>1||p->phase<phase||p->reserved2)return 0;phase=p->phase;
  if(p->kind==DSI_STEP_GPIO){if(p->role<1||p->role>3||!(roles&(1u<<p->role))||p->value>1||p->offset||p->bytes||p->type)return 0;}
  else if(p->kind==DSI_STEP_DELAY){if(p->role||p->type||p->offset||p->bytes||!p->value)return 0;waits+=p->value;}
  else if(p->kind==DSI_STEP_PACKET){struct a733_dsi_packet packet;
   if(p->phase||p->role||p->value||p->offset>v[DSI_PAYLOAD_BYTES]||p->bytes>v[DSI_PAYLOAD_BYTES]-p->offset||
    !a733_dsi_encode(&packet,p->type,0,s->payload+p->offset,p->bytes))return 0;
   ++packets;
  }else return 0;
 }
 return packets&&waits<v[DSI_TIMEOUT]&&zero(s->payload+v[DSI_PAYLOAD_BYTES],DSI_PAYLOAD-v[DSI_PAYLOAD_BYTES]);
}
