#include "sunstone_display.h"
#include "a733_display_modes.h"
#include "a733_display_engine.h"
#include "a733_hdmi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t fixture[SUNSTONE_DISPLAY_MAX_EDID];
static size_t fixture_bytes;
static unsigned connected=1,pending,owner_reads;
static uint64_t generation=1;
static int detect(void *v,uint64_t d,unsigned *c,uint64_t *g)
{(void)v;assert(d==1000);*c=connected;*g=generation;return 1;}
static int read_edid(void *v,uint64_t d,uint8_t *p,size_t n,size_t *bytes)
{(void)v;assert(d==1000&&n>=fixture_bytes);memcpy(p,fixture,fixture_bytes);*bytes=fixture_bytes;return 1;}
static enum sunstone_display_mode_status hdmi_valid(void *v,const struct sunstone_display_mode *m)
{(void)v;enum a733_hdmi_mode out;return a733_hdmi_mode_from_pixel(m,&out);}
static int step(void *v,const struct sunstone_panel_step *s,uint64_t d)
{(void)v;assert(s&&d==1000);if(pending){pending=0;return 0;}pending=1;return 1;}
static int owner_now(void *v,uint64_t *out){(void)v;*out=100;return 1;}
static int owner_validate(void *v,const struct a733_hdmi_request *r,const struct a733_hdmi_reservation *p,uint64_t d)
{(void)v;(void)r;(void)p;return d==1000;}
static int owner_read8(void *v,uintptr_t address,uint8_t *out)
{(void)v;++owner_reads;assert(address==A733_HDMI_BASE+0x3004);*out=connected?2:0;return 1;}
static void load(const char *path)
{FILE *f=fopen(path,"rb");assert(f);fixture_bytes=fread(fixture,1,sizeof(fixture),f);assert(feof(f));fclose(f);}
static void checksum(uint8_t *p){p[127]=0;unsigned sum=0;for(unsigned i=0;i<127;++i)sum+=p[i];p[127]=(uint8_t)-sum;}
int main(int argc,char **argv)
{
 assert(argc==5);
 struct sunstone_display_modes modes,unchanged;
 for(int i=1;i<argc;++i){
  load(argv[i]);memset(&modes,0xa5,sizeof(modes));
  int parsed=sunstone_display_edid_parse(fixture,fixture_bytes,&modes);
  if(parsed!=1)fprintf(stderr,"EDID %d parse %d\n",i,parsed);
  assert(parsed==SUNSTONE_EDID_OK);
  assert(modes.count>0&&!modes.truncated);
  unsigned detailed4k=0;for(size_t j=0;j<modes.count;++j){assert(sunstone_display_mode_valid(&modes.modes[j]));detailed4k|=modes.modes[j].width>=3840;}
  assert(detailed4k);printf("public EDID %d: %zu modes, skipped %u, unknown %u\n",i,modes.count,modes.skipped_timings,modes.unknown_extensions);
  unchanged=modes;fixture[127]^=1;
  assert(sunstone_display_edid_parse(fixture,fixture_bytes,&modes)==SUNSTONE_EDID_CHECKSUM&&!memcmp(&modes,&unchanged,sizeof(modes)));
  fixture[127]^=1;
  assert(sunstone_display_edid_parse(fixture,fixture_bytes-1,&modes)==SUNSTONE_EDID_LENGTH);
  if(fixture_bytes>128)assert(sunstone_display_edid_parse(fixture,128,&modes)==SUNSTONE_EDID_LENGTH);
 }
 load(argv[3]);assert(fixture_bytes==384&&fixture[256]==0x70);
 assert(sunstone_display_edid_parse(fixture,fixture_bytes,&modes)==1);
 /* This real Acer dump carries DisplayID I timings at 3840x2160/120 Hz.
  * Clearing its other detailed/CTA modes leaves the DisplayID inventory. */
 uint8_t original[SUNSTONE_DISPLAY_MAX_EDID];memcpy(original,fixture,fixture_bytes);
 memset(fixture+54,0,72);checksum(fixture);memset(fixture+128,0,128);fixture[128]=2;fixture[129]=3;checksum(fixture+128);
 assert(sunstone_display_edid_parse(fixture,fixture_bytes,&modes)==1&&modes.count==2);
 assert(modes.modes[0].width==3840&&modes.modes[0].height==2160);
 fixture[258]=122;checksum(fixture+256);
 unchanged=modes;assert(sunstone_display_edid_parse(fixture,fixture_bytes,&modes)==SUNSTONE_EDID_BLOCK&&!memcmp(&modes,&unchanged,sizeof(modes)));
 memcpy(fixture,original,fixture_bytes);fixture[259]^=1;checksum(fixture+256);
 assert(sunstone_display_edid_parse(fixture,fixture_bytes,&modes)==SUNSTONE_EDID_BLOCK);
 load(argv[2]);struct sunstone_display_port port={.connector=SUNSTONE_DISPLAY_HDMI};
 struct sunstone_display_connector_state state={.port=&port,.ops={detect,read_edid,hdmi_valid}};
 uint8_t scratch[SUNSTONE_DISPLAY_MAX_EDID];
 assert(sunstone_display_get_modes(&state,1000,scratch,sizeof(scratch),&modes));
 struct sunstone_display_mode selected;
 assert(sunstone_display_choose(&state,&modes,0,&selected)==SUNSTONE_MODE_OK);
 assert(selected.width==1920||selected.width==1280);
 struct sunstone_display_mode too_large=modes.modes[0];
 assert(sunstone_display_choose(&state,&modes,&too_large,&selected)==SUNSTONE_MODE_UNSUPPORTED_PROFILE);
 generation++;assert(sunstone_display_detect(&state,1000));
 assert(sunstone_display_choose(&state,&modes,0,&selected)==SUNSTONE_MODE_LINK);
 connected=0;assert(sunstone_display_detect(&state,1000)&&state.changed);
 assert(!sunstone_display_get_modes(&state,1000,scratch,sizeof(scratch),&modes));connected=1;
 /* Actual HDMI connector adapter samples native HPD and carries its owned,
  * complete EDID to generic enumeration without changing its clock profile. */
 struct a733_hdmi owner={0};owner.self=&owner;owner.io.self=&owner.io;
 owner.state=A733_HDMI_PREPARED;owner.io.admitted=1;owner.io.request.mode=A733_HDMI_720P60;
 owner.io.ops.now_us=owner_now;owner.io.ops.validate=owner_validate;owner.io.ops.read8=owner_read8;
 owner.edid_size=(uint16_t)fixture_bytes;memcpy(owner.edid,fixture,fixture_bytes);
 assert(a733_hdmi_connector_init(&state,&port,&owner));
 owner.entered=1;assert(!sunstone_display_detect(&state,1000)&&!owner_reads);owner.entered=0;
 assert(sunstone_display_get_modes(&state,1000,scratch,sizeof(scratch),&modes));
 assert(sunstone_display_choose(&state,&modes,0,&selected)==SUNSTONE_MODE_OK&&selected.width==1280);
 connected=0;assert(sunstone_display_detect(&state,1000));connected=1;
 assert(sunstone_display_detect(&state,1000));
 assert(sunstone_display_choose(&state,&modes,0,&selected)==SUNSTONE_MODE_LINK);
 assert(!sunstone_display_get_modes(&state,1000,scratch,sizeof(scratch),&modes));
 const struct sunstone_panel *panels[]={&sunstone_panel_innolux_p079zca,&sunstone_panel_lg_lh500wx1_sd03};
 for(unsigned i=0;i<2;++i){
  assert(sunstone_panel_valid(panels[i]));
  struct sunstone_panel_sequence sequence={0};const struct sunstone_panel_ops ops={step};
  assert(sunstone_panel_sequence_init(&sequence,panels[i],&ops,0,0,1000));pending=1;
  for(size_t j=0;j<2*panels[i]->prepare_count+1;++j){int r=sunstone_panel_poll(&sequence);assert(r>=0);if(r==1)break;}
  assert(sequence.next==panels[i]->prepare_count&&sunstone_panel_poll(&sequence)==1);
  struct sunstone_panel invalid=*panels[i];invalid.format=(enum sunstone_panel_format)-1;assert(!sunstone_panel_valid(&invalid));
 }
 struct sunstone_display_port dsi={.connector=SUNSTONE_DISPLAY_DSI,.controller=1,.lanes=4,.lane_map={0,1,2,3},.panel=panels[0]};
 struct a733_dsi_video_timing timing;
 assert(a733_dsi_panel_timing(&dsi,panels[0]->modes,341400000,56900000,&timing)==SUNSTONE_MODE_CLOCK);
 dsi.panel=panels[1];assert(a733_dsi_panel_timing(&dsi,panels[1]->modes,402000000,67000000,&timing)==SUNSTONE_MODE_GEOMETRY);
 dsi.panel=0;assert(a733_dsi_panel_timing(&dsi,panels[0]->modes,0,0,&timing)==SUNSTONE_MODE_NO_PANEL);
 struct sunstone_display_mode mode={74250000,1280,720,110,40,220,5,5,20,3};
 uint32_t numerator,denominator;assert(sunstone_display_refresh(&mode,&numerator,&denominator));
 assert(numerator==74250000&&denominator==1650*750);
 struct a733_de_surface surface={0x10000000,1280*720*4,1,1280*4};struct a733_de_plan de;
 assert(a733_de_plan_mode(&mode,4,&surface,&de)&&de.size==((719u<<16)|1279u));
 enum a733_hdmi_mode hdmi;
 assert(a733_hdmi_mode_from_pixel(&mode,&hdmi)==SUNSTONE_MODE_OK&&hdmi==A733_HDMI_720P60);
 assert(a733_dp_mode_valid(&mode)==SUNSTONE_MODE_OK);mode.pixel_hz--;
 assert(a733_hdmi_mode_from_pixel(&mode,&hdmi)==SUNSTONE_MODE_UNSUPPORTED_PROFILE);
 mode.flags|=SUNSTONE_MODE_INTERLACED;assert(!a733_de_plan_mode(&mode,4,&surface,&de));
 puts("display classes: modes, real EDID/DisplayID, malformed bounds, hotplug generations, two panels and exact hardware admission passed");
}
