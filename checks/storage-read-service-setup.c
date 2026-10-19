/* Preflight the actual sealed-session parser and native MMC config geometry.
 * No device callbacks, old test bodies or hardware accesses are executed. */
#include "storage_read_session.h"
#include "a733_mmc_platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
 struct storage_read_session s;assert(fread(&s,1,sizeof(s),f)==sizeof(s)&&fgetc(f)==EOF);fclose(f);
 assert(storage_read_session_valid(&s));
 struct a733_mmc_platform_config c={.owner=s.owner,.handoff=s.authority,.topology=s.board.topology,
  .deadline_us=1000000+s.identify_us,.lifetime_deadline_us=60990000,.kind=SUNXI_MMC_SD,.voltage_mask=s.voltage_mask,.sd_profile=s.sd_profile};
 memcpy(c.fabric,s.board.fabric,sizeof(c.fabric));memcpy(c.fabric_hz,s.board.fabric_hz,sizeof(c.fabric_hz));
 memcpy(c.expected,s.expected,sizeof(s.expected));memcpy(c.desired,s.desired,sizeof(s.desired));
 a733_clock_mask_t scopes=0;assert(a733_mmc_platform_scopes(&c,&scopes));
 assert((scopes&A733_CLOCK_FABRIC)==A733_CLOCK_FABRIC);
 puts("PASS actual session and native MMC configuration preflight; no MMIO");
}
